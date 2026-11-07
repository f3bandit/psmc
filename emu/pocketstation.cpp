// PocketStation emulator.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
// Each section follows the HDL module named in its heading, from Jamie Blanks' MiSTer core
// (rtl/asic/*.sv); the card port follows this project's rtl/ps_spi.sv. Comments here only note
// how the hardware behaviour was mapped onto software; the behaviour itself is documented in
// those modules and in docs/emulator-spec.md.

#include "pocketstation.hpp"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace ps {

using nba::core::Bus;

static constexpr u32 RATE_HZ[9] = {32768, 63488, 126976, 253952, 507904,
                                   1015808, 1998848, 3997696, 7995392};
static constexpr u64 XTAL_FX = (u64)(MASTER_HZ / CRYSTAL_HZ) << 16;   // 244 master ticks
static constexpr int BUTTON_PULSE_CYCLES = 1023;

PocketStation::PocketStation() {
  for (int i = 0; i < 9; i++) cyc_fx[i] = ((u64)MASTER_HZ << 16) / RATE_HZ[i];
  cpu = std::make_unique<nba::core::arm::ARM7TDMI>(scheduler, bus);
  Reset();
}

bool PocketStation::LoadBios(const std::vector<u8>& image) {
  if (image.size() < 16384) return false;
  for (int i = 0; i < 8192; i++) bootrom[i] = image[2 * i] | image[2 * i + 1] << 8;
  return true;
}

void PocketStation::LoadCard(const std::vector<u8>& image) {
  std::fill(flash.array.begin(), flash.array.end(), 0xFF);
  std::copy_n(image.begin(), std::min<size_t>(image.size(), flash.array.size()), flash.array.begin());
}

void PocketStation::SetClock(u32 clock_bcd, u32 calendar_bcd) {
  rtc.clock = clock_bcd;
  rtc.calendar = calendar_bcd;
}

void PocketStation::Reset() {
  // ps_memctl, ps_timers, ps_clkctl, ps_intc, ps_lcdc, ps_pio, ps_dac, ps_lvd, ps_irda, ps_spi:
  // register reset values. Video memory, SRAM, flash and the RTC counters survive.
  mem = MemCtl{};
  flash.seq = Flash::IDLE; flash.page_valid = 0; flash.delay = 0;
  intc = Intc{};
  tmr = Timers{};
  clk = ClkCtl{};
  rtc.prgsel = rtc.inc_armed = rtc.tick = rtc.resync = false;
  rtc.cntsel = 0; rtc.phase = 0;
  auto vram = lcd.vram;
  lcd = Lcd{};
  lcd.vram = vram;
  lcd.cal = sig.cal & 0x3F;
  pio = Pio{};
  dac = Dac{};
  lvd_ctrl = 3;
  irda_static = 2; irda_dynamic = 0;
  spi = Spi{};
  prefetch_q = ramdata_q = 0;
  cpu->Reset();
}

// ---------------------------------------------------------------------------- savestate --

namespace {
constexpr char STATE_MAGIC[8] = {'M', 'C', 'P', 'S', 'S', 'T', 'A', '1'};

struct Writer {
  std::vector<u8> out;
  template<typename T> void Put(const T& v) {
    static_assert(std::is_trivially_copyable<T>::value, "plain data only");
    const u8* p = reinterpret_cast<const u8*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
  }
};

struct Reader {
  const std::vector<u8>& in;
  size_t pos = 0;
  bool ok = true;
  template<typename T> void Get(T& v) {
    static_assert(std::is_trivially_copyable<T>::value, "plain data only");
    if (pos + sizeof(T) > in.size()) { ok = false; return; }
    std::memcpy(reinterpret_cast<u8*>(&v), in.data() + pos, sizeof(T));
    pos += sizeof(T);
  }
};
}  // namespace

// Everything below is listed in one place for both directions, so the two can't drift apart.
template<typename IO> static void StateFields(PocketStation& m, IO&& io) {
  io(m.t); io(m.next_xtal);
  io(m.sram); io(m.prefetch_q); io(m.ramdata_q);
  io(m.mem); io(m.sig); io(m.intc); io(m.tmr); io(m.clk); io(m.rtc); io(m.lcd);
  io(m.pio); io(m.dac); io(m.lvd_ctrl); io(m.irda_static); io(m.irda_dynamic);
  io(m.spi); io(m.docked);
  io(m.flash.seq); io(m.flash.sector); io(m.flash.page_valid); io(m.flash.page); io(m.flash.delay);
  io(m.scheduler.pending);
  m.cpu->StateFields(io);
}

std::vector<u8> PocketStation::SaveState() const {
  Writer w;
  for (char ch : STATE_MAGIC) w.Put(ch);
  StateFields(const_cast<PocketStation&>(*this), [&](auto& v) { w.Put(v); });
  return w.out;
}

