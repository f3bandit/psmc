// Compatibility sweep: run every PocketStation app on a set of cards through the emulator.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// For each card image (.gme, .mcd, .mcs, or a bare MCX file) and each app on it: start from a
// unit whose clock is set, load the card, tell the kernel (a dock), go to the app with Right
// from the clock, start it with Fire, then play it for a while with a fixed pattern of
// presses. Reports, per app, one JSON line: did it start, did the screen move, did it make
// sound, use infrared, the LED or the card, read the serial number, return to the clock, or
// hit an undefined instruction or an abort; plus three screens (PBM-style 0/1 strings).
//
//   sweep BIOS SECONDS FILE...
#include "pocketstation.hpp"
#include "card.hpp"

#include <cstdio>
#include <cstring>
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
      printf("{\"file\":\"%s\",\"error\":\"%s\"}\n", path.c_str(), err.c_str());
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
      std::string title = e.title;
      for (auto& ch : title) if (ch == '"' || ch == '\\') ch = '\'';
      printf("{\"file\":\"%s\",\"app\":%d,\"name\":\"%s\",\"title\":\"%s\",\"blocks\":%d,\"started\":%d,"
             "\"screens\":%zu,\"sound\":%d,\"ir_tx\":%d,\"ir_rx\":%d,\"led\":%d,\"card_writes\":%d,\"to_kernel\":%d,"
             "\"undef\":%d,\"abort\":%d,\"browse\":\"%s\",\"s_start\":\"%s\",\"s_mid\":\"%s\",\"s_end\":\"%s\"}\n",
             path.c_str(), app_index, e.product.c_str(), title.c_str(), e.blocks, started, screens.size(), sound, ir_tx, ir_rx,
             led, card_writes, to_kernel, undef, abort_, browse.c_str(), s_start.c_str(), s_mid.c_str(), s_end.c_str());
      fflush(stdout);
    }
    if (app_index == 0) {
      printf("{\"file\":\"%s\",\"error\":\"no PocketStation app on this card (%zu saves)\"}\n", path.c_str(), entries.size());
      fflush(stdout);
    }
  }
}
