// PocketStation emulator: command line front end, for checking against the HDL sim.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// Usage: ps_emu --bios FILE [--card FILE] [--seconds S] [--press BTN@T[+D]]...
//               [--card-test T] [--probe T] [--bench] [--dump-card FILE]
// Same options and output as sim/ps_sim, so the two can be run side by side.

#include "pocketstation.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ps;

static std::vector<u8> ReadFile(const std::string& path) {
  std::vector<u8> v;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return v;
  u8 buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
  fclose(f);
  return v;
}

static u8 XorSum(const u8* p, int n) { u8 c = 0; while (n--) c ^= *p++; return c; }

static void FormatCard(std::vector<u8>& c) {
  // Same image as the sim's: header, empty directory, unused broken-sector list, frame 63 marked.
  c.assign(131072, 0);
  c[0] = 'M'; c[1] = 'C'; c[127] = 'M' ^ 'C';
  for (int f = 1; f < 16; f++) {
    u8* p = &c[f * 128];
    p[0] = 0xA0; p[8] = 0xFF; p[9] = 0xFF;
    p[127] = XorSum(p, 127);
  }
  for (int f = 16; f < 36; f++) {
    u8* p = &c[f * 128];
    p[0] = p[1] = p[2] = p[3] = 0xFF; p[8] = 0xFF; p[9] = 0xFF;
    p[127] = XorSum(p, 127);
  }
  c[63 * 128] = 'M'; c[63 * 128 + 1] = 'C'; c[63 * 128 + 127] = 'M' ^ 'C';
}

// Retries a command that gets no ack on its first byte, as a console does next frame.
static CardResult Command(PocketStation& m, const std::vector<u8>& tx) {
  for (int tries = 0; tries < 60; tries++) {
    CardResult r = m.CardCommand(tx);
    m.RunFor(0.001);
    if (r.complete || r.rx.size() > 1) {
      if (tries) printf("  [bus] answered after %d retries\n", tries);
      return r;
    }
    m.RunFor(0.0167);
  }
  return m.CardCommand(tx);
}

static void Hex(const char* tag, const std::vector<u8>& v, size_t from, size_t n) {
  printf("  %s", tag);
  for (size_t i = from; i < from + n && i < v.size(); i++) printf(" %02X", v[i]);
  printf("\n");
}

static int CardTest(PocketStation& m, const std::vector<u8>& image) {
  int fails = 0;
  printf("[card] test at t=%.4fs\n", m.Now());
  auto id = Command(m, {0x81, 0x53, 0, 0, 0, 0, 0, 0, 0, 0});
  printf("[card] 53h ID: %s, %zu bytes\n", id.complete ? "complete" : "NO ACK", id.rx.size());
  Hex("rx:", id.rx, 0, id.rx.size());
  bool id_ok = id.complete && id.rx.size() == 10 && id.rx[2] == 0x5A && id.rx[3] == 0x5D && id.rx[4] == 0x5C;
  if (!id_ok) { printf("[card] ID: FAIL\n"); fails++; }

  auto read = [&](int frame, u8* data, u8* flag) {
    std::vector<u8> tx(140, 0);
    tx[0] = 0x81; tx[1] = 0x52; tx[4] = frame >> 8; tx[5] = frame & 0xFF;
    auto r = Command(m, tx);
    printf("[card] 52h read frame %d: %s, %zu bytes\n", frame, r.complete ? "complete" : "NO ACK", r.rx.size());
    if (r.rx.size() > 1) *flag = r.rx[1];
    if (!r.complete || r.rx.size() != 140) return false;
    memcpy(data, &r.rx[10], 128);
    u8 want = (frame >> 8) ^ (frame & 0xFF) ^ XorSum(data, 128);
    return r.rx[2] == 0x5A && r.rx[3] == 0x5D && r.rx[6] == 0x5C && r.rx[7] == 0x5D &&
           r.rx[138] == want && r.rx[139] == 0x47;
  };

  u8 buf[128], flag = 0;
  for (int f = 0; f < 2; f++) {
    bool ok = read(f, buf, &flag) && !memcmp(buf, &image[f * 128], 128);
    printf("[card] read %d: %s (flag %02X)\n", f, ok ? "PASS" : "FAIL", flag);
    fails += !ok;
  }

  u8 pat[128];
  for (int i = 0; i < 128; i++) pat[i] = (u8)(i * 37 + 11);
  std::vector<u8> tx(138, 0);
  tx[0] = 0x81; tx[1] = 0x57; tx[4] = 0; tx[5] = 63;
  memcpy(&tx[6], pat, 128);
  tx[134] = 0 ^ 63 ^ XorSum(pat, 128);
  auto w = Command(m, tx);
  bool wok = w.complete && w.rx.size() == 138 && w.rx[135] == 0x5C && w.rx[136] == 0x5D && w.rx[137] == 0x47;
  printf("[card] 57h write frame 63: %s, %zu bytes\n", w.complete ? "complete" : "NO ACK", w.rx.size());
  printf("[card] write 63: %s\n", wok ? "PASS" : "FAIL");
  fails += !wok;

  bool rok = read(63, buf, &flag) && !memcmp(buf, pat, 128);
  printf("[card] read-back 63: %s (flag %02X)\n", rok ? "PASS" : "FAIL", flag);
  fails += !rok;

  auto card = m.Card();
  bool fok = !memcmp(&card[63 * 128], pat, 128);
  printf("[card] flash frame 63: %s\n", fok ? "PASS" : "FAIL");
  fails += !fok;
  printf("[card] %s (%d failed)\n", fails ? "FAILED" : "ALL PASSED", fails);
  return fails;
}