bool PocketStation::LoadState(const std::vector<u8>& data) {
  if (data.size() < 8 || std::memcmp(data.data(), STATE_MAGIC, 8) != 0) return false;
  // Restore into a copy first, so a short or mismatched file leaves the unit untouched.
  Reader r{data, 8};
  PocketStation probe;
  StateFields(probe, [&](auto& v) { r.Get(v); });
  if (!r.ok || r.pos != data.size()) return false;
  Reader r2{data, 8};
  StateFields(*this, [&](auto& v) { r2.Get(v); });
  cpu->RestoreSpsrPointer();
  host = Host{};
  buttons = 0;
  return true;
}

// --------------------------------------------------------------------------------- time --

void PocketStation::Advance(int n) {
  t += cyc_fx[std::min<int>(clk.freq, 8)] * (u64)n;
  scheduler.AddCycles(n);

  // ps_timers: 16-bit down counters on 2, 32 or 512 system clocks. Expiry latches its
  // interrupt directly (the HDL raises a one-clock pulse that the controller latches).
  static constexpr u32 DIV[4] = {2, 32, 512, 512};
  for (int c = 0; c < 3; c++) {
    if (!tmr.en[c]) continue;
    tmr.presc[c] += n;
    u32 lim = DIV[tmr.div[c]];
    while (tmr.presc[c] >= lim) {
      tmr.presc[c] -= lim;
      if (tmr.value[c] == 0) {
        tmr.value[c] = tmr.load[c];
        // A reload of 0 is how apps park a timer: MGS Integral's sound player leaves timer 2
        // running with reload 0 at the end of every tune, which taken literally is an FIQ
        // every two clocks and the app never runs again. A timer parked at 0 doesn't expire.
        if (tmr.load[c] != 0) intc.latch |= 1 << (c == 2 ? INT_TC2 : INT_TC0 + c);
      } else {
        tmr.value[c]--;
      }
    }
  }

  // ps_intc: a button's latch stands for 1023 system clocks.
  for (int b = 0; b < 6; b++) {
    if (intc.pulse[b] > 0 && (intc.pulse[b] -= n) <= 0) {
      intc.pulse[b] = 0;
      intc.latch &= ~(1 << b);
    }
  }

  while (t >= next_xtal) {
    CrystalTick();
    next_xtal += XTAL_FX;
  }

  SampleInterrupts();
  UpdateCpuLines();
}

// Request levels in Sony's source order. The counters latch on their own (see Advance).
static u16 RequestLevels(const PocketStation& m) {
  u16 req = m.buttons & 0x1F;
  if (m.host.sel_low && m.spi.LinkOn()) req |= 1 << INT_SPI;
  if (m.rtc.tick) req |= 1 << INT_RTC;
  if (m.docked) req |= 1 << INT_BAT;
  // ps_irda rx_level: the demodulated line, active low. While receiving it reads high with no
  // light and low while the far side's emitter is on (Chocobo World times its bits on exactly
  // that: the header is the low phase); low in stand-by and while transmitting.
  if (m.IrListening() && !m.ir_light) req |= 1 << INT_IF;
  return req;
}

void PocketStation::SampleInterrupts() {
  // ps_intc: latch on the rising edge (both edges for the supply line and IR); a button's latch
  // is a pulse that also drops at the release. Sampled on system clocks only, so it holds still
  // through standby and catches up on wake.
  u16 req = RequestLevels(*this);
  u16 rise = req & ~intc.prev;
  u16 fall = ~req & intc.prev;
  intc.latch |= rise | (fall & INT_DOUBLE_MASK);
  intc.latch &= ~(fall & INT_LEVEL_MASK);
  for (int b = 0; b < 6; b++) {
    if (rise & (1 << b)) intc.pulse[b] = BUTTON_PULSE_CYCLES;
    if (fall & (1 << b)) intc.pulse[b] = 0;
  }
  intc.prev = req;
}

void PocketStation::UpdateCpuLines() {
  u16 active = intc.latch & intc.mask;
  cpu->IRQLine() = (active & INT_IRQ_MASK) != 0;
  cpu->FIQLine() = (active & INT_FIQ_MASK) != 0;
}

// ps_rtc helpers: two BCD digits per field.
static u8 BcdInc(u8 v, u8 limit) {
  if (v >= limit) return 0x00;
  if ((v & 0xF) == 9) return (u8)(((v >> 4) + 1) << 4);
  return (u8)(v + 1);
}

static u8 MonthDays(u8 month, u8 year) {
  bool leap = ((year & 0xF) == 0 || (year & 0xF) == 4 || (year & 0xF) == 8) && ((year >> 4) & 1) == 0;
  if ((year & 0xF) == 2 || (year & 0xF) == 6) leap = ((year >> 4) & 1) == 1;
  switch (month) {
    case 0x02: return leap ? 0x29 : 0x28;
    case 0x04: case 0x06: case 0x09: case 0x11: return 0x30;
    default: return 0x31;
  }
}

