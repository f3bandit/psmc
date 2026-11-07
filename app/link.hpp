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
#include <cstdio>
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

// Runs two units face to face up to `until_us` microseconds after their start ticks: each one's
// receiver sees the other's light, in 100 us slices while either has its infrared on. The link and
// tools/link_replay both run units with this, so a replay is the same to the cycle.
void RunPair(ps::PocketStation& a, ps::PocketStation& b, uint64_t base_a, uint64_t base_b, int64_t& pair_us, int64_t until_us);
bool IrOn(const ps::PocketStation& u);
constexpr uint32_t REPLAY_MAGIC = 0x4C53434D;   // "MCSL"

class Link {
 public:
  enum State { OFF, WAITING, CONNECTING, STARTING, RUNNING };

  ~Link();
  State state = OFF;
  std::string status;          // one line for the UI: what's happening
  std::string ended;           // why the last link ended (empty: none yet)
  std::string peer_name, peer_ip;
  std::vector<Found> found;    // units on the network waiting for a link
  bool stalled = false;        // running, but waiting on the other side's presses
  std::unique_ptr<ps::PocketStation> peer;   // the other unit, run here in step with ours

  // Be visible on the network and take links from other units (the app does this all along).
  bool Host(const std::string& name);
  bool Join(const std::string& ip, int port, const std::string& name);
  void Stop(const std::string& why);                               // end it (tells the other side)
  // Look for other units (fills `found`): their broadcasts, and asking every address on this
  // network. Call while the Link page is up.
  void Discover(double now);
  void StopDiscovery();

  // Every frame. `own` is this unit, `bios` the BIOS it runs. While STARTING the unit must not
  // run (its state is what was sent); from RUNNING on, Advance runs it instead of the app.
  void Poll(double now, ps::PocketStation& own, const std::vector<uint8_t>& bios);
  // Runs both units in lockstep up to real time (`wall`, seconds), this unit's buttons `buttons`.
  void Advance(ps::PocketStation& own, uint8_t buttons, double wall);
  bool Running() const { return state == RUNNING; }
  bool Busy() const { return state != OFF; }               // anything going on, waiting included
  bool Linking() const { return state >= CONNECTING; }    // a link being made, or running
  uint32_t Tick() const { return tick_; }   // 1/60 s steps run together so far

  // The monitor: what happened on the link, for both units (this side runs both): presses,
  // each unit's infrared (off / listening / sending), the light pulses each way with their
  // timing, stalls, the step checks, how it ended. The last lines are kept here; with
  // `log_path` set every line is also appended to that file.
  std::vector<std::string> log;
  std::string log_path;
  // The replay: with `replay_dir` set, each link is saved there as link-YYYYMMDD-HHMMSS.mcpslink:
  // both units and cards as they started, then every press of either, by tick. The link is exact,
  // so tools/link_replay runs it again cycle for cycle (screens, infrared, everything).
  std::string replay_dir;

  static std::string LocalIp();

 private:
  void Send(uint8_t type, const std::vector<uint8_t>& payload);
  void Flush();
  void Handle(uint8_t type, const std::vector<uint8_t>& p, ps::PocketStation& own, const std::vector<uint8_t>& bios);
  void Start(ps::PocketStation& own, const std::vector<uint8_t>& bios);
  void StepPair(ps::PocketStation& a, ps::PocketStation& b, int64_t until_us);
  void Close();
  void Sweep(double now);
  void Log(const std::string& line);
  FILE* replay_ = nullptr;
  std::string NameA() const { return host_ ? name_ : peer_name; }
  std::string NameB() const { return host_ ? peer_name : name_; }
  // Monitor state: per unit, its IR mode and the pulses of the current infrared session.
  struct IrWatch {
    int mode = 0;                       // 0 off, 1 listening, 2 sending
    bool light = false;
    uint8_t buttons = 0;
    int pulses = 0;
    int64_t first_us = -1, last_us = 0, edge_us = 0;
    std::vector<int> widths;            // the first pulses and gaps, microseconds (+on, -off)
  } wa_, wb_;
  bool ir_session_ = false;
  int64_t session_us_ = 0;
  double stall_since_ = -1;
  void Watch(IrWatch& w, ps::PocketStation& u, const std::string& who);
  struct Pending { long long sock; std::string ip; double since; std::vector<uint8_t> buf; };
  struct Probe { long long sock; std::string ip; double since; bool sent; std::vector<uint8_t> buf; };
  std::vector<Pending> pending_;
  std::vector<Probe> probes_;
  double next_sweep_ = 0;

  long long listen_ = -1, sock_ = -1, udp_ = -1, adv_ = -1;
  bool host_ = false, connected_ = false, sent_hello_ = false, sent_start_ = false;
  std::string name_;
  double next_advert_ = 0, connect_started_ = 0;
  std::vector<uint8_t> in_, out_;

  // The start: what each side sent.
  std::vector<uint8_t> my_state_, my_card_, their_state_, their_card_;
  bool got_start_ = false;

  // Lockstep: ticks of 1/60 s; presses are sent DELAY ticks (250 ms) ahead, enough to ride over
  // the few hundred ms a phone on Wi-Fi can hold packets back.
  static constexpr uint32_t DELAY = 15;
  uint32_t tick_ = 0, sent_upto_ = 0;
  std::vector<uint8_t> mine_, theirs_;
  int64_t pair_us_ = 0;          // emulated time since the start, microseconds
  uint64_t base_a_ = 0, base_b_ = 0;   // each unit's master tick at the start
  double start_wall_ = -1;
  struct Check { uint32_t tick, a, b; };
  std::vector<Check> my_checks_, their_checks_;
  struct Snap { uint32_t tick; std::vector<uint8_t> a, b; };
  std::vector<Snap> snaps_;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> own_hist_;
  bool mismatch_ = false, got_dump_ = false;
  double mismatch_wall_ = 0;
  uint32_t last_match_ = 0;
  std::string dump_base_;
};

}  // namespace irlink
