// PocketStation emulator.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// A software model of the Sony PocketStation for the Windows app, built block by block from
// Jamie Blanks' MiSTer core (MiSTer-devel/PocketStation_MiSTer, GPL-2.0-or-later): each class
// below follows the HDL module of the same name in rtl/asic/. The CPU is NanoBoyAdvance's
// ARM7TDMI interpreter (nba-emu/NanoBoyAdvance, GPL-3.0-or-later). The memory card port follows
// this project's rtl/ps_spi.sv. See docs/emulator-spec.md.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nba/scheduler.hh>
#include "bus/bus.hh"
#include "arm/arm7tdmi.hh"

namespace ps {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Time runs on an 8 MHz master clock: 244 per crystal tick, one per system clock at the top
// CPU rate. Lower rates are reached with a fractional step, as ps_clocks' NCO does.
constexpr u32 CRYSTAL_HZ = 32768;
constexpr u32 MASTER_HZ = 244 * CRYSTAL_HZ;   // 7995392

enum Int : int {
  INT_EXT0 = 0, INT_EXT1, INT_EXT2, INT_EXT3, INT_EXT4, INT_EXT5,
  INT_SPI, INT_TC0, INT_TC1, INT_RTC, INT_VOL, INT_BAT, INT_IF, INT_TC2, INT_COUNT
};
constexpr u16 INT_FIQ_MASK    = (1 << INT_TC2) | (1 << INT_SPI);
constexpr u16 INT_IRQ_MASK    = (u16)~INT_FIQ_MASK & 0x3FFF;
constexpr u16 INT_LEVEL_MASK  = 0x003F;   // buttons
constexpr u16 INT_DOUBLE_MASK = (1 << INT_BAT) | (1 << INT_IF);
constexpr u16 INT_WAKE_MASK   = (1 << INT_RTC) | (1 << INT_VOL) | (1 << INT_BAT);
constexpr u16 INT_WAKE_ALWAYS = 1 << INT_EXT0;

// Buttons, in Sony's interrupt order.
enum Button : int { BTN_ENTER = 0, BTN_RIGHT, BTN_LEFT, BTN_DOWN, BTN_UP };

struct CardResult {
  std::vector<u8> rx;     // bytes that came back
  bool complete = false;  // every byte but the last was acknowledged
};

struct PocketStation {
  PocketStation();

  // --- setup ---------------------------------------------------------------------------------
  bool LoadBios(const std::vector<u8>& image);       // 16 KiB
  void LoadCard(const std::vector<u8>& image);       // 128 KiB flash image (.mcr / .mcd)
  std::vector<u8> Card() const { return flash.array; }
  void SetClock(u32 clock_bcd, u32 calendar_bcd);    // RTC fields, as ps_rtc keeps them
  void Reset();

  // --- running -------------------------------------------------------------------------------
  void RunFor(double seconds);                       // unit time
  double Now() const { return (double)(t >> 16) / MASTER_HZ; }

  // --- outside world ------------------------------------------------------------------------
  void SetButtons(u8 mask);                          // bit per Button, 1 = pressed
  void SetDocked(bool docked);                       // console power on the card connector
  // A console-side command on the card bus (81h ...), run at console timing. Docks the unit
  // if it isn't.
  CardResult CardCommand(const std::vector<u8>& tx);

  // 32x32, row major, 1 = dark pixel; all zero while the display is off.
  void Frame(u8 out[32][32]) const;
  bool LedOn() const { return pio.dir & 2 ? !(pio.out & 2) : false; }
  int Frequency() const { return clk.freq; }
  bool Standby() const { return clk.standby; }
  // Speaker level, sampled on the crystal: signed, roughly -512..511, 0 when silent.
  std::vector<int16_t> TakeAudio() { std::vector<int16_t> a; a.swap(audio); return a; }
  bool CardDirty() { bool d = flash.dirty; flash.dirty = false; return d; }

  // The whole unit as a real one keeps it on its battery: RAM, registers, CPU, clock. The card
  // (flash) and the BIOS are not included; they are loaded on their own.
  std::vector<u8> SaveState() const;
  bool LoadState(const std::vector<u8>& data);

  // --- internals, public for the bus and tests ---------------------------------------------
  u64 t = 0;                    // master ticks, 16.16
  u64 next_xtal = 0;            // when the next crystal tick falls, 16.16
  std::array<u64, 9> cyc_fx{};  // one system clock at each PMFrequency setting, 16.16

