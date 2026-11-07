// MC/PS MemCard Pro: two units' infrared, over the local network.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// PocketStation games talk by infrared with bits about 2 ms long, which no network round trip
// keeps up with. So a link doesn't send the light: at the start each side sends the other its
// unit (state and card) once, and from then on both sides run BOTH units, in lockstep, with the
// infrared between them wired up exactly as two units held face to face. Only button presses
// cross the network, each a few frames ahead of when it's used, so normal Wi-Fi lag doesn't show.
// The emulation is deterministic, so the two copies stay identical; a checksum every two seconds
// makes sure. Each side shows its own unit; when the link ends, each keeps its own as it is.
//
// One side waits for a link (and says so on the network every second, UDP broadcast on port
// 7551); the other picks it from the list, or types its address. TCP on port 7551 carries it.
#pragma once

#include "../emu/pocketstation.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace irlink {

constexpr int PORT = 7551;

struct Found {
  std::string name, ip;
  int port = PORT;
  double seen = 0;
};

class Link {
 public:
  enum State { OFF, WAITING, CONNECTING, STARTING, RUNNING };

  ~Link();
  State state = OFF;
  std::string status;          // one line for the UI: what's happening, or why it ended
  std::string peer_name, peer_ip;
  std::vector<Found> found;    // units on the network waiting for a link
  bool stalled = false;        // running, but waiting on the other side's presses
  std::unique_ptr<ps::PocketStation> peer;   // the other unit, run here in step with ours

  bool Host(const std::string& name);                              // wait for a link
  bool Join(const std::string& ip, int port, const std::string& name);
  void Stop(const std::string& why);                               // end it (tells the other side)
  // Listen for units waiting for a link (fills `found`). Call while the Link page is up.
  void Discover(double now);
  void StopDiscovery();

  // Every frame. `own` is this unit, `bios` the BIOS it runs. While STARTING the unit must not
  // run (its state is what was sent); from RUNNING on, Advance runs it instead of the app.
  void Poll(double now, ps::PocketStation& own, const std::vector<uint8_t>& bios);
  // Runs both units in lockstep up to real time (`wall`, seconds), this unit's buttons `buttons`.
  void Advance(ps::PocketStation& own, uint8_t buttons, double wall);
  bool Running() const { return state == RUNNING; }
  bool Busy() const { return state != OFF; }
  uint32_t Tick() const { return tick_; }   // 1/60 s steps run together so far

  static std::string LocalIp();

 private:
  void Send(uint8_t type, const std::vector<uint8_t>& payload);
  void Flush();
  void Handle(uint8_t type, const std::vector<uint8_t>& p, ps::PocketStation& own, const std::vector<uint8_t>& bios);
  void Start(ps::PocketStation& own, const std::vector<uint8_t>& bios);
  void StepPair(ps::PocketStation& a, ps::PocketStation& b, int64_t until_us);
  void Close();

  long long listen_ = -1, sock_ = -1, udp_ = -1, adv_ = -1;
  bool host_ = false, connected_ = false, sent_hello_ = false, sent_start_ = false;
  std::string name_;
  double next_advert_ = 0, connect_started_ = 0;
  std::vector<uint8_t> in_, out_;

  // The start: what each side sent.
  std::vector<uint8_t> my_state_, my_card_, their_state_, their_card_;
  bool got_start_ = false;

  // Lockstep: ticks of 1/60 s; presses are sent DELAY ticks ahead.
  static constexpr uint32_t DELAY = 6;
  uint32_t tick_ = 0, sent_upto_ = 0;
  std::vector<uint8_t> mine_, theirs_;
  int64_t pair_us_ = 0;          // emulated time since the start, microseconds
  uint64_t base_a_ = 0, base_b_ = 0;   // each unit's master tick at the start
  double start_wall_ = -1;
  std::vector<std::pair<uint32_t, uint32_t>> my_checks_, their_checks_;
};

}  // namespace irlink
