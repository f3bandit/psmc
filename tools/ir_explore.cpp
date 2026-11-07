// Infrared link check for every app on a set of cards: finds where each app turns its infrared on,
// then runs two units face to face (the link's own RunPair) through that and shows both screens.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
//   ir_explore BIOS [--nodes N] [--watch SECONDS] [--chords] [--pair] FILE...
//
// Explore: from the app's first screen, every button in turn (a short press, then 0.6 s), breadth
// first, new screens only, up to N screens. A press after which the unit is listening or sending
// on infrared is an "IR path". Pair (with --pair): two units on copies of the card, each its own
// serial number, both started in the app; for each IR path found, and each pairing of paths
// (one unit listening and the other sending, or both taking the same path together), runs them for
// 20 s and prints the pulses each sent and both screens before and after.
#include "card.hpp"
#include "link.hpp"
#include "pocketstation.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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

static bool KernelScreen(const PocketStation& m) { return m.sram[0x230] == 0x01 && m.sram[0x240] == 0x01 && m.sram[0x236] != 0x00; }

static std::string Screen(const PocketStation& m) {
  u8 f[32][32];
  m.Frame(f);
  std::string s;
  for (auto& r : f) for (u8 x : r) s += x ? '#' : '.';
  return s;
}

static int Mode(const PocketStation& u) { return u.IrSending() ? 2 : ((u.irda_static & 1) && !(u.irda_static & 2)) ? 2 : u.IrListening() ? 1 : 0; }

static const char* BN[] = {"F", "R", "L", "D", "U"};
// Actions: the buttons (FRLDU), long presses (f u d: Fire, Up, Down held 1.5 s) and pairs held
// together (a L+R, b U+D, c F+U, e F+D, g F+L, h F+R).
static const char ACTS_ALL[] = "FRLDUfudabcegh";
static u8 ActMask(char c) {
  switch (c) {
    case 'F': case 'f': return 1 << BTN_ENTER;
    case 'R': return 1 << BTN_RIGHT;
    case 'L': return 1 << BTN_LEFT;
    case 'D': case 'd': return 1 << BTN_DOWN;
    case 'U': case 'u': return 1 << BTN_UP;
    case 'a': return 1 << BTN_LEFT | 1 << BTN_RIGHT;
    case 'b': return 1 << BTN_UP | 1 << BTN_DOWN;
    case 'c': return 1 << BTN_ENTER | 1 << BTN_UP;
    case 'e': return 1 << BTN_ENTER | 1 << BTN_DOWN;
    case 'g': return 1 << BTN_ENTER | 1 << BTN_LEFT;
    case 'h': return 1 << BTN_ENTER | 1 << BTN_RIGHT;
  }
  return 0;
}
static double ActHold(char c) { return (c == 'f' || c == 'u' || c == 'd') ? 1.5 : 0.12; }

// A press, then `after` seconds; returns the IR modes seen (bit 1 listening, bit 2 sending).
static int PressMask(PocketStation& m, u8 mask, double hold, double after = 0.6);
static int Press(PocketStation& m, int btn, double hold = 0.12, double after = 0.6) { return PressMask(m, 1 << btn, hold, after); }
static int PressMask(PocketStation& m, u8 mask, double hold, double after) {
  int seen = 0;
  m.SetButtons(mask);
  for (double t = 0; t < hold; t += 0.02) { m.RunFor(0.02); seen |= Mode(m); m.TakeAudio(); }
  m.SetButtons(0);
  for (double t = 0; t < after; t += 0.02) { m.RunFor(0.02); seen |= Mode(m); m.TakeAudio(); }
  return seen;
}

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
  FILE* f = fopen("/tmp/ir_explore_bare.mcs", "wb");
  fwrite(mcs.data(), 1, mcs.size(), f);
  fclose(f);
  return card::Load("/tmp/ir_explore_bare.mcs", card);
}

struct Path { std::string keys; int modes; std::string screen; };

static std::vector<u8> g_bios;

// A unit with its clock set, then the card, the kernel told, and the app started.
static std::vector<u8> HomeState(u32 serial) {
  PocketStation b;
  b.LoadBios(g_bios);
  b.sig.serial_lo = serial & 0xFFFF;
  b.sig.serial_hi = serial >> 16;
  b.LoadCard(card::Formatted());
  b.Reset();
  b.RunFor(1.5);
  Press(b, BTN_ENTER, 0.15);
  b.RunFor(0.5);
  Press(b, BTN_UP, 2.0);
  for (int i = 0; i < 6; i++) Press(b, BTN_ENTER);
  b.RunFor(2.0);
  return b.SaveState();
}

