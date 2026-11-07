// MC/PS MemCard Pro: watching the app on a phone from the PC, like Phosphor Link does for the SS1.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// The phone keeps a running log (what it's doing, every press, link events, a status line every
// second) and serves it on TCP port 7552. The Windows app finds phones on the network (the link's
// search), connects, and writes what it gets to remote/<phone name>/ beside its card: app.log,
// plus any files the phone sends. A request.txt dropped in that folder is passed to the phone as a
// command ("snapshot": the unit's state, card and screen come back as files); it's renamed to
// request.done once sent.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace monitor {

constexpr int PORT = 7552;

// On the phone: the log and the port the PC reads it from.
class Server {
 public:
  ~Server();
  void Start(const std::string& name);
  void Log(const std::string& line);   // timestamped; kept (the last 3000) for a PC that connects later
  void SendFile(const std::string& name, const std::vector<uint8_t>& data);
  // Every frame: takes a PC's connection, sends what's waiting; returns the PC's commands.
  std::vector<std::string> Poll();
  bool Connected() const { return client_ >= 0; }

 private:
  void Queue(uint8_t type, const std::vector<uint8_t>& p);
  long long listen_ = -1, client_ = -1;
  std::string name_;
  std::deque<std::string> backlog_;
  std::vector<uint8_t> out_, in_;
};

// On the PC: reads every phone it can find.
class Client {
 public:
  ~Client();
  // `units`: ip -> name of the units found on the network; `dir`: where remote/ goes.
  void Poll(double now, const std::map<std::string, std::string>& units, const std::string& dir);

 private:
  struct Conn {
    long long sock = -1;
    bool connected = false;
    double since = 0;
    std::string name, ip;
    std::vector<uint8_t> in, out;
  };
  std::map<std::string, Conn> conns_;   // by ip
  std::map<std::string, double> retry_;
};

}  // namespace monitor
