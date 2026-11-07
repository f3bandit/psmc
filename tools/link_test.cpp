// Two units linked over the real network code (localhost), each side running both units in
// lockstep: MGS Integral's ID transfer, side B receiving, side A sending. Checks that both sides'
// copies of both units stay identical and prints both screens.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
//   link_test BIOS MGS_CARD
#include "card.hpp"
#include "link.hpp"
#include "pocketstation.hpp"

#include <cstdio>
#include <cstring>

using namespace ps;

static std::vector<u8> ReadAll(const char* p) {
  std::vector<u8> v;
  FILE* f = fopen(p, "rb");
  if (!f) return v;
  u8 b[65536];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}

static void Press(PocketStation& m, int btn, double hold = 0.12) { m.SetButtons(1 << btn); m.RunFor(hold); m.SetButtons(0); m.RunFor(0.5); }

static void Boot(PocketStation& m, const std::vector<u8>& bios, u32 serial, const char* card) {
  m.LoadBios(bios);
  m.sig.serial_lo = serial & 0xFFFF;
  m.sig.serial_hi = serial >> 16;
  std::vector<u8> img;
  card::Load(card, img);
  card::BindApps(img, serial);
  m.LoadCard(img);
  m.Reset();
  m.RunFor(1.5);
  Press(m, BTN_ENTER, 0.15);
  m.RunFor(0.5);
  Press(m, BTN_UP, 2.0);
  for (int i = 0; i < 6; i++) Press(m, BTN_ENTER);
  m.RunFor(2);
  Press(m, BTN_RIGHT);
  Press(m, BTN_ENTER);
  m.RunFor(4);
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "link_test BIOS MGS_CARD\n"); return 1; }
  auto bios = ReadAll(argv[1]);
  PocketStation ua, ub;
  Boot(ua, bios, 0x11112222, argv[2]);
  Boot(ub, bios, 0x33334444, argv[2]);
  // Different amounts of time on each before the link, as two real units would have.
  ub.RunFor(1.234);

  irlink::Link la, lb;
  if (!la.Host("Side A")) { printf("host: %s\n", la.status.c_str()); return 1; }
  double wall = 0;
  for (int i = 0; i < 30; i++) { la.Poll(wall, ua, bios); lb.Discover(wall); wall += 0.05; }
  printf("found %zu unit(s)%s\n", lb.found.size(), lb.found.empty() ? "" : (": " + lb.found[0].name + " at " + lb.found[0].ip).c_str());
  lb.StopDiscovery();
  lb.Join("127.0.0.1", irlink::PORT, "Side B");
  for (int i = 0; i < 200 && !(la.Running() && lb.Running()); i++) { la.Poll(wall, ua, bios); lb.Poll(wall, ub, bios); wall += 0.01; }
  printf("A: %s | B: %s\n", la.status.c_str(), lb.status.c_str());
  if (!la.Running() || !lb.Running()) return 1;

  // Script, in seconds from the link: which side presses what.
  struct Ev { double t; char side; int btn; } evs[] = {
      {0.5, 'a', BTN_UP}, {0.5, 'b', BTN_UP},                          // the DATA menu
      {1.5, 'b', BTN_UP}, {2.0, 'b', BTN_UP}, {2.5, 'b', BTN_ENTER},  // B: RECEIVE
      {3.5, 'a', BTN_UP}, {4.0, 'a', BTN_ENTER},                      // A: SEND
  };
  double t0 = wall;
  u8 ba = 0, bb = 0;
  int stalls = 0;
  while (wall - t0 < 25) {
    double t = wall - t0;
    ba = bb = 0;
    for (auto& e : evs)
      if (t >= e.t && t < e.t + 0.12) (e.side == 'a' ? ba : bb) |= 1 << e.btn;
    la.Poll(wall, ua, bios);
    lb.Poll(wall, ub, bios);
    la.Advance(ua, ba, wall);
    lb.Advance(ub, bb, wall);
    stalls += la.stalled + lb.stalled;
    ua.TakeAudio();
    ub.TakeAudio();
    if (!la.Running() || !lb.Running()) { printf("ended: A: %s | B: %s\n", la.status.c_str(), lb.status.c_str()); return 1; }
    wall += 1.0 / 60;
  }
  // Each side's copy of the other's unit must be the other's unit, exactly.
  bool same_b = ua.sram == ub.sram ? false : true;   // (different units, different RAM)
  bool a_copy = la.peer->sram == ub.sram && la.peer->Now() == ub.Now();
  bool b_copy = lb.peer->sram == ua.sram && lb.peer->Now() == ua.Now();
  printf("stalled frames %d, A's copy of B %s, B's copy of A %s, units differ %s\n", stalls, a_copy ? "matches" : "DIFFERS",
         b_copy ? "matches" : "DIFFERS", same_b ? "yes" : "no");
  u8 fa[32][32], fb[32][32];
  ua.Frame(fa);
  ub.Frame(fb);
  printf("side A's unit                      side B's unit\n");
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) putchar(fa[y][x] ? '#' : '.');
    printf("   ");
    for (int x = 0; x < 32; x++) putchar(fb[y][x] ? '#' : '.');
    putchar('\n');
  }
  la.Stop("test done");
  for (int i = 0; i < 10; i++) { lb.Poll(wall, ub, bios); wall += 0.01; }
  printf("after the end: B says \"%s\"\n", lb.status.c_str());
  return a_copy && b_copy ? 0 : 1;
}
