// MC/PS MemCard Pro: alerts from PocketStation games.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
#include "alerts.hpp"

#include "card.hpp"

#include <cstring>

namespace alerts {

using ps::u8;
using ps::u32;

namespace {

// How long the speaker has to be silent, and the buttons untouched, before a beep counts as the
// game calling rather than the game being played.
constexpr double QUIET = 20.0;
constexpr double HANDS_OFF = 10.0;
constexpr double MIN_BURST = 0.3;
// A game-specific alert usually lands with its beep; wait this long before falling back to the
// generic one.
constexpr double SETTLE = 2.0;
// At most one generic alert this often.
constexpr double GENERIC_GAP = 60.0;

bool KernelScreen(const ps::PocketStation& m) {
  return m.sram[0x230] == 0x01 && m.sram[0x240] == 0x01 && m.sram[0x236] != 0x00;
}

int Bcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

#include "known_apps.inc"

// Chocobo World, from Final Fantasy VIII: US, Japan, Europe (English, French, German, Italian, Spanish).
bool IsChocobo(const card::Entry& e) {
  for (const char* code : {"BASLUSP00892", "BISLPSP01880", "BESLESP02080", "BESLESP02081", "BESLESP02082",
                           "BESLESP02083", "BESLESP02084"})
    if (e.product.compare(0, 12, code) == 0) return true;
  return false;
}

}  // namespace

std::string DisplayName(const card::Entry& e) {
  if (e.pocketstation) return GameName(e);
  // A save: its game's name when the game's PocketStation app is a known one (same product code,
  // 'P' where the save has its hyphen), else the save's own title.
  std::string code = e.product.substr(0, 12);
  if (code.size() == 12 && code[6] == '-') code[6] = 'P';
  for (auto& k : KNOWN_CODES) if (code == k.code) return std::string(k.name) + " save";
  return e.title;
}

std::string GameName(const card::Entry& e) {
  std::string p = e.product;
  while (!p.empty() && p.back() == ' ') p.pop_back();
  for (auto& k : KNOWN_APPS) if (p == k.file) return k.name;
  for (auto& k : KNOWN_CODES) if (p.compare(0, 12, k.code) == 0) return k.name;
  // Otherwise the title from the save, when it reads well folded to ASCII.
  if (!e.title.empty() && e.title.find('?') == std::string::npos && e.title != e.product) {
    std::string t = e.title;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
  }
  return "Your PocketStation game";
}

Watcher::Running Watcher::Current(const ps::PocketStation& m) {
  if (KernelScreen(m)) return {};
  // The kernel maps the running app's file into the virtual flash window; its first block is the
  // one with a directory entry. Some apps unmap it once they've copied themselves to RAM, so keep
  // the last one seen.
  const auto& f = m.flash.array;
  int block = -1;
  for (u32 v = 0; v < 16 && block < 0; v++) {
    u32 phys;
    if (m.mem.Translate(v, phys) && phys >= 1 && phys < 16 && f[phys * 128] == 0x51) block = (int)phys;
  }
  if (block < 0) return last_;
  auto it = titles_.find(block);
  if (it != titles_.end()) return last_ = it->second;
  Running r;
  r.block = block;
  r.title = "Your PocketStation game";
  for (auto& e : card::List(f))
    if (e.first_block == block) { r.title = GameName(e); r.product = e.product; }
  titles_[block] = r;
  return last_ = r;
}

// Chocobo World (Final Fantasy VIII): the game keeps two copies of Boko's state in the first block
// of its file, at 0x200 and 0x300, and writes the older one each time; the counter at +8 says which
// is newer. Fields (BCD unless noted), after ChocoEdit by suloku: +1 level, +2 HP, +3 max HP,
// +6 rank, +0x14..0x17 items A-D. +0x2F is what Boko is doing: 1 walking, 2 an event is waiting
// (the "! EVENT" screen and its beep), higher values while one plays out.
void Watcher::Chocobo(const ps::PocketStation& m, double now, std::vector<Alert>& out) {
  const auto& f = m.flash.array;
  if (choco_block_ < 0 || !(f[choco_block_ * 128] == 0x51)) {
    if (now < choco_scan_at_) return;   // look for the game again every few seconds, not every frame
    choco_scan_at_ = now + 5;
    choco_block_ = -1;
    for (auto& e : card::List(f))
      if (e.pocketstation && IsChocobo(e)) { choco_block_ = e.first_block; break; }
    choco_seen_ = false;
    if (choco_block_ < 0) return;
  }
  const u8* a = &f[choco_block_ * 8192 + 0x200];
  const u8* b = &f[choco_block_ * 8192 + 0x300];
  u32 ca, cb;
  memcpy(&ca, a + 8, 4);
  memcpy(&cb, b + 8, 4);
  const u8* s = cb > ca ? b : a;
  if (!choco_seen_) {
    memcpy(choco_, s, sizeof choco_);
    choco_seen_ = true;
    choco_down_ = Bcd(s[2]) == 0;
    return;
  }
  if (memcmp(choco_, s, sizeof choco_) == 0) return;
  const u8* p = choco_;
  const std::string T = "Chocobo World";
  auto say = [&](const std::string& text) { out.push_back({T, text}); };

  if (s[0x2F] == 2 && p[0x2F] != 2) say("Boko ran into something! An event is waiting.");
  if (Bcd(s[1]) > Bcd(p[1])) say("Boko reached level " + std::to_string(Bcd(s[1])) + "!");
  if (Bcd(s[6]) > Bcd(p[6])) say("Boko's rank went up to " + std::to_string(Bcd(s[6])) + ".");
  // HP at 0: he lost a fight (the game puts him back on his feet by itself soon after).
  if (Bcd(s[2]) == 0 && !choco_down_) say("Boko lost a battle.");
  choco_down_ = Bcd(s[2]) == 0;
  for (int i = 0; i < 4; i++)
    if (Bcd(s[0x14 + i]) > Bcd(p[0x14 + i])) { say(std::string("Boko found an item (") + char('A' + i) + ")."); break; }

  memcpy(choco_, s, sizeof choco_);
}

void Watcher::Poll(const ps::PocketStation& m, double now, bool sound, bool buttons, std::vector<Alert>& out) {
  size_t before = out.size();
  if (buttons) last_button_ = now;
  Current(m);   // keep track of the app while it's mapped

  Chocobo(m, now, out);
  if (out.size() > before) specific_at_ = now;

  // Beeps.
  if (sound && !in_burst_) {
    in_burst_ = true;
    burst_start_ = now;
    // Only a beep out of a quiet spell, with nobody playing, is a call.
    if (now - quiet_since_ >= QUIET && now - last_button_ >= HANDS_OFF) {
      pending_at_ = now;
      pending_title_.clear();
    }
  }
  if (!sound && in_burst_) {
    in_burst_ = false;
    quiet_since_ = now;
    if (pending_at_ >= 0 && now - burst_start_ < MIN_BURST) pending_at_ = -1;   // a click, not a call
  }
  if (pending_at_ >= 0 && now - pending_at_ >= SETTLE) {
    bool covered = specific_at_ >= pending_at_ - SETTLE;
    if (!covered && now - last_generic_ >= GENERIC_GAP && (in_burst_ || now - burst_start_ >= MIN_BURST)) {
      if (KernelScreen(m)) out.push_back({"PocketStation", "Alarm"});
      else {
        auto r = Current(m);
        out.push_back({r.title, "Your game is calling. Have a look!"});
      }
      last_generic_ = now;
    }
    pending_at_ = -1;
  }

  // The LED.
  bool led = m.LedOn();
  if (led && !led_ && now - last_button_ >= HANDS_OFF && specific_at_ < now - SETTLE) {
    auto r = Current(m);
    out.push_back({r.block >= 0 ? r.title : "PocketStation", "The LED is on."});
  }
  led_ = led;
}

}  // namespace alerts
