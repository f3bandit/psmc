// Which kernel calls each app makes, and which kernel RAM it touches, with the real BIOS: what a
// BIOS of our own has to do the same way. Runs every app the way tools/sweep does (launch, then a
// press every 0.4 s) with the core's tracing hooks on, from the moment the app is started.
// Records only what crosses between an app and the kernel: SWI numbers, their arguments and
// results, and the app's own reads and writes of kernel RAM (0..1FFh); nothing of the kernel's code.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
//   kernel_trace BIOS SECONDS FILE...      a report per app, then the totals
#include "pocketstation.hpp"
#include "card.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ps;

static std::vector<u8> ReadAll(const std::string& p) {
  std::vector<u8> v;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return v;
  u8 b[65536];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}

static bool KernelScreen(const PocketStation& m) {
  return m.sram[0x230] == 0x01 && m.sram[0x240] == 0x01 && m.sram[0x236] != 0x00;
}

static std::string Screen(const PocketStation& m) {
  u8 f[32][32];
  m.Frame(f);
  std::string s;
  for (auto& r : f) for (u8 x : r) s += x ? '1' : '0';
  return s;
}

static void Press(PocketStation& m, int btn, double hold = 0.12, double after = 0.6) {
  m.SetButtons(1 << btn);
  m.RunFor(hold);
  m.SetButtons(0);
  m.RunFor(after);
}

// A bare MCX file (starts "SC", "MCX" at 0x52): give it a directory entry like a PS1 game would.
static std::string WrapBare(const std::vector<u8>& d, std::vector<u8>& card) {
  card = card::Formatted();
  size_t blocks = (d.size() + 8191) / 8192;
  if (blocks < 1 || blocks > 15) return "too big";
  std::vector<u8> mcs(128 + blocks * 8192, 0);
  mcs[0] = 0x51;
  uint32_t size = (uint32_t)(blocks * 8192);
  memcpy(&mcs[4], &size, 4);
  mcs[8] = mcs[9] = 0xFF;
  const char* name = "BISCPSP00000SWEEP";
  memcpy(&mcs[0x0A], name, strlen(name));
  u8 x = 0;
  for (int i = 0; i < 127; i++) x ^= mcs[i];
  mcs[127] = x;
  memcpy(&mcs[128], d.data(), d.size());
  FILE* f = fopen("/tmp/sweep_bare.mcs", "wb");
  fwrite(mcs.data(), 1, mcs.size(), f);
  fclose(f);
  return card::Load("/tmp/sweep_bare.mcs", card);
}


struct Trace {
  PocketStation* m = nullptr;
  struct Pending { u32 ret, num, r0, r1, r2, r3; bool app; };
  std::vector<Pending> pending;
  struct Call { long n = 0; std::set<std::string> examples; };
  std::map<u32, Call> app_swi, kernel_swi;
  struct Ram { long reads = 0, writes = 0; int sizes = 0; std::set<u32> written; std::set<int> modes; };
  std::map<u32, Ram> ram;   // by word address
  bool entered = false;
  u32 entry_addr = 0;   // from the app's header (title sector +5Ch)
  std::string entry;   // the unit as the app's first instruction runs
  static bool AppPc(u32 pc) { return pc >= 0x02000000 && pc < 0x04000000; }
  static bool RamPc(u32 pc) { return pc < 0x800; }   // code copied to RAM by the app
};

static void OnSwi(void* ctx, u32 num, u32 from) {
  Trace& t = *(Trace*)ctx;
  auto& r = t.m->cpu->state.reg;
  bool app = Trace::AppPc(from) || (Trace::RamPc(from) && from >= 0x200);
  bool thumb = t.m->cpu->state.cpsr.f.thumb;
  t.pending.push_back({from + (thumb ? 2u : 4u), num, r[0], r[1], r[2], r[3], app});
  if (t.pending.size() > 32) t.pending.erase(t.pending.begin());
}

static void OnExRet(void* ctx, u32 to, u32 r0) {
  Trace& t = *(Trace*)ctx;

  for (size_t i = t.pending.size(); i-- > 0;) {
    auto& p = t.pending[i];
    if ((to & ~1u) != (p.ret & ~1u)) continue;
    auto& c = (p.app ? t.app_swi : t.kernel_swi)[p.num];
    c.n++;
    if (c.examples.size() < 6) {
      char b[96];
      snprintf(b, sizeof b, "(%x,%x,%x,%x)->%x", p.r0, p.r1, p.r2, p.r3, r0);
      c.examples.insert(b);
    }
    t.pending.erase(t.pending.begin() + i);
    return;
  }
}