static void AdvanceSecond(PocketStation::Rtc& r) {
  u8 s = r.clock, mi = r.clock >> 8, h = r.clock >> 16, d = r.clock >> 24;
  u8 dt = r.calendar, mo = r.calendar >> 8, y = r.calendar >> 16;
  s = BcdInc(s, 0x59);
  if (s == 0) {
    mi = BcdInc(mi, 0x59);
    if (mi == 0) {
      h = BcdInc(h, 0x23);
      if (h == 0) {
        d = d >= 7 ? 1 : d + 1;
        if (dt >= MonthDays(mo, y)) {
          dt = 1;
          mo = mo >= 0x12 ? 1 : BcdInc(mo, 0x12);
          if (mo == 1) y = BcdInc(y, 0x99);
        } else {
          dt = BcdInc(dt, 0x99);
        }
      }
    }
  }
  r.clock = (u32)d << 24 | (u32)h << 16 | (u32)mi << 8 | s;
  r.calendar = (u32)y << 16 | (u32)mo << 8 | dt;
}

void PocketStation::CrystalTick() {
  // ps_clkctl: an edge on an armed wake line ends standby, sampled on the crystal.
  u16 level = RequestLevels(*this);
  u16 arm = (intc.mask & INT_WAKE_MASK) | INT_WAKE_ALWAYS;
  u16 rise = level & ~clk.wake_prev, fall = ~level & clk.wake_prev;
  if (clk.standby && ((rise | (fall & INT_DOUBLE_MASK)) & arm)) clk.standby = false;
  clk.wake_prev = level;

  // ps_rtc: square wave, 1 Hz running and 4096 Hz in programming mode; the time advances on
  // the falling edge of the 1 Hz wave, and the first pulse after a mode change is skipped.
  u32 half = rtc.prgsel ? 3 : 16383;
  if (rtc.phase >= half) {
    rtc.phase = 0;
    if (rtc.tick && !rtc.prgsel) {
      if (rtc.resync) rtc.resync = false;
      else AdvanceSecond(rtc);
    }
    rtc.tick = !rtc.tick;
  } else {
    rtc.phase++;
  }

  // ps_flash: the commit about 100 us after the last data write, then about 20 ms busy.
  if (flash.seq == Flash::ARMED) {
    if (flash.delay > 0) flash.delay--;
    else if (flash.page_valid) flash.Commit();
  } else if (flash.seq == Flash::PROGRAM) {
    if (flash.delay > 0) flash.delay--;
    else { flash.seq = Flash::IDLE; flash.page_valid = 0; }
  }

  // ps_dac: the speaker, sampled at the crystal rate. Port 5 high is speaker standby.
  if (audio.size() < 1u << 16) {
    bool on = dac.active && !(pio.out & 0x20);
    int v = dac.level & 0x3FF;
    if (v & 0x200) v -= 0x400;
    audio.push_back(on ? (int16_t)v : 0);
  }
}

void PocketStation::RunFor(double seconds) {
  if (seconds <= 0) return;
  RunUntil(t + (u64)(seconds * MASTER_HZ * 65536.0));
}

void PocketStation::RunUntil(u64 target) {
  while (t < target) {
    if (clk.standby) {
      // ps_clkctl: the system clock stops; only crystal-driven blocks run.
      t = next_xtal;
      CrystalTick();
      next_xtal += XTAL_FX;
      if (!clk.standby) { SampleInterrupts(); UpdateCpuLines(); }
      continue;
    }
    cpu->Run();
  }
}

// -------------------------------------------------------------------------- outside world --

void PocketStation::SetButtons(u8 mask) { buttons = mask & 0x1F; }

void PocketStation::SetDocked(bool d) { docked = d; }

void PocketStation::SpiAck() {
  if (host.sel_low) host.acked = true;
}

CardResult PocketStation::CardCommand(const std::vector<u8>& tx) {
  // The console side of ps_snac_link: 250 kHz, a byte every 32 us, the unit answers with the
  // byte it loaded beforehand and acknowledges each byte it wants continued. Same timing as the
  // Verilator harness (sim/sim_main.cpp): 10 us select setup, up to 4 ms for an ack, 65 us
  // gaps through the header and 20 us through data, 10 us hold.
  auto us = [](double v) { return (u64)(v * MASTER_HZ / 1e6) << 16; };
  auto run_until = [&](u64 when, bool stop_on_ack) {
    while (t < when) {
      if (stop_on_ack && host.acked) return;
      if (clk.standby) { t = next_xtal; CrystalTick(); next_xtal += XTAL_FX; SampleInterrupts(); UpdateCpuLines(); continue; }
      cpu->Run();
    }
  };

  CardResult res;
  if (!docked) docked = true;
  host.sel_low = true;
  host.acked = false;
  spi.released = false;
  spi.ready = false;
  run_until(t + us(10), false);

  for (size_t i = 0; i < tx.size(); i++) {
    bool last = i + 1 == tx.size();
    u8 out = (spi.mode & 1) && spi.LinkOn() ? spi.tx : 0xFF;   // what the unit shifts out
    run_until(t + us(32), false);                                  // eight bits
    spi.rx = tx[i];
    spi.ready = true;
    res.rx.push_back(out);
    if (last) { res.complete = true; break; }
    host.acked = false;
    run_until(t + us(4000), true);
    if (!host.acked) break;
    run_until(t + us(i < 6 ? 65 : 20), false);
  }

  run_until(t + us(10), false);
  host.sel_low = false;
  host.acked = false;
  spi.released = true;
  SampleInterrupts();
  UpdateCpuLines();
  return res;
}