  void Advance(int sys_cycles);  // the bus and the core spend system clocks here
  void CrystalTick();
  void SampleInterrupts();
  void UpdateCpuLines();

  // Memory.
  std::array<u8, 2048> sram{};
  std::array<u16, 8192> bootrom{};
  u32 prefetch_q = 0, ramdata_q = 0;   // for narrow reads of 16-bit devices

  struct MemCtl {
    bool genrem = false, flashvir = false;
    u16 active = 0;
    u8 fdc = 0x14;
    u8 boot_wait = 0;
    std::array<u8, 16> vaddr{};
    bool st_multiple = false, st_noactive = false, st_dabort = false, st_fabort = false;
    bool Translate(u32 virt_block, u32& phys_block) const;
    bool Multiple() const;
  } mem;

  struct Flash {
    std::vector<u8> array = std::vector<u8>(131072, 0xFF);
    enum Seq { IDLE, KEY1, KEY2, ARMED, PROGRAM } seq = IDLE;
    u32 sector = 0;
    u64 page_valid = 0;
    std::array<u16, 64> page{};
    int delay = 0;
    bool dirty = false;
    u16 Read(u32 half_index) const { return array[half_index * 2] | array[half_index * 2 + 1] << 8; }
    bool Busy() const { return seq == PROGRAM; }   // BUSY reads 0 while programming
    void Commit();
  } flash;

  struct Signature { u16 serial_lo = 0xFFFF, serial_hi = 0xFFFF, cal = 0xFFFF; } sig;

  struct Intc {
    u16 latch = 0, mask = 0, prev = 0;
    std::array<int, 6> pulse{};   // button latch countdown, system clocks
  } intc;

  struct Timers {
    std::array<u16, 3> load{0xFFFF, 0xFFFF, 0xFFFF}, value{0xFFFF, 0xFFFF, 0xFFFF};
    std::array<u8, 3> div{}, en{};
    std::array<u32, 3> presc{};
  } tmr;

  struct ClkCtl { u8 freq = 0; bool standby = false; u16 wake_prev = 0; } clk;

  struct Rtc {
    u32 clock = 0x04000000, calendar = 0x00980101;
    bool prgsel = false, inc_armed = false, tick = false, resync = false;
    u8 cntsel = 0;
    u32 phase = 0;
  } rtc;

  struct Lcd {
    u8 dismod = 0, fr = 0, cal = 0;
    bool cpen = false, dison = false, rot = false;
    std::array<u32, 32> vram{};
  } lcd;

  struct Pio { u8 dir = 0, out = 0; } pio;
  struct Dac { bool active = false; u16 level = 0; } dac;
  u8 lvd_ctrl = 3;
  u8 irda_static = 2, irda_dynamic = 0;
  // Infrared, ps_irda. The far side is another unit: `ir_light` is true while its emitter is
  // on (the receiver demodulates the 40 kHz subcarrier to a level).
  bool ir_light = false;
  bool IrSending() const { return (irda_static & 1) && !(irda_static & 2) && (irda_dynamic & 1); }
  bool IrListening() const { return !(irda_static & 1) && !(irda_static & 2); }

  // Card port, rtl/ps_spi.sv.
  struct Spi {
    u8 mode = 0, ctrl1 = 0, ctrl2 = 0, tx = 0xFF, rx = 0;
    bool ready = false, released = true;
    bool LinkOn() const { return ctrl1 & 2; }
  } spi;
  void SpiAck();   // MODE bit 1 set: the unit acknowledges the byte it just received

  // The console on the other end of the card bus.
  struct Host {
    bool sel_low = false;
    bool acked = false;
  } host;

  u8 buttons = 0;
  bool docked = false;
  std::vector<int16_t> audio;

  nba::core::Scheduler scheduler;
  nba::core::Bus bus{*this};
  std::unique_ptr<nba::core::arm::ARM7TDMI> cpu;

  // Device registers.
  u32 DevRead(u32 addr, bool side_effects);
  void DevWrite(u32 addr, u32 value, u8 be);
  u32 MemctlRead(u32 off, bool side_effects);
  void MemctlWrite(u32 off, u32 value, u8 be);
  void FlashWrite(u32 half_index, u16 value);
};

} // namespace ps
