// Runs one app on a card for a while, untouched, and prints the alerts the app would raise.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
//   alerts_run BIOS CARD APP_INDEX MINUTES [PLAY_PRESSES]
// With PLAY_PRESSES, answers each alert with that many presses of Fire, as a player would.
#include "alerts.hpp"
#include "card.hpp"
#include "pocketstation.hpp"

#include <cstdio>
#include <cstdlib>

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

static void Press(PocketStation& m, int btn, double hold = 0.12, double after = 0.6) {
  m.SetButtons(1 << btn);
  m.RunFor(hold);
  m.SetButtons(0);
  m.RunFor(after);
}

int main(int argc, char** argv) {
  if (argc < 5) { fprintf(stderr, "alerts_run BIOS CARD APP_INDEX MINUTES [PLAY_PRESSES]\n"); return 1; }
  int index = atoi(argv[3]), play = argc > 5 ? atoi(argv[5]) : 0;
  double minutes = atof(argv[4]);
  PocketStation m;
  m.LoadBios(ReadAll(argv[1]));
  m.sig.serial_lo = 0xC0DE;
  m.sig.serial_hi = 0x5A17;
  std::vector<u8> img;
  auto err = card::Load(argv[2], img);
  if (!err.empty()) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  card::BindApps(img, 0x5A17C0DE);
  m.LoadCard(img);
  m.Reset();
  m.RunFor(1.5);
  Press(m, BTN_ENTER, 0.15);
  m.RunFor(0.5);
  Press(m, BTN_UP, 2.0);
  for (int i = 0; i < 6; i++) Press(m, BTN_ENTER);
  m.RunFor(2.0);
  for (int k = 0; k < index; k++) Press(m, BTN_RIGHT);
  Press(m, BTN_ENTER);

  alerts::Watcher w;
  double end = m.Now() + minutes * 60;
  while (m.Now() < end) {
    m.RunFor(0.05);
    auto a = m.TakeAudio();
    bool sound = false;
    for (size_t j = 1; j < a.size() && !sound; j++) sound = a[j] != a[j - 1];
    std::vector<alerts::Alert> out;
    w.Poll(m, m.Now(), sound, false, out);
    for (auto& x : out) printf("%9.1f  %s: %s\n", m.Now(), x.title.c_str(), x.text.c_str());
    if (!out.empty() && getenv("SHOW")) {
      u8 f[32][32];
      m.Frame(f);
      for (auto& r : f) { for (u8 x : r) putchar(x ? '#' : '.'); putchar('\n'); }
    }
    if (!out.empty() && play) {
      for (int i = 0; i < play; i++) {
        Press(m, BTN_ENTER, 0.12, 1.5);
        std::vector<alerts::Alert> o2;
        w.Poll(m, m.Now(), false, true, o2);
        for (auto& x : o2) printf("%9.1f  %s: %s\n", m.Now(), x.title.c_str(), x.text.c_str());
      }
    }
  }
}