static void Probe(PocketStation& m) {
  printf("[probe] at t=%.4fs\n", m.Now());
  auto show = [&](const char* name, std::vector<u8> tx) {
    auto r = Command(m, tx);
    printf("[probe] %-26s %3zu/%-3zu bytes  %s\n", name, r.rx.size(), tx.size(), r.complete ? "complete" : "stopped");
    for (size_t i = 0; i < r.rx.size(); i += 16) {
      printf("    tx"); for (size_t k = i; k < i + 16 && k < tx.size(); k++) printf(" %02X", tx[k]); printf("\n");
      printf("    rx"); for (size_t k = i; k < i + 16 && k < r.rx.size(); k++) printf(" %02X", r.rx[k]); printf("\n");
    }
  };
  std::vector<u8> id = {0x81, 0x53}; id.resize(10, 0);
  show("53h ID", id);
  show("58h version", {0x81, 0x58, 0, 0, 0});
  std::vector<u8> st = {0x81, 0x5A}; st.resize(21, 0);
  show("5Ah status", st);
  std::vector<u8> dt = {0x81, 0x5B, 0x00, 0, 0}; dt.resize(14, 0);
  show("5Bh FUNC 00h date/time", dt);
  std::vector<u8> mr = {0x81, 0x5B, 0x01, 0, 0x00, 0x00, 0x00, 0x04, 0x80, 0}; mr.resize(139, 0);
  show("5Bh FUNC 01h @04000000", mr);
}

int main(int argc, char** argv) {
  std::string bios, card_file, dump_file;
  double seconds = 3.0, test_t = -1, probe_t = -1;
  bool bench = false;
  struct Press { int bit; double t, dur; };
  std::vector<Press> presses;
  const char* names[5] = {"enter", "right", "left", "down", "up"};

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); } return argv[++i]; };
    if (a == "--bios") bios = next();
    else if (a == "--card") card_file = next();
    else if (a == "--seconds") seconds = atof(next().c_str());
    else if (a == "--card-test") test_t = atof(next().c_str());
    else if (a == "--probe") probe_t = atof(next().c_str());
    else if (a == "--dump-card") dump_file = next();
    else if (a == "--bench") bench = true;
    else if (a == "--press") {
      std::string p = next();
      size_t at = p.find('@');
      Press pr{-1, 0, 0.15};
      for (int b = 0; b < 5; b++) if (p.substr(0, at) == names[b]) pr.bit = b;
      if (pr.bit < 0 || at == std::string::npos) { fprintf(stderr, "bad --press %s\n", p.c_str()); return 2; }
      std::string rest = p.substr(at + 1);
      size_t plus = rest.find('+');
      pr.t = atof(rest.substr(0, plus).c_str());
      if (plus != std::string::npos) pr.dur = atof(rest.substr(plus + 1).c_str());
      presses.push_back(pr);
    } else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }

  PocketStation m;
  auto rom = ReadFile(bios);
  if (!m.LoadBios(rom)) { fprintf(stderr, "can't load BIOS %s\n", bios.c_str()); return 1; }
  printf("BIOS %s: %zu bytes\n", bios.c_str(), rom.size());
  std::vector<u8> card;
  if (!card_file.empty()) { card = ReadFile(card_file); printf("card %s: %zu bytes\n", card_file.c_str(), card.size()); }
  else { FormatCard(card); printf("card: freshly formatted\n"); }
  m.LoadCard(card);
  m.Reset();

  double dock_t = -1;
  if (test_t >= 0) dock_t = test_t > 0.2 ? test_t - 0.2 : 0;
  if (probe_t >= 0 && (dock_t < 0 || probe_t - 0.2 < dock_t)) dock_t = probe_t > 0.2 ? probe_t - 0.2 : 0;

  auto wall0 = std::chrono::steady_clock::now();
  int led = -1, freq = -1, stby = -1, rc = 0;
  bool docked = false, tested = false, probed = false;
  const double step = 0.0005;
  while (m.Now() < seconds) {
    double t = m.Now();
    u8 b = 0;
    for (auto& p : presses) if (t >= p.t && t < p.t + p.dur) b |= 1 << p.bit;
    m.SetButtons(b);
    if (!docked && dock_t >= 0 && t >= dock_t) { m.SetDocked(true); docked = true; printf("t=%8.4fs docked\n", t); }
    if (!tested && test_t >= 0 && t >= test_t) { tested = true; rc = CardTest(m, card) ? 1 : 0; }
    if (!probed && probe_t >= 0 && t >= probe_t) { probed = true; Probe(m); }
    m.RunFor(step);
    if (m.LedOn() != led || m.Frequency() != freq || m.Standby() != stby) {
      led = m.LedOn(); freq = m.Frequency(); stby = m.Standby();
      printf("t=%8.4fs led=%d freq_sel=%d standby=%d\n", m.Now(), led, freq, stby);
    }
  }
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();

  u8 px[32][32];
  m.Frame(px);
  printf("final LCD at %.2fs:\n", m.Now());
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) putchar(px[y][x] ? '#' : '.');
    putchar('\n');
  }
  if (bench) printf("\n%.1f s of PocketStation time took %.2f s: %.1fx real time\n", m.Now(), wall, m.Now() / wall);
  if (!dump_file.empty()) {
    auto c = m.Card();
    if (FILE* f = fopen(dump_file.c_str(), "wb")) { fwrite(c.data(), 1, c.size(), f); fclose(f); }
  }
  return rc;
}