void PocketStation::Frame(u8 out[32][32]) const {
  // ps_lcdc: on when DISON, CPEN and the band gap (LVD bit 0 clear) are all on; ROT turns the
  // picture 180 degrees; DISMOD blanks 8-row segments. Bit 0 of a row is the leftmost pixel.
  bool on = lcd.dison && lcd.cpen && !(lvd_ctrl & 1);
  for (int y = 0; y < 32; y++) {
    int seg = y >> 3;
    bool seg_on;
    switch (lcd.dismod) {
      case 0: seg_on = true; break;
      case 1: seg_on = seg == 0; break;
      case 2: seg_on = seg == 1; break;
      case 3: seg_on = seg == 2; break;
      case 4: seg_on = seg == 3; break;
      case 5: seg_on = seg <= 1; break;
      case 6: seg_on = seg == 1 || seg == 2; break;
      default: seg_on = seg >= 2; break;
    }
    u32 row = lcd.vram[lcd.rot ? 31 - y : y];
    for (int x = 0; x < 32; x++) {
      int bit = lcd.rot ? 31 - x : x;
      out[y][x] = on && seg_on ? (row >> bit) & 1 : 0;
    }
  }
}

// ------------------------------------------------------------------------- ps_memctl --

bool PocketStation::MemCtl::Translate(u32 virt, u32& phys) const {
  // Indexed by physical block, holding the virtual block: a search by value, lowest wins.
  if (!flashvir) return false;
  for (u32 p = 0; p < 16; p++)
    if ((active >> p & 1) && vaddr[p] == virt) { phys = p; return true; }
  return false;
}

bool PocketStation::MemCtl::Multiple() const {
  for (int a = 0; a < 16; a++)
    for (int b = a + 1; b < 16; b++)
      if ((active >> a & 1) && (active >> b & 1) && vaddr[a] == vaddr[b]) return true;
  return false;
}

u32 PocketStation::MemctlRead(u32 off, bool side_effects) {
  if (off >= 0x100 && off < 0x140) return mem.vaddr[(off >> 2) & 15];
  switch (off) {
    case 0x000: return (mem.flashvir ? 2 : 0) | (mem.genrem ? 1 : 0);
    case 0x004: {
      u32 v = (mem.st_fabort << 3) | (mem.st_dabort << 2) | (mem.st_noactive << 1) | mem.st_multiple;
      if (side_effects) mem.st_multiple = mem.st_noactive = mem.st_dabort = mem.st_fabort = false;
      return v;
    }
    case 0x008: return mem.active;
    case 0x00C: return mem.boot_wait;
    case 0x010: return (mem.fdc & 0x7B) | (flash.Busy() ? 0 : 4);
#ifdef PS_TRACE_SERIAL
    case 0x300: if (side_effects) fprintf(stderr, "[serial] lo read pc=%08x lr=%08x t=%.3f val=%04x%04x active=%04x vaddr=%x,%x,%x,%x virt=%d\n", cpu->state.r15, cpu->state.r14, Now(), sig.serial_hi, sig.serial_lo, mem.active, mem.vaddr[0], mem.vaddr[1], mem.vaddr[2], mem.vaddr[3], mem.flashvir); return sig.serial_lo;
    case 0x302: if (side_effects) fprintf(stderr, "[serial] hi read pc=%08x\n", cpu->state.r15); return sig.serial_hi;
#else
    case 0x300: return sig.serial_lo;
    case 0x302: return sig.serial_hi;
#endif
    case 0x308: return sig.cal;
    case 0x304: case 0x306: case 0x30A: case 0x30C: case 0x30E: return 0xFFFF;
    default: return 0;
  }
}

static u32 Merge(u32 old_value, u32 value, u8 be) {
  for (int b = 0; b < 4; b++)
    if (be >> b & 1) old_value = (old_value & ~(0xFFu << 8 * b)) | (value & 0xFFu << 8 * b);
  return old_value;
}