static bool StartApp(PocketStation& m, const std::vector<u8>& home, const std::vector<u8>& img, int app_index) {
  m.LoadBios(g_bios);
  m.LoadState(home);
  m.LoadCard(img);
  m.SetDocked(true);
  m.RunFor(0.2);
  m.CardCommand({0x81, 0x53, 0, 0, 0, 0, 0, 0, 0, 0});
  m.RunFor(0.3);
  m.SetDocked(false);
  m.RunFor(1.0);
  for (int k = 0; k < app_index; k++) Press(m, BTN_RIGHT);
  Press(m, BTN_ENTER);
  m.RunFor(3.0);
  return !KernelScreen(m);
}

static void Show(const std::string& a, const std::string& b, const char* why) {
  printf("  -- %s\n", why);
  for (int y = 0; y < 32; y++) printf("  %s   %s\n", a.substr(y * 32, 32).c_str(), b.substr(y * 32, 32).c_str());
}

// Runs keys ("FRRU...") on a unit inside the pair, the other unit idle or running its own keys.
struct Script { std::string keys; size_t i = 0; double next = 0; int held_until_us = -1; };

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "ir_explore BIOS [--nodes N] [--pair] FILE...\n"); return 1; }
  g_bios = ReadAll(argv[1]);
  if (getenv("SLICE_US")) irlink::g_slice_us = atoi(getenv("SLICE_US"));
  int max_nodes = 250;
  bool pair = false;
  double watch = 0;
  const char* acts = "FRLDU";
  std::vector<std::string> extra;
  std::vector<std::string> files;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--nodes") && i + 1 < argc) max_nodes = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--pair")) pair = true;
    else if (!strcmp(argv[i], "--chords")) acts = ACTS_ALL;
    else if (!strcmp(argv[i], "--path") && i + 1 < argc) extra.push_back(argv[++i]);   // an IR path to try too (from the manual)   // long presses and two buttons together too
    else if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch = atof(argv[++i]);   // look longer for IR after each press
    else files.push_back(argv[i]);
  }
  const u32 SA = 0x5A17C0DE, SB = 0x3C0FFEE5;
  auto homeA = HomeState(SA), homeB = HomeState(SB);

  for (auto& path : files) {
    auto data = ReadAll(path);
    std::vector<u8> img;
    std::string err;
    if (data.size() >= 0x60 && data[0] == 'S' && data[1] == 'C' && memcmp(&data[0x52], "MCX", 3) == 0) err = WrapBare(data, img);
    else if (data.size() >= 2 && data[0] == 'S' && data[1] == 'N') err = "SN dev-kit file";
    else err = card::Load(path, img);
    if (!err.empty()) { printf("== %s\n  error: %s\n", path.c_str(), err.c_str()); continue; }
    auto entries = card::List(img);
    int app_index = 0;
    for (auto& e : entries) {
      if (!e.pocketstation) continue;
      app_index++;
      printf("== %s (app %d, %s)\n", path.c_str(), app_index, e.product.c_str());
      fflush(stdout);
      std::vector<u8> imgA = img, imgB = img;
      card::BindApps(imgA, SA);
      card::BindApps(imgB, SB);
      PocketStation m;
      if (!StartApp(m, homeA, imgA, app_index)) { printf("  didn't start\n"); continue; }
      int start_mode = Mode(m);
      // Breadth first over screens.
      struct Node { std::vector<u8> st; std::string keys; };
      std::deque<Node> q;
      std::set<std::string> seen;
      seen.insert(Screen(m));
      q.push_back({m.SaveState(), ""});
      std::vector<Path> ir;
      std::set<std::string> ir_screens;
      int nodes = 0;
      while (!q.empty() && nodes < max_nodes) {
        Node n = q.front();
        q.pop_front();
        if (n.keys.size() >= 8) continue;
        for (const char* ap = acts; *ap; ap++) {
          char act = *ap;
          PocketStation c;
          c.LoadBios(g_bios);
          c.LoadCard(imgA);
          c.LoadState(n.st);
          int modes = PressMask(c, ActMask(act), ActHold(act));
          std::string sc = Screen(c);
          if (watch > 0 && !modes) {
            PocketStation w;
            w.LoadBios(g_bios);
            w.LoadCard(imgA);
            w.LoadState(c.SaveState());
            for (double t = 0; t < watch && !modes; t += 0.05) { w.RunFor(0.05); modes |= Mode(w); w.TakeAudio(); }
          }
          std::string keys = n.keys + act;
          if (modes && !(start_mode && modes == start_mode && n.keys.empty())) {
            if (ir_screens.insert(sc + char('0' + modes)).second) ir.push_back({keys, modes, sc});
          }
          if (KernelScreen(c)) continue;   // left the app
          if (seen.insert(sc).second) { q.push_back({c.SaveState(), keys}); nodes++; }
        }
      }
      for (auto& x : extra) ir.push_back({x, 3, ""});
      printf("  explored %d screens; started with IR mode %d; IR paths %zu\n", nodes, start_mode, ir.size());
      for (auto& p : ir) {
        printf("    %-10s %s%s\n", p.keys.c_str(), p.modes & 1 ? "listens " : "", p.modes & 2 ? "sends" : "");
        if (getenv("IR_SHOW")) for (int y = 0; y < 32; y++) printf("      %s\n", p.screen.substr(y * 32, 32).c_str());
      }
      fflush(stdout);
      if (!pair || ir.empty()) continue;

      // Pairs: each path on A with B idle (sees nothing back); each listener against each sender;
      // and both on the same path together.
      std::vector<std::pair<int, int>> tries;   // (path for A, path for B), -1 = stay
      for (size_t i = 0; i < ir.size() && i < 6; i++) tries.push_back({(int)i, (int)i});
      for (size_t i = 0; i < ir.size() && i < 6; i++)
        for (size_t j = 0; j < ir.size() && j < 6; j++)
          if (i != j && (ir[i].modes & 1) && (ir[j].modes & 2)) tries.push_back({(int)i, (int)j});
      if (tries.size() > 10) tries.resize(10);
      for (auto [ia, ib] : tries) {
        PocketStation A, B;
        StartApp(A, homeA, imgA, app_index);
        StartApp(B, homeB, imgB, app_index);
        uint64_t ba = A.t, bb = B.t;
        int64_t us = 0;
        auto run = [&](double s) {
          int64_t end = us + (int64_t)(s * 1e6);
          irlink::RunPair(A, B, ba, bb, us, end);
          A.TakeAudio();
          B.TakeAudio();
        };
        long pa = 0, pb = 0;
        int rxbase = getenv("IR_RXBASE") ? (int)strtol(getenv("IR_RXBASE"), nullptr, 0) : 0, psa = -1, psb = -1, traced = 0, lighttr = 0;
        int64_t lat = 0, lbt = 0;
        bool la = false, lb = false;
        auto run_count = [&](double s) {
          int64_t end = us + (int64_t)(s * 1e6);
          while (us < end) {
            irlink::RunPair(A, B, ba, bb, us, std::min(end, us + 100));
            if (rxbase && B.IrSending() != lb && lighttr < 60) { lighttr++; printf("    %8.4fs  B light %d after %lld us  (B clock %d)\n", us / 1e6, B.IrSending(), (long long)(us - lbt), B.clk.freq); lbt = us; }
            if (rxbase && A.IrSending() != la && lighttr < 60) { lighttr++; printf("    %8.4fs  A light %d after %lld us  (A clock %d)\n", us / 1e6, A.IrSending(), (long long)(us - lat), A.clk.freq); lat = us; }
            if (A.IrSending() && !la) pa++;
            if (B.IrSending() && !lb) pb++;
            la = A.IrSending();
            lb = B.IrSending();
            if (rxbase) {   // a receiver's state machine, for digging into one game
              int sa = A.sram[rxbase + 40], sb = B.sram[rxbase + 40];
              if ((sa != psa || sb != psb) && traced < 400) {
                traced++;
                printf("    %8.4fs  A state %d  B state %d  (A light %d, B light %d, B bits %d, unit A %d B %d)\n", us / 1e6, sa, sb, la, lb, B.sram[rxbase + 12], A.sram[rxbase + 32] | A.sram[rxbase + 33] << 8, B.sram[rxbase + 32] | B.sram[rxbase + 33] << 8);
              }
              psa = sa;
              psb = sb;
            }
            if ((us / 100) % 167 == 0) { A.TakeAudio(); B.TakeAudio(); }
          }
        };
        (void)run;
        std::string ka = ir[ia].keys, kb = ir[ib].keys;
        // The listener first (a second ahead), the sender after; the same path: together.
        size_t n = std::max(ka.size(), kb.size());
        bool same = ia == ib;
        // all but the last key on both, then the last keys: A's, a second, B's
        // Both units' keys step by step, each held as long as its action says.
        auto step = [&](char ca, char cb) {
          double ha = ca ? ActHold(ca) : 0, hb = cb ? ActHold(cb) : 0;
          for (double t = 0; t < std::max(ha, hb); t += 0.02) {
            A.SetButtons(t < ha ? ActMask(ca) : 0);
            B.SetButtons(t < hb ? ActMask(cb) : 0);
            run_count(0.02);
          }
          A.SetButtons(0);
          B.SetButtons(0);
          run_count(0.6);
        };
        for (size_t k = 0; k + 1 < n; k++) step(k + 1 < ka.size() ? ka[k] : 0, k + 1 < kb.size() ? kb[k] : 0);
        if (same) step(ka.back(), kb.back());
        else { step(ka.back(), 0); run_count(0.4); step(0, kb.back()); }
        std::string mid = Screen(A), midB = Screen(B);
        run_count(1.5);
        std::string s2a = Screen(A), s2b = Screen(B);
        run_count(18.0);
        printf("  PAIR A:%s B:%s  pulses A %ld B %ld\n", ka.c_str(), kb.c_str(), pa, pb);
        Show(mid, midB, "after the link keys");
        Show(s2a, s2b, "1.5 s later");
        Show(Screen(A), Screen(B), "20 s later");
        fflush(stdout);
      }
    }
  }
}
