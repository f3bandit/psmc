// Runs a saved infrared link again, cycle for cycle: both units from where they started, every
// press of either at the tick it landed. Prints both screens when either unit's infrared changes,
// a second after, at the times asked for, and at the end.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
//   link_replay BIOS FILE.mcpslink [SECONDS...]
//   link_replay BIOS FILE.mcpslink --history SIDE.state
//       compares the replay with one side's own unit, tick by tick, from the history in a dump
//       (the -theirs.state or -ours.state saved when a link got out of step): prints the first
//       tick where they part and what differs.
#include "link.hpp"
#include "pocketstation.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

static uint32_t Crc32(const u8* d, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return ~c;
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
  if (getenv("SLICE_US")) irlink::g_slice_us = atoi(getenv("SLICE_US"));
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
  std::map<uint32_t, std::vector<u8>> hist;
  char hist_unit = 0;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--history") && i + 1 < argc) {
      auto d = ReadAll(argv[++i]);
      size_t p = 4;
      auto g = [&]() { uint32_t v = 0; if (p + 4 <= d.size()) memcpy(&v, &d[p], 4); p += 4; return v; };
      p += g();   // unit A at the check
      p += g();   // unit B at the check
      if (p < d.size()) {
        hist_unit = (char)d[p++];
        uint32_t n = g();
        for (uint32_t k = 0; k < n && p < d.size(); k++) {
          uint32_t t = g(), len = g();
          hist[t].assign(d.begin() + p, d.begin() + std::min(d.size(), p + len));
          p += len;
        }
      }
      printf("history: unit %c, %zu ticks (t%u..t%u)\n", hist_unit ? hist_unit : '?', hist.size(),
             hist.empty() ? 0 : hist.begin()->first, hist.empty() ? 0 : hist.rbegin()->first);
    } else shots.push_back((uint32_t)(atof(argv[i]) * 60));
  }
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
    if (!hist.empty()) {
      auto it = hist.find(tick + 1);
      if (it != hist.end()) {
        auto mine = (hist_unit == 'A' ? a : b).SaveState();
        if (mine != it->second) {
          printf("\nFIRST DIFFERENCE at t%u (unit %c): replay vs that side's own unit\n", tick + 1, hist_unit);
          size_t shown = 0;
          for (size_t i = 0; i < std::min(mine.size(), it->second.size()) && shown < 40; i++)
            if (mine[i] != it->second[i]) { printf("  +%04zx: %02x / %02x\n", i, mine[i], it->second[i]); shown++; }
          PocketStation x;
          x.LoadBios(bios);
          x.LoadState(it->second);
          auto& r = (hist_unit == 'A' ? a : b).cpu->state;
          printf("  replay pc %08x t %llu | theirs pc %08x t %llu\n", r.r15, (unsigned long long)(hist_unit == 'A' ? a : b).t,
                 x.cpu->state.r15, (unsigned long long)x.t);
          Screens(a, b, "at the first difference", tick + 1);
          return 0;
        }
        hist.erase(it);
        if (hist.empty()) { printf("the whole history matches the replay\n"); return 0; }
      }
    }
    a.TakeAudio();
    b.TakeAudio();
    if ((tick + 1) % 30 == 0) {   // the link's step check, same formula: each unit's whole state
      auto sa = a.SaveState(), sb = b.SaveState();
      printf("step check t%u: A %08x B %08x\n", tick + 1, Crc32(sa.data(), sa.size()), Crc32(sb.data(), sb.size()));
    }
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
