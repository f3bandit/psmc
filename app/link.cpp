// MC/PS MemCard Pro: two units' infrared, over the local network.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
#include "link.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
static void sclose(long long s) { closesocket((SOCKET)s); }
static void nonblock(long long s) { u_long one = 1; ioctlsocket((SOCKET)s, FIONBIO, &one); }
static bool would_block() { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
#define MSG_NOSIGNAL 0
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
static void sclose(long long s) { close((int)s); }
static void nonblock(long long s) { fcntl((int)s, F_SETFL, O_NONBLOCK); }
static bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }
#endif

namespace irlink {

namespace {

constexpr uint32_t VERSION = 1;
enum : uint8_t { M_HELLO = 1, M_START = 2, M_INPUT = 3, M_CHECK = 4, M_BYE = 5 };
const char* ADVERT = "MCPSLINK1";

void StartSockets() {
#ifdef _WIN32
  static bool done = false;
  if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; }
#endif
}

uint32_t Crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
uint32_t Get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void NoDelay(long long s) {
  int one = 1;
  setsockopt((sock_t)s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
}

}  // namespace

Link::~Link() { Close(); StopDiscovery(); }

std::string Link::LocalIp() {
  // The address other units reach this one at: the one a packet out would leave from (nothing
  // is sent; connecting a UDP socket only picks the route).
  StartSockets();
  long long s = (long long)socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return "";
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(9);
  inet_pton(AF_INET, "192.168.1.1", &a.sin_addr);
  std::string ip;
  if (connect((sock_t)s, (sockaddr*)&a, sizeof a) == 0) {
    sockaddr_in me{};
    socklen_t n = sizeof me;
    if (getsockname((sock_t)s, (sockaddr*)&me, &n) == 0) {
      char b[64];
      inet_ntop(AF_INET, &me.sin_addr, b, sizeof b);
      ip = b;
    }
  }
  sclose(s);
  return ip == "0.0.0.0" ? "" : ip;
}

void Link::Close() {
  if (sock_ >= 0) sclose(sock_);
  if (listen_ >= 0) sclose(listen_);
  if (adv_ >= 0) sclose(adv_);
  sock_ = listen_ = adv_ = -1;
  connected_ = sent_hello_ = sent_start_ = got_start_ = false;
  in_.clear();
  out_.clear();
}

bool Link::Host(const std::string& name) {
  StartSockets();
  Stop("");
  name_ = name;
  long long s = (long long)socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { status = "Couldn't open the network"; return false; }
  int one = 1;
  setsockopt((sock_t)s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind((sock_t)s, (sockaddr*)&a, sizeof a) != 0 || listen((sock_t)s, 1) != 0) {
    sclose(s);
    status = "Port " + std::to_string(PORT) + " is in use";
    return false;
  }
  nonblock(s);
  listen_ = s;
  // The advert: a UDP broadcast every second.
  long long u = (long long)socket(AF_INET, SOCK_DGRAM, 0);
  if (u >= 0) {
    setsockopt((sock_t)u, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
    nonblock(u);
    adv_ = u;
  }
  host_ = true;
  state = WAITING;
  next_advert_ = 0;
  std::string ip = LocalIp();
  status = "Waiting for a link" + (ip.empty() ? std::string() : " at " + ip);
  return true;
}

bool Link::Join(const std::string& ip, int port, const std::string& name) {
  StartSockets();
  Stop("");
  name_ = name;
  long long s = (long long)socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { status = "Couldn't open the network"; return false; }
  nonblock(s);
  NoDelay(s);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) { sclose(s); status = "That isn't an IP address"; return false; }
  int r = connect((sock_t)s, (sockaddr*)&a, sizeof a);
  if (r != 0 && !would_block()) { sclose(s); status = "Couldn't reach " + ip; return false; }
  sock_ = s;
  host_ = false;
  peer_ip = ip;
  state = CONNECTING;
  connect_started_ = -1;
  status = "Linking with " + ip + "...";
  return true;
}

void Link::Stop(const std::string& why) {
  if (connected_ && sock_ >= 0) {
    // Say goodbye if the socket takes it; never wait, never fail over it.
    connected_ = false;
    std::vector<uint8_t> p(why.begin(), why.end());
    Send(M_BYE, p);
    send((sock_t)sock_, (const char*)out_.data(), (int)out_.size(), MSG_NOSIGNAL);
  }
  Close();
  state = OFF;
  stalled = false;
  host_ = false;
  if (!why.empty()) status = why;
  tick_ = sent_upto_ = 0;
  mine_.clear();
  theirs_.clear();
  my_checks_.clear();
  their_checks_.clear();
  my_state_.clear();
  my_card_.clear();
  peer.reset();
  their_state_.clear();
  their_card_.clear();
  start_wall_ = -1;
}

void Link::Discover(double now) {
  StartSockets();
  if (udp_ < 0) {
    long long u = (long long)socket(AF_INET, SOCK_DGRAM, 0);
    if (u < 0) return;
    int one = 1;
    setsockopt((sock_t)u, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    setsockopt((sock_t)u, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind((sock_t)u, (sockaddr*)&a, sizeof a) != 0) { sclose(u); return; }
    nonblock(u);
    udp_ = u;
  }
  char b[256];
  for (;;) {
    sockaddr_in from{};
    socklen_t fl = sizeof from;
    int n = (int)recvfrom((sock_t)udp_, b, sizeof b - 1, 0, (sockaddr*)&from, &fl);
    if (n <= 0) break;
    b[n] = 0;
    char tag[16] = {}, nm[128] = {};
    int port = 0;
    if (sscanf(b, "%15s %d %127[^\n]", tag, &port, nm) < 2 || strcmp(tag, ADVERT) != 0) continue;
    char ip[64];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
    if (state == WAITING && std::string(nm) == name_ && std::string(ip) == LocalIp()) continue;   // ourselves
    auto it = std::find_if(found.begin(), found.end(), [&](const Found& f) { return f.ip == ip && f.port == port; });
    if (it == found.end()) found.push_back({nm, ip, port, now});
    else { it->seen = now; it->name = nm; }
  }
  found.erase(std::remove_if(found.begin(), found.end(), [&](const Found& f) { return now - f.seen > 4; }), found.end());
}

void Link::StopDiscovery() {
  if (udp_ >= 0) sclose(udp_);
  udp_ = -1;
  found.clear();
}

void Link::Send(uint8_t type, const std::vector<uint8_t>& payload) {
  out_.push_back(type);
  Put32(out_, (uint32_t)payload.size());
  out_.insert(out_.end(), payload.begin(), payload.end());
}

void Link::Flush() {
  while (!out_.empty() && sock_ >= 0) {
    int n = (int)send((sock_t)sock_, (const char*)out_.data(), (int)std::min<size_t>(out_.size(), 65536), MSG_NOSIGNAL);
    if (n > 0) { out_.erase(out_.begin(), out_.begin() + n); continue; }
    if (n < 0 && would_block()) return;
    connected_ = false;   // the socket is gone: no goodbye
    Stop("The link with " + (peer_name.empty() ? peer_ip : peer_name) + " dropped");
    return;
  }
}

void Link::Poll(double now, ps::PocketStation& own, const std::vector<uint8_t>& bios) {
  if (state == OFF) return;
  if (state == WAITING) {
    if (adv_ >= 0 && now >= next_advert_) {
      next_advert_ = now + 1.0;
      char msg[200];
      snprintf(msg, sizeof msg, "%s %d %s", ADVERT, PORT, name_.c_str());
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(PORT);
      a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
      sendto((sock_t)adv_, msg, (int)strlen(msg), 0, (sockaddr*)&a, sizeof a);
    }
    sockaddr_in from{};
    socklen_t fl = sizeof from;
    long long c = (long long)accept((sock_t)listen_, (sockaddr*)&from, &fl);
    if (c >= 0) {
      nonblock(c);
      NoDelay(c);
      sock_ = c;
      char ip[64];
      inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
      peer_ip = ip;
      sclose(listen_);
      listen_ = -1;
      if (adv_ >= 0) sclose(adv_);
      adv_ = -1;
      connected_ = true;
      state = STARTING;
      status = "Linking with " + peer_ip + "...";
    }
  }
  if (state == CONNECTING) {
    if (connect_started_ < 0) connect_started_ = now;
    // Connected once the socket is writable without an error.
    fd_set w;
    FD_ZERO(&w);
    FD_SET((sock_t)sock_, &w);
    timeval tv{0, 0};
    if (select((int)sock_ + 1, nullptr, &w, nullptr, &tv) > 0) {
      int err = 0;
      socklen_t el = sizeof err;
      getsockopt((sock_t)sock_, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
      if (err) { Stop("Couldn't reach " + peer_ip); return; }
      connected_ = true;
      state = STARTING;
    } else if (now - connect_started_ > 8) {
      Stop("No answer from " + peer_ip);
      return;
    }
  }
  if (!connected_) return;

  if (!sent_hello_) {
    std::vector<uint8_t> p;
    Put32(p, VERSION);
    Put32(p, Crc32(bios.data(), bios.size()));
    p.insert(p.end(), name_.begin(), name_.end());
    Send(M_HELLO, p);
    sent_hello_ = true;
  }

  uint8_t b[65536];
  bool closed = false;
  for (;;) {
    int n = (int)recv((sock_t)sock_, (char*)b, sizeof b, 0);
    if (n > 0) { in_.insert(in_.end(), b, b + n); continue; }
    if (n < 0 && would_block()) break;
    closed = true;   // read what came before it (a goodbye says why)
    break;
  }
  while (in_.size() >= 5) {
    uint32_t len = Get32(&in_[1]);
    if (len > 4 * 1024 * 1024) { Stop("The other side sent something odd"); return; }
    if (in_.size() < 5 + len) break;
    uint8_t type = in_[0];
    std::vector<uint8_t> p(in_.begin() + 5, in_.begin() + 5 + len);
    in_.erase(in_.begin(), in_.begin() + 5 + len);
    Handle(type, p, own, bios);
    if (state == OFF) return;
  }
  if (closed) {
    connected_ = false;
    Stop("The link with " + (peer_name.empty() ? peer_ip : peer_name) + " ended");
    return;
  }
  Flush();
}

void Link::Handle(uint8_t type, const std::vector<uint8_t>& p, ps::PocketStation& own, const std::vector<uint8_t>& bios) {
  switch (type) {
    case M_HELLO: {
      if (p.size() < 8) { Stop("The other side sent something odd"); return; }
      if (Get32(&p[0]) != VERSION) { Stop("The other side runs a different version of the app"); return; }
      if (Get32(&p[4]) != Crc32(bios.data(), bios.size())) {
        Stop("The other side has a different BIOS: both need the same one");
        return;
      }
      peer_name.assign(p.begin() + 8, p.end());
      if (peer_name.empty()) peer_name = peer_ip;
      status = "Linking with " + peer_name + "...";
      // Our unit as it is now: from here it waits until both copies start together.
      my_state_ = own.SaveState();
      my_card_ = own.Card();
      std::vector<uint8_t> s;
      Put32(s, (uint32_t)my_state_.size());
      s.insert(s.end(), my_state_.begin(), my_state_.end());
      s.insert(s.end(), my_card_.begin(), my_card_.end());
      Send(M_START, s);
      sent_start_ = true;
      break;
    }
    case M_START: {
      if (p.size() < 4) { Stop("The other side sent something odd"); return; }
      uint32_t n = Get32(&p[0]);
      if (p.size() != 4 + n + 131072) { Stop("The other side sent something odd"); return; }
      their_state_.assign(p.begin() + 4, p.begin() + 4 + n);
      their_card_.assign(p.begin() + 4 + n, p.end());
      got_start_ = true;
      break;
    }
    case M_INPUT:
      if (p.size() == 5 && Get32(&p[0]) == theirs_.size()) theirs_.push_back(p[4]);
      break;
    case M_CHECK:
      if (p.size() == 8) their_checks_.push_back({Get32(&p[0]), Get32(&p[4])});
      break;
    case M_BYE: {
      std::string why(p.begin(), p.end());
      connected_ = false;   // don't answer
      Stop((peer_name.empty() ? peer_ip : peer_name) + " ended the link" + (why.empty() ? "" : ": " + why));
      return;
    }
  }
  if (state == STARTING && sent_start_ && got_start_) Start(own, bios);
}

void Link::Start(ps::PocketStation& own, const std::vector<uint8_t>& bios) {
  // Both sides load both units from what was sent, the same way, so the copies match exactly.
  own.LoadCard(my_card_);
  own.LoadState(my_state_);
  peer = std::make_unique<ps::PocketStation>();
  peer->LoadBios(bios);
  peer->LoadCard(their_card_);
  if (!peer->LoadState(their_state_)) { Stop("Couldn't load the other unit"); return; }
  own.ir_light = peer->ir_light = false;
  ps::PocketStation& a = host_ ? own : *peer;
  ps::PocketStation& b = host_ ? *peer : own;
  base_a_ = a.Now();
  base_b_ = b.Now();
  pair_us_ = 0;
  tick_ = 0;
  mine_.assign(DELAY, 0);
  theirs_.assign(DELAY, 0);
  sent_upto_ = DELAY;
  start_wall_ = -1;
  state = RUNNING;
  status = "Linked with " + peer_name;
}

void Link::StepPair(ps::PocketStation& a, ps::PocketStation& b, int64_t until_us) {
  // As two units face to face: each one's receiver sees the other's emitter. While either has
  // its infrared on, in 100 us slices (a bit is about 2 ms); otherwise a whole tick at once.
  while (pair_us_ < until_us) {
    bool ir = a.IrSending() || a.IrListening() || b.IrSending() || b.IrListening();
    int64_t s = ir ? std::min<int64_t>(100, until_us - pair_us_) : until_us - pair_us_;
    bool la = a.IrSending(), lb = b.IrSending();
    a.ir_light = lb;
    b.ir_light = la;
    pair_us_ += s;
    double t = pair_us_ * 1e-6;
    a.RunFor(base_a_ + t - a.Now());
    b.RunFor(base_b_ + t - b.Now());
  }
}

void Link::Advance(ps::PocketStation& own, uint8_t buttons, double wall) {
  if (state != RUNNING) return;
  if (start_wall_ < 0) start_wall_ = wall;
  // Our presses, DELAY ticks ahead.
  while (sent_upto_ <= tick_ + DELAY) {
    mine_.push_back(buttons);
    std::vector<uint8_t> p;
    Put32(p, sent_upto_);
    p.push_back(buttons);
    Send(M_INPUT, p);
    sent_upto_++;
  }
  ps::PocketStation& a = host_ ? own : *peer;
  ps::PocketStation& b = host_ ? *peer : own;
  double target = (wall - start_wall_) * 60;
  int n = 0;
  stalled = false;
  while (tick_ < target && n < 8) {
    if (theirs_.size() <= tick_) { stalled = true; break; }
    a.SetButtons(host_ ? mine_[tick_] : theirs_[tick_]);
    b.SetButtons(host_ ? theirs_[tick_] : mine_[tick_]);
    tick_++;
    StepPair(a, b, (int64_t)tick_ * 1000000 / 60);
    n++;
    if (tick_ % 120 == 0) {
      uint32_t c = Crc32(a.sram.data(), a.sram.size());
      c = Crc32(b.sram.data(), b.sram.size(), c);
      my_checks_.push_back({tick_, c});
      std::vector<uint8_t> p;
      Put32(p, tick_);
      Put32(p, c);
      Send(M_CHECK, p);
    }
  }
  // Behind real time by more than half a second (the other side was slow): don't race to catch
  // up, carry on from here.
  if (target - tick_ > 30) start_wall_ += (target - tick_ - 30) / 60;
  peer->TakeAudio();
  // The two copies must match.
  while (!my_checks_.empty() && !their_checks_.empty()) {
    auto m = my_checks_.front(), t = their_checks_.front();
    if (m.first < t.first) { my_checks_.erase(my_checks_.begin()); continue; }
    if (t.first < m.first) { their_checks_.erase(their_checks_.begin()); continue; }
    my_checks_.erase(my_checks_.begin());
    their_checks_.erase(their_checks_.begin());
    if (m.second != t.second) { Stop("The two units got out of step; the link was ended (each keeps its own)"); return; }
  }
  Flush();
}

}  // namespace irlink