static void OnEntry(void* ctx, u32 to) {
  Trace& t = *(Trace*)ctx;
  {
    t.entered = true;
    auto& m = *t.m;
    auto& st = m.cpu->state;
    char b[400];
    snprintf(b, sizeof b,
             "entry %08x cpsr %08x r0 %x sp %x | int mask %04x | clock %d | timers en %d%d%d div %d%d%d load %x %x %x | lcd dismod %x fr %x "
             "dison %d rot %d cpen %d | ir static %x | dac %d | bank %s",
             to, st.cpsr.v, st.reg[0], st.reg[13], m.intc.mask, m.clk.freq, m.tmr.en[0], m.tmr.en[1], m.tmr.en[2], m.tmr.div[0], m.tmr.div[1],
             m.tmr.div[2], m.tmr.load[0], m.tmr.load[1], m.tmr.load[2], m.lcd.dismod, m.lcd.fr, m.lcd.dison, m.lcd.rot,
             m.lcd.cpen, m.irda_static, m.dac.active, "");
    t.entry = b;
  }
}

static void OnKram(void* ctx, u32 addr, int size, bool write, u32 value) {
  Trace& t = *(Trace*)ctx;
  u32 pc = t.m->cpu->state.r15;
  if (!Trace::AppPc(pc) && !(Trace::RamPc(pc) && pc >= 0x200)) return;   // the kernel's own
  auto& r = t.ram[addr & ~3u];
  (write ? r.writes : r.reads)++;
  r.sizes |= 1 << size;
  r.modes.insert(t.m->cpu->state.cpsr.f.mode);
  if (write && r.written.size() < 4) r.written.insert(value);
}

static std::map<u32, long> g_app_swi_apps;   // how many apps use each call
static std::map<u32, long> g_ram_apps;