void PocketStation::MemctlWrite(u32 off, u32 value, u8 be) {
  if (off >= 0x100 && off < 0x140) {
    if (!mem.flashvir && (be & 1)) mem.vaddr[(off >> 2) & 15] = value & 15;
    return;
  }
  switch (off) {
    case 0x000:
      if (be & 1) {
        if (value & 1) mem.genrem = true;   // one way
        if (value & 2) {
          bool multiple = mem.Multiple(), noactive = mem.active == 0;
          mem.flashvir = !multiple && !noactive;
          mem.st_multiple = multiple;
          mem.st_noactive = noactive;
        } else {
          mem.flashvir = false;
        }
      }
      break;
    case 0x008: if (!mem.flashvir) mem.active = Merge(mem.active, value, be) & 0xFFFF; break;
    case 0x00C: mem.boot_wait = Merge(mem.boot_wait, value, be) & 0x1F; break;
    case 0x010: {
      u8 was = mem.fdc;
      mem.fdc = Merge(mem.fdc, value, be) & 0x7F;
      // Step 6: ENPRG or LOADPAGE cleared after the data writes commits the page now.
      bool ended = ((was & 1) && !(mem.fdc & 1)) || ((was & 0x20) && !(mem.fdc & 0x20));
      if (ended && flash.seq == Flash::ARMED && flash.page_valid) flash.delay = 0;
      break;
    }
    default: break;
  }
}

// --------------------------------------------------------------------------- ps_flash --

void PocketStation::Flash::Commit() {
  // Erase and program act on the whole sector: halfwords software did not write read FFFF.
  for (int i = 0; i < 64; i++) {
    u16 v = (page_valid >> i & 1) ? page[i] : 0xFFFF;
    array[sector * 128 + i * 2] = v & 0xFF;
    array[sector * 128 + i * 2 + 1] = v >> 8;
  }
  seq = PROGRAM;
  delay = 655;
  dirty = true;
}

// The page commits once software ends the sequence (clears ENPRG or LOADPAGE, step 6 of Sony's
// sequence) or, for software that only polls BUSY, after a quiet spell with no data writes.
// ps_flash uses about 100 us for that spell, measured in crystal ticks; at the slow CPU rates
// (MGS Integral saves at 63 kHz, ~16 us a clock) the program loop is slower than that between
// halfwords, so the page committed after its first halfword and the rest read erased. The real
// part clearly waits longer; 64 ticks (about 2 ms) still settles promptly.
static constexpr int FLASH_SETTLE_TICKS = 64;

void PocketStation::FlashWrite(u32 h, u16 v) {
  constexpr u32 KEY_A = 0x2AD5, KEY_B = 0x152A;   // 080055AA, 08002A54
  bool enprg = mem.fdc & 1, lock = mem.fdc & 2, loadpage = mem.fdc & 0x20, loadsgn = mem.fdc & 0x40;
  bool prog = enprg && loadpage && !lock;
  bool sign = enprg && loadsgn && !lock;
  auto& f = flash;
  switch (f.seq) {
    case Flash::IDLE:
      if (prog && h == KEY_A && v == 0xFFAA) f.seq = Flash::KEY1;
      break;
    case Flash::KEY1:
      f.seq = (h == KEY_B && v == 0xFF55) ? Flash::KEY2 : Flash::IDLE;
      break;
    case Flash::KEY2:
      if (h == KEY_A && v == 0xFFA0) { f.seq = Flash::ARMED; f.page_valid = 0; f.delay = FLASH_SETTLE_TICKS; }
      else f.seq = Flash::IDLE;
      break;
    case Flash::ARMED:
      if (sign && (h >> 7) == 0) {
        // ps_signature: the low offsets stand in for the signature while LOADSGN is armed.
        switch (h & 0x7F) {
          case 0x00: sig.serial_lo = v; break;
          case 0x01: sig.serial_hi = v; break;
          case 0x04: sig.cal = v; break;
        }
        f.delay = FLASH_SETTLE_TICKS;
      } else if (!f.page_valid) {
        f.sector = h >> 6;
        f.page[h & 63] = v;
        f.page_valid |= 1ull << (h & 63);
        f.delay = FLASH_SETTLE_TICKS;
      } else if ((h >> 6) == f.sector) {
        f.page[h & 63] = v;
        f.page_valid |= 1ull << (h & 63);
        f.delay = FLASH_SETTLE_TICKS;
      } else {
        f.Commit();   // a write outside the sector ends the window
      }
      break;
    case Flash::PROGRAM:
      break;
  }
}

// ---------------------------------------------------------------- device registers --

