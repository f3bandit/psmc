// MC/PS MemCard Pro: alerts from PocketStation games.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// A PocketStation game calls its owner the only way it can: it beeps, sometimes lights the LED,
// and writes what happened to its save. The watcher looks at all three while the unit runs and
// turns them into short messages the app can show as notifications:
//
//  - every game: a beep after a quiet spell while nobody is pressing buttons means the game
//    wants attention ("<game> is calling"); on the clock it's the alarm.
//  - games we know: their save is read field by field and the message says what happened
//    (Chocobo World: an event is waiting, Boko levelled up, was knocked out, ...).
#pragma once

#include "card.hpp"
#include "../emu/pocketstation.hpp"

#include <map>
#include <string>
#include <vector>

namespace alerts {

struct Alert {
  std::string title;   // the game, or "PocketStation"
  std::string text;
};

// The name to show for an app: from the known releases, else its own title when readable.
std::string GameName(const card::Entry& e);

class Watcher {
 public:
  // Call after every stretch of emulation. `sound` is whether the speaker moved in that stretch,
  // `buttons` whether any button was held. Appends what's new to `out`.
  void Poll(const ps::PocketStation& m, double now, bool sound, bool buttons, std::vector<Alert>& out);
  // The card was replaced: forget what was seen on the old one.
  void Reset() { *this = Watcher(); }

 private:
  struct Running { int block = -1; std::string title, product; };
  Running Current(const ps::PocketStation& m);
  void Chocobo(const ps::PocketStation& m, double now, std::vector<Alert>& out);

  std::map<int, Running> titles_;   // first block -> app
  Running last_;
  double quiet_since_ = 0, last_button_ = -1e9, burst_start_ = -1, pending_at_ = -1;
  bool in_burst_ = false, led_ = false;
  std::string pending_title_;
  double specific_at_ = -1e9;       // last game-specific alert, which a beep then needn't repeat
  double last_generic_ = -1e9;

  // Chocobo World: the last save seen (newest of its two copies).
  bool choco_seen_ = false, choco_down_ = false;
  int choco_block_ = -1;
  double choco_scan_at_ = -1e9;
  ps::u8 choco_[0x30] = {};
};

}  // namespace alerts
