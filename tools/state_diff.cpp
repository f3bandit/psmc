// Compares the two sides' units from a link that got out of step (the -ours.state and
// -theirs.state files saved beside its replay) and names the parts of the unit that differ.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
//   state_diff OURS.state THEIRS.state
#include "pocketstation.hpp"

#include <cstdio>
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

// The fields of a saved unit, in the order PocketStation::SaveState writes them.
struct Field { std::string name; size_t size; };
static std::vector<Field> Layout() {
  PocketStation m;
  std::vector<Field> f;
  auto add = [&](const char* n, size_t s) { f.push_back({n, s}); };
  add("t", sizeof m.t); add("next_xtal", sizeof m.next_xtal);
  add("sram", sizeof m.sram); add("prefetch_q", sizeof m.prefetch_q); add("ramdata_q", sizeof m.ramdata_q);
  add("mem", sizeof m.mem); add("sig", sizeof m.sig); add("intc", sizeof m.intc); add("tmr", sizeof m.tmr);
  add("clk", sizeof m.clk); add("rtc", sizeof m.rtc); add("lcd", sizeof m.lcd);
  add("pio", sizeof m.pio); add("dac", sizeof m.dac); add("lvd_ctrl", sizeof m.lvd_ctrl);
  add("irda_static", sizeof m.irda_static); add("irda_dynamic", sizeof m.irda_dynamic);
  add("spi", sizeof m.spi); add("docked", sizeof m.docked);
  add("flash.seq", sizeof m.flash.seq); add("flash.sector", sizeof m.flash.sector);
  add("flash.page_valid", sizeof m.flash.page_valid); add("flash.page", sizeof m.flash.page);
  add("flash.delay", sizeof m.flash.delay); add("scheduler.pending", sizeof m.scheduler.pending);
  static const char* cpu_names[] = {"cpu.state (registers)", "cpu.pipe", "cpu.irq_line", "cpu.fiq_line",
                                    "cpu.latch_irq_disable", "cpu.ldm_usermode_conflict", "cpu.cpu_mode_is_invalid"};
  int i = 0;
  m.cpu->StateFields([&](auto& v) { f.push_back({cpu_names[i++], sizeof v}); });
  return f;
}

static bool Split(const std::vector<u8>& d, uint32_t& tick, std::vector<u8>& a, std::vector<u8>& b) {
  if (d.size() < 12) return false;
  size_t p = 0;
  auto g = [&]() { uint32_t v; memcpy(&v, &d[p], 4); p += 4; return v; };
  tick = g();
  uint32_t na = g();
  if (p + na + 4 > d.size()) return false;
  a.assign(d.begin() + p, d.begin() + p + na);
  p += na;
  uint32_t nb = g();
  if (p + nb > d.size()) return false;
  b.assign(d.begin() + p, d.begin() + p + nb);
  return true;
}

static void Compare(const char* unit, const std::vector<u8>& x, const std::vector<u8>& y, const std::vector<Field>& lay) {
  if (x == y) { printf("unit %s: the same\n", unit); return; }
  printf("unit %s differs:\n", unit);
  size_t off = 8;   // after the magic
  for (auto& f : lay) {
    if (off + f.size > x.size() || off + f.size > y.size()) break;
    if (memcmp(&x[off], &y[off], f.size) != 0) {
      printf("  %-26s", f.name.c_str());
      int shown = 0;
      for (size_t i = 0; i < f.size && shown < 12; i++)
        if (x[off + i] != y[off + i]) { printf(" +%zx:%02x/%02x", i, x[off + i], y[off + i]); shown++; }
      printf("\n");
    }
    off += f.size;
  }
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "state_diff OURS.state THEIRS.state\n"); return 1; }
  uint32_t t1, t2;
  std::vector<u8> a1, b1, a2, b2;
  if (!Split(ReadAll(argv[1]), t1, a1, b1) || !Split(ReadAll(argv[2]), t2, a2, b2)) { fprintf(stderr, "can't read the dumps\n"); return 1; }
  printf("tick %u / %u (values: ours/theirs)\n", t1, t2);
  auto lay = Layout();
  Compare("A", a1, a2, lay);
  Compare("B", b1, b2, lay);
}