u32 PocketStation::DevRead(u32 addr, bool side_effects) {
  u32 top = (addr >> 24) & 0xF, hi = (addr >> 23) & 1;
  switch (top) {
    case 0xA:
      if (!hi) {   // ps_intc
        switch ((addr >> 2) & 7) {
          case 0: return intc.latch;
          case 1: return RequestLevels(*this);
          case 2: return intc.mask;
          default: return 0;
        }
      } else {     // ps_timers
        u32 ch = (addr >> 4) & 3, which = (addr >> 2) & 3;
        if (ch == 3) return 0;
        switch (which) {
          case 0: return tmr.load[ch];
          case 1: return tmr.value[ch];
          case 2: return tmr.en[ch] << 2 | tmr.div[ch];
          default: return 0;
        }
      }
    case 0xB:
      if (!hi) {   // ps_clkctl: PMFrequency with LOCK
        return ((addr >> 2) & 7) == 0 ? (1u << 4) | clk.freq : 0;
      } else {     // ps_rtc: offsets past the registers read the clock
        u32 r = (addr & 0x7FFFF0) ? 2 : (addr >> 2) & 3;
        switch (r) {
          case 0: return (u32)rtc.cntsel << 1 | rtc.prgsel;
          case 2: return rtc.clock;
          case 3: return rtc.calendar;
          default: return 0;
        }
      }
    case 0xC:
      if (!hi) {   // ps_spi (rtl/ps_spi.sv)
        switch ((addr >> 2) & 7) {
          case 0: return spi.mode;
          case 1: return (spi.released ? 2 : 0) | (spi.ready ? 1 : 0);
          case 2: return spi.rx;
          case 4: return spi.ctrl1;
          case 5: return spi.ready;
          case 6: return spi.ctrl2;
          default: return 0;
        }
      } else {     // ps_irda
        switch ((addr >> 2) & 3) {
          case 0: return irda_static;
          case 1: return irda_dynamic;
          default: return 0;
        }
      }
    case 0xD:
      if (!hi) {   // ps_lcdc
        u32 off = addr & 0xFFF;
        if (off == 0) return (u32)lcd.rot << 7 | lcd.dison << 6 | lcd.fr << 4 | lcd.cpen << 3 | lcd.dismod;
        if (off == 4) return lcd.cal;
        if (off >= 0x100 && off < 0x180) return lcd.vram[(off >> 2) & 31];
        return 0;
      } else {     // IO: ps_pio, ps_dac, ps_lvd
        u32 off = addr & 0xFF;
        if (off < 0x10) {
          if (((off >> 2) & 3) == 0) return pio.dir;
          if (((off >> 2) & 3) == 3) {
            u8 pins = ((pio.out & pio.dir) | ~pio.dir) & 0xF;   // undriven pins read high
            return (u32)(pio.out & 0x60) | (docked ? 0x10 : 0) | pins;
          }
          return 0;
        }
        if (off < 0x20) return ((off >> 2) & 3) == 0 ? dac.active : 0;
        if (off == 0x20) return lvd_ctrl;
        return 0;
      }
    default:
      return 0;
  }
}