static void Report(const Trace& t, const std::string& name) {
  printf("== %s\n", name.c_str());
  if (!t.entry.empty()) printf("  %s\n", t.entry.c_str());
  for (auto& [n, c] : t.app_swi) {
    printf("  SWI %02x x%ld", n, c.n);
    for (auto& e : c.examples) printf(" %s", e.c_str());
    printf("\n");
    g_app_swi_apps[n]++;
  }
  for (auto& [n, c] : t.kernel_swi) printf("  (kernel's own SWI %02x x%ld)\n", n, c.n);
  for (auto& [a, r] : t.ram) {
    printf("  RAM %03x  reads %ld writes %ld sizes %s%s%s", a, r.reads, r.writes, r.sizes & 1 ? "b" : "", r.sizes & 2 ? "h" : "",
           r.sizes & 4 ? "w" : "");
    printf(" in");
    for (int md : r.modes) printf(" %s", md == 0x10 ? "usr" : md == 0x11 ? "fiq" : md == 0x12 ? "irq" : md == 0x13 ? "svc" : md == 0x1F ? "sys" : "?");
    printf(" | wrote");
    for (auto v : r.written) printf(" %x", v);
    printf("\n");
    g_ram_apps[a]++;
  }
  fflush(stdout);
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "sweep BIOS SECONDS FILE...\n"); return 1; }
  auto bios = ReadAll(argv[1]);
  double seconds = atof(argv[2]);
  const u32 SERIAL = 0x5A17C0DE;

  // A unit with its clock set, on an empty card: every app starts from this.
  PocketStation base;
  base.LoadBios(bios);
  base.sig.serial_lo = SERIAL & 0xFFFF;
  base.sig.serial_hi = SERIAL >> 16;
  base.LoadCard(card::Formatted());
  base.Reset();
  base.RunFor(1.5);
  Press(base, BTN_ENTER, 0.15);
  base.RunFor(0.5);
  Press(base, BTN_UP, 2.0);
  for (int i = 0; i < 6; i++) Press(base, BTN_ENTER);
  base.RunFor(2.0);
  if (!KernelScreen(base)) { fprintf(stderr, "base unit isn't on the clock\n"); return 1; }
  auto home = base.SaveState();

  for (int fi = 3; fi < argc; fi++) {
    std::string path = argv[fi];
    auto data = ReadAll(path);
    std::vector<u8> img;
    std::string err;
    if (data.size() >= 0x60 && data[0] == 'S' && data[1] == 'C' && memcmp(&data[0x52], "MCX", 3) == 0)
      err = WrapBare(data, img);
    else if (data.size() >= 2 && data[0] == 'S' && data[1] == 'N')
      err = "SN Systems dev-kit file, not a PocketStation file";
    else
      err = card::Load(path, img);
    if (!err.empty()) {
      printf("== %s\n  error: %s\n", path.c_str(), err.c_str());
      fflush(stdout);
      continue;
    }
    card::BindApps(img, SERIAL);
    auto entries = card::List(img);
    int app_index = 0;
    for (auto& e : entries) {
      if (!e.pocketstation) continue;
      app_index++;
      PocketStation m;
      Trace tr;
      tr.m = &m;
      {
        size_t o = (size_t)e.first_block * 8192 + 0x5C;
        if (o + 4 <= img.size()) tr.entry_addr = img[o] | img[o + 1] << 8 | img[o + 2] << 16 | (u32)img[o + 3] << 24;
      }
      m.LoadBios(bios);
      m.LoadState(home);
      m.LoadCard(img);
      // Tell the kernel the card changed, as a console would.
      m.SetDocked(true);
      m.RunFor(0.2);
      m.CardCommand({0x81, 0x53, 0, 0, 0, 0, 0, 0, 0, 0});
      m.RunFor(0.3);
      m.SetDocked(false);
      m.RunFor(1.0);
      for (int k = 0; k < app_index; k++) Press(m, BTN_RIGHT);
      std::string browse = Screen(m);
      m.cpu->hook_ctx = &tr;
      m.cpu->swi_hook = OnSwi;
      m.cpu->exret_hook = OnExRet;
      m.kram_ctx = &tr;
      m.kram_hook = OnKram;
      m.watch_fetch = tr.entry_addr & ~1u;
      m.fetch_hook = OnEntry;
      Press(m, BTN_ENTER);
      m.RunFor(3.0);
      bool started = !KernelScreen(m);
      std::string s_start = Screen(m);

      // Play: a press every 0.4 s, cycling through the buttons, Fire most often; watch.
      std::set<std::string> screens;
      bool sound = false, ir_tx = false, ir_rx = false, led = false, to_kernel = false;
      int undef = 0, abort_ = 0, card_writes = 0;
      std::string s_mid, s_end;
      const int pattern[] = {BTN_ENTER, BTN_RIGHT, BTN_ENTER, BTN_LEFT, BTN_ENTER, BTN_UP, BTN_ENTER, BTN_DOWN};
      int steps = (int)(seconds / 0.05);
      for (int i = 0; i < steps; i++) {
        int phase = i % 8;
        m.SetButtons(phase < 2 ? (1 << pattern[(i / 8) % 8]) : 0);
        m.RunFor(0.05);
        auto a = m.TakeAudio();
        for (size_t j = 1; j < a.size() && !sound; j++) if (a[j] != a[j - 1]) sound = true;
        if (m.IrSending()) ir_tx = true;
        if (m.IrListening()) ir_rx = true;
        if (m.LedOn()) led = true;
        if (m.CardDirty()) card_writes++;
        u32 mode = m.cpu->state.cpsr.v & 0x1F;
        if (mode == 0x1B) undef++;
        if (mode == 0x17) abort_++;
        if (i % 4 == 0) screens.insert(Screen(m));
        if (i > 20 && KernelScreen(m)) to_kernel = true;
        if (i == steps / 2) s_mid = Screen(m);
      }
      s_end = Screen(m);
      Report(tr, path + (app_index > 1 ? " (app " + std::to_string(app_index) + ")" : "") + (started ? "" : "  [didn't start]") +
                     (undef || abort_ ? "  [crashed]" : ""));
      (void)s_end; (void)ir_tx; (void)ir_rx; (void)led; (void)to_kernel;
      fflush(stdout);
    }
    if (app_index == 0) {
      printf("{\"file\":\"%s\",\"error\":\"no PocketStation app on this card (%zu saves)\"}\n", path.c_str(), entries.size());
      fflush(stdout);
    }
  }
  printf("\n== Totals: apps using each call\n");
  for (auto& [n, c] : g_app_swi_apps) printf("  SWI %02x  %ld apps\n", n, c);
  printf("== Totals: apps touching each kernel RAM word\n");
  for (auto& [a, c] : g_ram_apps) printf("  RAM %03x  %ld apps\n", a, c);
}
