// Runs a saved infrared link again, cycle for cycle: both units from where they started, every
// press of either at the tick it landed. Prints both screens when either unit's infrared changes,
// a second after, at the times asked for, and at the end.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
//   link_replay BIOS FILE.mcpslink [SECONDS...]
#include "link.hpp"
#include "pocketstation.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

static void Screens(PocketStation& a, PocketStation& b, const std::string& why, uint32_t tick) {
  u8 fa[32][32], fb[32][32];
  a.Frame(fa);
  b.Frame(fb);
  printf("\n--- t%u (%.2fs) %s\n", tick, tick / 60.0, why.c_str());
  printf("A static %x dyn %x light %d          B static %x dyn %x light %d\n", a.irda_static, a.irda_dynamic, a.IrSending(),
         b.irda_static, b.irda_dynamic, b.IrSending());
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) putchar(fa[y][x] ? '#' : '.');
    printf("   ");
    for (int x = 0; x < 32; x++) putchar(fb[y][x] ? '#' : '.');
    putchar('\n');
  }
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "link_replay BIOS FILE.mcpslink [SECONDS...]\n"); return 1; }
  auto bios = ReadAll(argv[1]);
  auto f = ReadAll(argv[2]);
  size_t pos = 0;
  auto get32 = [&]() { uint32_t v = 0; if (pos + 4 <= f.size()) memcpy(&v, &f[pos], 4); pos += 4; return v; };
  auto blob = [&]() { uint32_t n = get32(); std::vector<u8> v(f.begin() + pos, f.begin() + std::min(f.size(), pos + n)); pos += n; return v; };
  if (get32() != irlink::REPLAY_MAGIC) { fprintf(stderr, "not a link replay\n"); return 1; }
  auto names = blob();
  auto sa = blob(), ca = blob(), sb = blob(), cb = blob();
  PocketStation a, b;
  a.LoadBios(bios);
  b.LoadBios(bios);
  a.LoadCard(ca);
  b.LoadCard(cb);
  if (!a.LoadState(sa) || !b.LoadState(sb)) { fprintf(stderr, "couldn't load the units (another BIOS?)\n"); return 1; }
  printf("A / B: %s\n", std::string(names.begin(), names.end()).c_str());
  struct Rec { uint32_t tick; u8 a, b; };
  std::vector<Rec> recs;
  while (pos + 6 <= f.size()) {
    Rec r;
    memcpy(&r.tick, &f[pos], 4);
    r.a = f[pos + 4];
    r.b = f[pos + 5];
    pos += 6;
    recs.push_back(r);
  }
  uint32_t end = recs.empty() ? 600 : recs.back().tick;
  std::vector<uint32_t> shots;
  for (int i = 3; i < argc; i++) shots.push_back((uint32_t)(atof(argv[i]) * 60));
  uint64_t base_a = a.t, base_b = b.t;
  int64_t us = 0;
  size_t ri = 0;
  int ma = -1, mb = -1;
  std::vector<uint32_t> after;   // a second after an infrared change
  for (uint32_t tick = 0; tick < end; tick++) {
    while (ri < recs.size() && recs[ri].tick == tick && recs[ri].a != 0xFF) {
      a.SetButtons(recs[ri].a);
      b.SetButtons(recs[ri].b);
      printf("t%u  presses  A %02x  B %02x\n", tick, recs[ri].a, recs[ri].b);
      ri++;
    }
    irlink::RunPair(a, b, base_a, base_b, us, (int64_t)(tick + 1) * 1000000 / 60);
    a.TakeAudio();
    b.TakeAudio();
    auto mode = [](PocketStation& u) { return ((u.irda_static & 1) && !(u.irda_static & 2)) ? 2 : u.IrListening() ? 1 : 0; };
    if (mode(a) != ma || mode(b) != mb) {
      static const char* nm[] = {"off", "receiving", "transmitting"};
      if (ma >= 0) Screens(a, b, std::string("infrared: A ") + nm[mode(a)] + ", B " + nm[mode(b)], tick + 1);
      ma = mode(a);
      mb = mode(b);
      after.push_back(tick + 61);
    }
    for (auto s : after) if (s == tick + 1) Screens(a, b, "a second after the infrared changed", tick + 1);
    for (auto s : shots) if (s == tick + 1) Screens(a, b, "asked for", tick + 1);
  }
  Screens(a, b, "the end of the link", end);
}