void PocketStation::DevWrite(u32 addr, u32 v, u8 be) {
  u32 top = (addr >> 24) & 0xF, hi = (addr >> 23) & 1;
  switch (top) {
    case 0xA:
      if (!hi) {   // ps_intc: writing zero does nothing
        switch ((addr >> 2) & 7) {
          case 2: intc.mask |= v & 0x3FFF; break;
          case 3: intc.mask &= ~v; break;
          case 4: intc.latch &= ~v; break;
        }
        UpdateCpuLines();
      } else {     // ps_timers: 16-bit registers, the upper half of a store dropped
        u32 ch = (addr >> 4) & 3;
        if (ch == 3) break;
        switch ((addr >> 2) & 3) {
          case 0:
            tmr.load[ch] = v & 0xFFFF;
#ifdef PS_TRACE_TIMER
            if (ch == 2) fprintf(stderr, "[tmr2 %p] load %04x pc=%08x t=%.4f\n", (void*)this, v & 0xFFFF, cpu->state.r15, Now());
#endif
            if (!tmr.en[ch]) { tmr.value[ch] = v & 0xFFFF; tmr.presc[ch] = 0; }
            break;
          case 2:
            if ((v & 4) && !tmr.en[ch]) tmr.presc[ch] = 0;
            tmr.div[ch] = v & 3;
            tmr.en[ch] = (v >> 2) & 1;
#ifdef PS_TRACE_TIMER
            if (ch == 2) fprintf(stderr, "[tmr2 %p] ctrl %x pc=%08x t=%.4f\n", (void*)this, v & 7, cpu->state.r15, Now());
#endif
            break;
        }
      }
      break;
    case 0xB:
      if (!hi) {   // ps_clkctl
        switch ((addr >> 2) & 7) {
          case 0: clk.freq = v & 0xF; break;
          case 1: if (v & 1) clk.standby = true; break;
        }
      } else {     // ps_rtc
        u32 r = (addr & 0x7FFFF0) ? 2 : (addr >> 2) & 3;
        if (r == 0) {
          if ((bool)(v & 1) != rtc.prgsel) { rtc.phase = 0; rtc.tick = false; rtc.resync = true; }
          rtc.prgsel = v & 1;
          rtc.cntsel = (v >> 1) & 7;
        } else if (r == 1 && (v & 1)) {
          if (!rtc.inc_armed) { rtc.inc_armed = true; break; }
          rtc.inc_armed = false;
          u8 d = rtc.clock >> 24;
          switch (rtc.cntsel) {
            case 0: rtc.clock = (rtc.clock & ~0xFFu) | BcdInc(rtc.clock, 0x59); break;
            case 1: rtc.clock = (rtc.clock & ~0xFF00u) | BcdInc(rtc.clock >> 8, 0x59) << 8; break;
            case 2: rtc.clock = (rtc.clock & ~0xFF0000u) | BcdInc(rtc.clock >> 16, 0x23) << 16; break;
            case 3: rtc.clock = (rtc.clock & 0xFFFFFF) | (u32)(d >= 7 ? 1 : d + 1) << 24; break;
            case 4: {
              u8 dt = rtc.calendar;
              dt = dt >= MonthDays(rtc.calendar >> 8, rtc.calendar >> 16) ? 1 : BcdInc(dt, 0x99);
              rtc.calendar = (rtc.calendar & ~0xFFu) | dt;
              break;
            }
            case 5: {
              u8 mo = rtc.calendar >> 8;
              mo = mo >= 0x12 ? 1 : BcdInc(mo, 0x12);
              rtc.calendar = (rtc.calendar & ~0xFF00u) | (u32)mo << 8;
              break;
            }
            case 6: rtc.calendar = (rtc.calendar & ~0xFF0000u) | BcdInc(rtc.calendar >> 16, 0x99) << 16; break;
          }
        }
      }
      break;
    case 0xC:
      if (!hi) {   // ps_spi (rtl/ps_spi.sv)
        switch ((addr >> 2) & 7) {
          case 0:
            // Setting MODE bit 1 acknowledges the byte just received, and consumes it.
            if ((v & 2) && !(spi.mode & 2)) { SpiAck(); spi.ready = false; }
            spi.mode = v & 7;
            break;
          case 2: spi.tx = v & 0xFF; break;
          case 4: spi.ctrl1 = v & 3; break;
          case 6:
            spi.ctrl2 = v & 3;
            if (v & 1) spi.ready = false;
            break;
        }
      } else {     // ps_irda
        switch ((addr >> 2) & 3) {
          case 0: irda_static = v & 0xF; break;
          case 1: irda_dynamic = v & 1; break;
        }
      }
      break;
    case 0xD:
      if (!hi) {   // ps_lcdc
        u32 off = addr & 0xFFF;
        if (off == 0 && (be & 1)) {
          lcd.dismod = v & 7; lcd.cpen = (v >> 3) & 1; lcd.fr = (v >> 4) & 3;
          lcd.dison = (v >> 6) & 1; lcd.rot = (v >> 7) & 1;
        } else if (off == 4 && (be & 1)) {
          lcd.cal = v & 0x3F;
        } else if (off >= 0x100 && off < 0x180 && be == 0xF) {
          lcd.vram[(off >> 2) & 31] = v;   // word writes only
        }
      } else {     // IO
        u32 off = addr & 0xFF;
        if (off < 0x10) {   // ps_pio: byte stores accepted
          if (!(be & 1)) break;
          switch ((off >> 2) & 3) {
            case 0: pio.dir = v & 0xF; break;
            case 1: pio.out |= v & 0x7F; break;
            case 2: pio.out &= ~v & 0x7F; break;
          }
        } else if (off < 0x20) {   // ps_dac
          switch ((off >> 2) & 3) {
            case 0: dac.active = v & 1; break;
            case 1: dac.level = (v >> 6) & 0x3FF; break;
          }
        } else if (off == 0x20) {  // ps_lvd
          lvd_ctrl = v & 7;
        }
      }
      break;
  }
}

} // namespace ps

// ------------------------------------------------------------------------------ ps_asb --
// The bus: region decode, cost in system clocks, 16-bit devices assembled into words, the
// narrow-read garbage of the wide-only devices, and aborts.

