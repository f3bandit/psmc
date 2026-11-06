// MC/PS MemCard Pro: the virtual card on the network.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// A console gives a card about 100 us to acknowledge each byte, which no network round trip
// meets, so whole commands come in and run through the emulated card bus here at console timing.
// Line protocol, replies as in Phosphor's API:
//   card_raw HEX   {"ok":true,"count":N,"stop":"none"|"no card"|"no ack","data":"<hex>"}
//   status         {"ok":true,"docked":true|false,"led":0|1}
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace net {

struct Server {
  // Runs one whole command on the card bus and returns the bytes that came back.
  std::function<std::vector<unsigned char>(const std::vector<unsigned char>& tx, bool& complete)> card_raw;
  std::function<std::string()> status;

  bool Open(int port);
  void Poll();                 // non-blocking: accept, read, answer
  void Close();
  bool Listening() const { return srv >= 0; }
  bool Connected() const { return cli >= 0; }
  std::string Peer() const { return peer; }
  double last_command = -1;    // seconds, by the caller's clock (set by Poll's caller)
  int commands = 0;

  long long srv = -1, cli = -1;
  std::string in, peer;
  int port = 0;
};

}  // namespace net