namespace nba::core {

using ps::PocketStation;

enum Region { R_SRAM, R_BOOT, R_VFLASH, R_FLASH, R_MEMCTL, R_DEV, R_FAULT };

struct Decoded { Region r; u32 phys_half = 0; };

static Decoded Decode(PocketStation& m, u32 addr) {
  u32 top = addr >> 24;
  if (top == 0x01 || (top == 0x00 && m.mem.genrem)) return {R_SRAM};
  if (top == 0x04 || (top == 0x00 && !m.mem.genrem)) return {R_BOOT};
  if (top == 0x02) {
    u32 phys;
    if (((addr >> 17) & 0x7F) == 0 && m.mem.Translate((addr >> 13) & 15, phys))
      return {R_VFLASH, phys << 12 | ((addr >> 1) & 0xFFF)};
    return {R_FAULT};
  }
  if (top == 0x08) return {R_FLASH, (addr >> 1) & 0xFFFF};
  if (top == 0x06) return {R_MEMCTL};
  if (top >= 0x0A && top <= 0x0D) return {R_DEV};
  return {R_FAULT};
}

static int Cost(PocketStation& m, Region r, bool thumb_fetch) {
  switch (r) {
    case R_SRAM: case R_MEMCTL: return 1;
    case R_BOOT: return (thumb_fetch ? 1 : 2) + ((m.mem.boot_wait >> 4) & 1);
    case R_FLASH: case R_VFLASH: return (thumb_fetch ? 1 : 2) + ((m.mem.fdc >> 4) & 1);
    case R_DEV: return 2;
    default: return 1;
  }
}

// size: 0 byte, 1 halfword, 2 word. Returns the aligned 32-bit word the access reads.
static u32 Read(PocketStation& m, Bus& bus, u32 addr, int size, int access) {
  bool fetch = access & Bus::Code;
  if (fetch && addr == m.watch_fetch && m.fetch_hook) { m.watch_fetch = 0xFFFFFFFF; m.fetch_hook(m.kram_ctx, addr); }
  Decoded d = Decode(m, addr);
  m.Advance(Cost(m, d.r, fetch && size == 1));

  if (d.r == R_FAULT) {
    bus.abort = true;
    (fetch ? m.mem.st_fabort : m.mem.st_dabort) = true;
    return 0;
  }

  bool narrow = size == 0;
  bool signature = d.r == R_MEMCTL && ((addr >> 8) & 0xFFFF) == 0x0003;
  u32 garbage = m.prefetch_q | (m.ramdata_q & 0xFFFFFFD0);

  u32 word = 0;
  switch (d.r) {
    case R_SRAM: {
      u32 i = addr & 0x7FC;
      word = m.sram[i] | m.sram[i + 1] << 8 | m.sram[i + 2] << 16 | (u32)m.sram[i + 3] << 24;
      if (!fetch) m.ramdata_q = word;
      if (m.kram_hook && !fetch && (addr & 0x7FF) < 0x200) m.kram_hook(m.kram_ctx, addr & 0x7FF, size, false, word);
      break;
    }
    case R_BOOT: {
      u32 h = (addr >> 1) & 0x1FFF;
      if (size == 2) word = m.bootrom[h & ~1u] | (u32)m.bootrom[h | 1] << 16;
      else word = (u32)m.bootrom[h] << (addr & 2 ? 16 : 0);
      break;
    }
    case R_FLASH: case R_VFLASH: {
      u32 h = d.phys_half;
      if (size == 2) word = m.flash.Read(h & ~1u) | (u32)m.flash.Read(h | 1) << 16;
      else word = (u32)m.flash.Read(h) << (addr & 2 ? 16 : 0);
      break;
    }
    case R_MEMCTL: {
      u32 v = m.MemctlRead(addr & 0x3FE, true);
      word = signature ? v << (addr & 2 ? 16 : 0) : v;
      break;
    }
    case R_DEV:
      word = m.DevRead(addr & ~3u, true);
      break;
    default: break;
  }

  if (narrow && (d.r == R_BOOT || d.r == R_FLASH || d.r == R_VFLASH || signature)) return garbage;

  if (fetch) {
    if (size == 2) m.prefetch_q = word;
    else m.prefetch_q = (m.prefetch_q & 0xFFFF) | (addr & 2 ? word & 0xFFFF0000 : word << 16);
  }
  return word;
}

static void Write(PocketStation& m, Bus& bus, u32 addr, int size, u32 value) {
  Decoded d = Decode(m, addr);
  m.Advance(Cost(m, d.r, false));
  if (d.r == R_FAULT) { bus.abort = true; m.mem.st_dabort = true; return; }

  // The core's data bus repeats a narrow value across every lane.
  u32 lanes = size == 0 ? (value & 0xFF) * 0x01010101u : size == 1 ? (value & 0xFFFF) * 0x00010001u : value;
  u8 be = size == 0 ? 1 << (addr & 3) : size == 1 ? 3 << (addr & 2) : 0xF;

  switch (d.r) {
    case R_SRAM: {
      u32 i = addr & 0x7FC;
      for (int b = 0; b < 4; b++)
        if (be >> b & 1) m.sram[i + b] = lanes >> 8 * b;
      if (m.kram_hook && (addr & 0x7FF) < 0x200) m.kram_hook(m.kram_ctx, addr & 0x7FF, size, true, value);
      break;
    }
    case R_FLASH: case R_VFLASH:
      if (size == 2) { m.FlashWrite(d.phys_half & ~1u, value & 0xFFFF); m.FlashWrite(d.phys_half | 1, value >> 16); }
      else m.FlashWrite(d.phys_half, lanes & 0xFFFF);
      break;
    case R_MEMCTL: m.MemctlWrite(addr & 0x3FC, lanes, be); break;
    case R_DEV: m.DevWrite(addr & ~3u, lanes, be); break;
    default: break;
  }
}

auto Bus::ReadByte(u32 address, int access) -> u8 {
  return Read(m, *this, address, 0, access) >> 8 * (address & 3);
}
auto Bus::ReadHalf(u32 address, int access) -> u16 {
  return Read(m, *this, address & ~1u, 1, access) >> 8 * (address & 2);
}
auto Bus::ReadWord(u32 address, int access) -> u32 {
  return Read(m, *this, address & ~3u, 2, access);
}
void Bus::WriteByte(u32 address, u8 value, int) { Write(m, *this, address, 0, value); }
void Bus::WriteHalf(u32 address, u16 value, int) { Write(m, *this, address & ~1u, 1, value); }
void Bus::WriteWord(u32 address, u32 value, int) { Write(m, *this, address & ~3u, 2, value); }
void Bus::Idle() { m.Advance(1); }

} // namespace nba::core
