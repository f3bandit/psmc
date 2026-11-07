// MC/PS MemCard Pro: two units' infrared, over the local network.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
#include "link.hpp"

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <cstdlib>
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

constexpr uint32_t VERSION = 3;   // 3: presses 15 ticks ahead; 2: per-unit checks every half second, dumps
enum : uint8_t { M_HELLO = 1, M_START = 2, M_INPUT = 3, M_CHECK = 4, M_BYE = 5, M_PING = 6, M_PONG = 7, M_DUMP = 8 };
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

std::vector<uint8_t> Frame(uint8_t type, const std::string& text) {
  std::vector<uint8_t> f{type};
  Put32(f, (uint32_t)text.size());
  f.insert(f.end(), text.begin(), text.end());
  return f;
}

// The directed broadcast address of this machine's network (a.b.c.255 for a /24).
std::string SubnetBroadcast(const std::string& ip) {
  auto dot = ip.rfind('.');
  return dot == std::string::npos ? "" : ip.substr(0, dot) + ".255";
}

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
  for (auto& p : pending_) sclose(p.sock);
  pending_.clear();
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
  status = "Visible on the network as " + name_ + (ip.empty() ? std::string() : " (" + ip + ")");
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
  if (state == RUNNING) Log("LINK END  " + (why.empty() ? std::string("(no reason)") : why));
  if (replay_) {
    uint8_t rec[6] = {(uint8_t)tick_, (uint8_t)(tick_ >> 8), (uint8_t)(tick_ >> 16), (uint8_t)(tick_ >> 24), 0xFF, 0xFF};   // the end
    fwrite(rec, 1, 6, replay_);
    fclose(replay_);
    replay_ = nullptr;
  }
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
  if (!why.empty()) status = ended = why;
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
    if (std::string(ip) == LocalIp()) continue;   // ourselves
    auto it = std::find_if(found.begin(), found.end(), [&](const Found& f) { return f.ip == ip && f.port == port; });
    if (it == found.end()) found.push_back({nm, ip, port, now});
    else { it->seen = now; it->name = nm; }
  }
  Sweep(now);
  found.erase(std::remove_if(found.begin(), found.end(), [&](const Found& f) { return now - f.seen > 12; }), found.end());
}

// Broadcasts don't cross every network (many Wi-Fi routers drop them, and a PC may send them out
// of the wrong adapter), so also ask every address on this network directly, every few seconds:
// a unit answers a PING on its link port with its name.
void Link::Sweep(double now) {
  std::string me = LocalIp();
  if (probes_.empty() && now >= next_sweep_ && !me.empty()) {
    next_sweep_ = now + 5;
    std::string net = me.substr(0, me.rfind('.') + 1);
    for (int i = 1; i < 255; i++) {
      std::string ip = net + std::to_string(i);
      if (ip == me && !getenv("MCPS_LINK_SEE_SELF")) continue;   // (tests: two units on one machine)
      long long s = (long long)socket(AF_INET, SOCK_STREAM, 0);
      if (s < 0) break;
      nonblock(s);
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(PORT);
      inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
      int r = connect((sock_t)s, (sockaddr*)&a, sizeof a);
      if (r != 0 && !would_block()) { sclose(s); continue; }
      probes_.push_back({s, ip, now, false, {}});
    }
  }
  for (auto& p : probes_) {
    if (p.sock < 0) continue;
    if (!p.sent) {
      fd_set w;
      FD_ZERO(&w);
      FD_SET((sock_t)p.sock, &w);
      timeval tv{0, 0};
      if (select((int)p.sock + 1, nullptr, &w, nullptr, &tv) > 0) {
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt((sock_t)p.sock, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
        if (err) { sclose(p.sock); p.sock = -1; continue; }
        auto f = Frame(M_PING, name_);
        send((sock_t)p.sock, (const char*)f.data(), (int)f.size(), MSG_NOSIGNAL);
        p.sent = true;
      }
    }
    if (p.sent) {
      uint8_t b[512];
      int n = (int)recv((sock_t)p.sock, (char*)b, sizeof b, 0);
      if (n > 0) p.buf.insert(p.buf.end(), b, b + n);
      if (p.buf.size() >= 5 && p.buf.size() >= 5 + Get32(&p.buf[1])) {
        if (p.buf[0] == M_PONG) {
          std::string nm(p.buf.begin() + 5, p.buf.begin() + 5 + Get32(&p.buf[1]));
          auto it = std::find_if(found.begin(), found.end(), [&](const Found& f) { return f.ip == p.ip; });
          if (it == found.end()) found.push_back({nm, p.ip, PORT, now});
          else { it->seen = now; it->name = nm; }
        }
        sclose(p.sock);
        p.sock = -1;
      } else if (n == 0) { sclose(p.sock); p.sock = -1; }
    }
    if (p.sock >= 0 && now - p.since > 2) { sclose(p.sock); p.sock = -1; }
  }
  probes_.erase(std::remove_if(probes_.begin(), probes_.end(), [](const Probe& p) { return p.sock < 0; }), probes_.end());
}

void Link::StopDiscovery() {
  if (udp_ >= 0) sclose(udp_);
  udp_ = -1;
  for (auto& p : probes_) if (p.sock >= 0) sclose(p.sock);
  probes_.clear();
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
      std::string sub = SubnetBroadcast(LocalIp());   // and this network's own broadcast address
      if (!sub.empty() && inet_pton(AF_INET, sub.c_str(), &a.sin_addr) == 1)
        sendto((sock_t)adv_, msg, (int)strlen(msg), 0, (sockaddr*)&a, sizeof a);
    }
    sockaddr_in from{};
    socklen_t fl = sizeof from;
    long long c = (long long)accept((sock_t)listen_, (sockaddr*)&from, &fl);
    if (c >= 0) {
      nonblock(c);
      NoDelay(c);
      char ip[64];
      inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
      pending_.push_back({c, ip, now, {}});
    }
    // What a new connection wants: a PING (another unit looking around) gets our name; a HELLO
    // is a link.
    for (auto& p : pending_) {
      if (p.sock < 0) continue;
      uint8_t b[4096];
      int n = (int)recv((sock_t)p.sock, (char*)b, sizeof b, 0);
      if (n > 0) p.buf.insert(p.buf.end(), b, b + n);
      bool whole = p.buf.size() >= 5 && p.buf.size() >= 5 + Get32(&p.buf[1]);
      if (whole && p.buf[0] == M_PING) {
        auto f = Frame(M_PONG, name_);
        send((sock_t)p.sock, (const char*)f.data(), (int)f.size(), MSG_NOSIGNAL);
        sclose(p.sock);
        p.sock = -1;
      } else if (whole && p.buf[0] == M_HELLO && state == WAITING) {
        sock_ = p.sock;
        p.sock = -1;
        in_ = p.buf;
        peer_ip = p.ip;
        sclose(listen_);
        listen_ = -1;
        if (adv_ >= 0) sclose(adv_);
        adv_ = -1;
        connected_ = true;
        state = STARTING;
        status = "Linking with " + peer_ip + "...";
      } else if (whole || n == 0 || now - p.since > 3) {
        sclose(p.sock);
        p.sock = -1;
      }
    }
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Pending& p) { return p.sock < 0; }), pending_.end());
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
      if (p.size() == 12) their_checks_.push_back({Get32(&p[0]), Get32(&p[4]), Get32(&p[8])});
      break;
    case M_DUMP: {
      // The other side's two units at a check where we differ: kept beside the replay.
      if (!dump_base_.empty()) {
        if (FILE* f = fopen((dump_base_ + "-theirs.state").c_str(), "wb")) { fwrite(p.data(), 1, p.size(), f); fclose(f); }
        Log("their units at the mismatch saved: " + dump_base_.substr(dump_base_.find_last_of("/\\") + 1) + "-theirs.state");
      }
      got_dump_ = true;
      break;
    }
    case M_BYE: {
      std::string why(p.begin(), p.end());
      connected_ = false;   // don't answer
      Stop((peer_name.empty() ? peer_ip : peer_name) + " ended the link" + (why.empty() ? "" : ": " + why));
      return;
    }
  }
  if (state == STARTING && sent_start_ && got_start_) Start(own, bios);
}

void Link::Log(const std::string& line) {
  char ts[32];
  time_t now = time(nullptr);
  strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
  char pre[64];
  snprintf(pre, sizeof pre, "%s  t%-6u %6.2fs  ", ts, tick_, pair_us_ / 1e6);
  std::string l = pre + line;
  log.push_back(l);
  if (log.size() > 300) log.erase(log.begin(), log.begin() + 100);
  if (!log_path.empty()) {
    if (FILE* f = fopen(log_path.c_str(), "ab")) { fprintf(f, "%s\n", l.c_str()); fclose(f); }
  }
}

void Link::Watch(IrWatch& w, ps::PocketStation& u, const std::string& who) {
  // The transmitter switched on (its light then blinks with the data), or the receiver on.
  bool tx_on = (u.irda_static & 1) && !(u.irda_static & 2);
  int mode = tx_on ? 2 : u.IrListening() ? 1 : 0;
  if (mode != w.mode) {
    static const char* names[] = {"infrared off", "receiver on (listening)", "transmitter on"};
    Log(who + ": " + names[mode]);
    w.mode = mode;
  }
  bool light = u.IrSending();
  if (light != w.light) {
    if (w.first_us < 0) w.first_us = pair_us_;
    if (w.widths.size() < 48 && w.edge_us) w.widths.push_back((int)(w.light ? pair_us_ - w.edge_us : -(pair_us_ - w.edge_us)));
    if (light) w.pulses++;
    w.edge_us = pair_us_;
    w.last_us = pair_us_;
    w.light = light;
  }
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
  base_a_ = a.t;
  base_b_ = b.t;
  pair_us_ = 0;
  tick_ = 0;
  mine_.assign(DELAY, 0);
  theirs_.assign(DELAY, 0);
  sent_upto_ = DELAY;
  start_wall_ = -1;
  state = RUNNING;
  status = "Linked with " + peer_name;
  wa_ = IrWatch();
  wb_ = IrWatch();
  mismatch_ = got_dump_ = false;
  last_match_ = 0;
  snaps_.clear();
  own_hist_.clear();
  dump_base_.clear();
  ir_session_ = false;
  stall_since_ = -1;
  auto crc = [](ps::PocketStation& u) { auto st = u.SaveState(); return Crc32(st.data(), st.size()); };
  char line[256];
  snprintf(line, sizeof line, "LINK START  A = %s (waited for the link), B = %s (joined)  peer %s  start states A %08x B %08x",
           NameA().c_str(), NameB().c_str(), peer_ip.c_str(), crc(a), crc(b));
  Log("");
  Log(line);
  if (!replay_dir.empty()) {
    char fn[64];
    time_t now = time(nullptr);
    strftime(fn, sizeof fn, "link-%Y%m%d-%H%M%S.mcpslink", localtime(&now));
    replay_ = fopen((replay_dir + fn).c_str(), "wb");
    dump_base_ = replay_dir + std::string(fn).substr(0, strlen(fn) - 9);   // without .mcpslink
    if (replay_) {
      auto put = [&](const std::vector<uint8_t>& v) {
        uint32_t n = (uint32_t)v.size();
        fwrite(&n, 4, 1, replay_);
        fwrite(v.data(), 1, v.size(), replay_);
      };
      uint32_t magic = REPLAY_MAGIC;
      fwrite(&magic, 4, 1, replay_);
      std::string names = NameA() + "\n" + NameB();
      put(std::vector<uint8_t>(names.begin(), names.end()));
      put(a.SaveState());
      put(a.Card());
      put(b.SaveState());
      put(b.Card());
      fflush(replay_);
      Log("replay: " + std::string(fn));
    }
  }
}

// A unit with its infrared in use: transmitter or receiver switched on.
bool IrOn(const ps::PocketStation& u) {
  return ((u.irda_static & 1) && !(u.irda_static & 2)) || u.IrListening();
}

static uint64_t UsToTicks(int64_t us) {
  // MASTER_HZ * 65536 / 1e6 is 8187281408 / 15625 ticks per microsecond; split so 64 bits do.
  static_assert((uint64_t)ps::MASTER_HZ * 65536u * 15625u == 8187281408ull * 1000000u, "master clock changed");
  uint64_t q = (uint64_t)us / 15625, r = (uint64_t)us % 15625;
  return q * 8187281408ull + r * 8187281408ull / 15625;
}

void RunPair(ps::PocketStation& a, ps::PocketStation& b, uint64_t base_a, uint64_t base_b, int64_t& pair_us, int64_t until_us) {
  while (pair_us < until_us) {
    bool ir = IrOn(a) || IrOn(b);
    int64_t s = ir ? std::min<int64_t>(100, until_us - pair_us) : until_us - pair_us;
    bool la = a.IrSending(), lb = b.IrSending();
    a.ir_light = lb;
    b.ir_light = la;
    pair_us += s;
    // Whole master ticks from whole microseconds: no floating point, so a PC and a phone land on
    // exactly the same cycle.
    uint64_t d = UsToTicks(pair_us);
    a.RunUntil(base_a + d);
    b.RunUntil(base_b + d);
  }
}

void Link::StepPair(ps::PocketStation& a, ps::PocketStation& b, int64_t until_us) {
  // As two units face to face: each one's receiver sees the other's emitter. While either has
  // its infrared on, in 100 us slices (a bit is about 2 ms); otherwise a whole tick at once.
  while (pair_us_ < until_us) {
    bool ir = IrOn(a) || IrOn(b);
    int64_t next = ir ? std::min<int64_t>(pair_us_ + 100, until_us) : until_us;
    RunPair(a, b, base_a_, base_b_, pair_us_, next);
    if (ir || IrOn(a) || IrOn(b)) {
      if (!ir_session_) {
        ir_session_ = true;
        session_us_ = pair_us_;
        wa_.pulses = wb_.pulses = 0;
        wa_.first_us = wb_.first_us = -1;
        wa_.widths.clear();
        wb_.widths.clear();
        wa_.edge_us = wb_.edge_us = 0;
        Log("IR session starts");
      }
      Watch(wa_, a, "A " + NameA());
      Watch(wb_, b, "B " + NameB());
    } else if (ir_session_) {
      Watch(wa_, a, "A " + NameA());
      Watch(wb_, b, "B " + NameB());
      ir_session_ = false;
      auto sum = [&](IrWatch& w, const std::string& who) {
        char l[160];
        snprintf(l, sizeof l, "  %s sent %d pulses%s", who.c_str(), w.pulses,
                 w.pulses ? (", over " + std::to_string((w.last_us - w.first_us) / 1000) + " ms").c_str() : "");
        Log(l);
        if (!w.widths.empty()) {
          std::string t = "    first light (+on/-off, us):";
          for (int x : w.widths) t += " " + std::to_string(x);
          Log(t);
        }
      };
      Log("IR session ends after " + std::to_string((pair_us_ - session_us_) / 1000) + " ms");
      sum(wa_, "A " + NameA());
      sum(wb_, "B " + NameB());
    }
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
  while (tick_ < target && n < 8 && !mismatch_) {
    if (theirs_.size() <= tick_) { stalled = true; break; }
    uint8_t ba = host_ ? mine_[tick_] : theirs_[tick_], bb = host_ ? theirs_[tick_] : mine_[tick_];
    auto press = [&](IrWatch& w, uint8_t v, const std::string& who) {
      if (v == w.buttons) return;
      static const char* nm[] = {"Fire", "Right", "Left", "Down", "Up"};
      std::string l = who + " presses";
      for (int i = 0; i < 5; i++) if (v & ~w.buttons & (1 << i)) l += std::string(" ") + nm[i];
      if (v & ~w.buttons) Log(l);
      w.buttons = v;
    };
    if (replay_ && (ba != wa_.buttons || bb != wb_.buttons)) {
      uint8_t rec[6] = {(uint8_t)tick_, (uint8_t)(tick_ >> 8), (uint8_t)(tick_ >> 16), (uint8_t)(tick_ >> 24), ba, bb};
      fwrite(rec, 1, 6, replay_);
      fflush(replay_);
    }
    press(wa_, ba, "A " + NameA());
    press(wb_, bb, "B " + NameB());
    a.SetButtons(ba);
    b.SetButtons(bb);
    tick_++;
    StepPair(a, b, (int64_t)tick_ * 1000000 / 60);
    n++;
    // This side's own unit after every tick, for the last second and a half: sent with a dump,
    // so a replay can find the very tick where one side's own unit left the common path.
    own_hist_.push_back({tick_, own.SaveState()});
    if (own_hist_.size() > 90) own_hist_.erase(own_hist_.begin());
    if (tick_ % 30 == 0) {
      // Step check every half second, one value per unit (its whole state), so a difference
      // says which unit and when. The states are kept a while in case the other side differs.
      auto sa = a.SaveState(), sb = b.SaveState();
      uint32_t ca = Crc32(sa.data(), sa.size()), cb = Crc32(sb.data(), sb.size());
      my_checks_.push_back({tick_, ca, cb});
      snaps_.push_back({tick_, std::move(sa), std::move(sb)});
      if (snaps_.size() > 40) snaps_.erase(snaps_.begin());
      std::vector<uint8_t> p;
      Put32(p, tick_);
      Put32(p, ca);
      Put32(p, cb);
      Send(M_CHECK, p);
    }
  }
  if (stalled && stall_since_ < 0) stall_since_ = wall;
  if (!stalled && stall_since_ >= 0) {
    if (wall - stall_since_ > 0.3) Log("waited " + std::to_string((int)((wall - stall_since_) * 1000)) + " ms for " + peer_name);
    stall_since_ = -1;
  }
  // Behind real time by more than half a second (the other side was slow): don't race to catch
  // up, carry on from here.
  if (target - tick_ > 30) start_wall_ += (target - tick_ - 30) / 60;
  peer->TakeAudio();
  // The two copies must match.
  while (!my_checks_.empty() && !their_checks_.empty() && !mismatch_) {
    auto m = my_checks_.front(), t = their_checks_.front();
    if (m.tick < t.tick) { my_checks_.erase(my_checks_.begin()); continue; }
    if (t.tick < m.tick) { their_checks_.erase(their_checks_.begin()); continue; }
    my_checks_.erase(my_checks_.begin());
    their_checks_.erase(their_checks_.begin());
    if (m.a != t.a || m.b != t.b) {
      char l[200];
      snprintf(l, sizeof l, "step check at t%u: A %08x vs %08x %s, B %08x vs %08x %s  (last match t%u)", m.tick, m.a, t.a,
               m.a == t.a ? "ok" : "DIFFERS", m.b, t.b, m.b == t.b ? "ok" : "DIFFERS", last_match_);
      Log(l);
      // Both units as they were at that check, ours to a file and to the other side, theirs back
      // from them: the two versions to compare.
      mismatch_ = true;
      mismatch_wall_ = wall;
      for (auto& sn : snaps_)
        if (sn.tick == m.tick) {
          std::vector<uint8_t> d;
          Put32(d, sn.tick);
          Put32(d, (uint32_t)sn.a.size());
          d.insert(d.end(), sn.a.begin(), sn.a.end());
          Put32(d, (uint32_t)sn.b.size());
          d.insert(d.end(), sn.b.begin(), sn.b.end());
          d.push_back(host_ ? 'A' : 'B');   // which unit is this side's own
          Put32(d, (uint32_t)own_hist_.size());
          for (auto& h : own_hist_) {
            Put32(d, h.first);
            Put32(d, (uint32_t)h.second.size());
            d.insert(d.end(), h.second.begin(), h.second.end());
          }
          if (!dump_base_.empty()) {
            dump_base_ += "-t" + std::to_string(sn.tick);
            if (FILE* f = fopen((dump_base_ + "-ours.state").c_str(), "wb")) { fwrite(d.data(), 1, d.size(), f); fclose(f); }
          }
          Send(M_DUMP, d);
        }
      break;
    }
    last_match_ = m.tick;
    if (m.tick % 1200 == 0) Log("step check t" + std::to_string(m.tick) + ": both sides match");
  }
  if (mismatch_ && (got_dump_ || wall - mismatch_wall_ > 3)) {
    Stop("The two units got out of step; the link was ended (each keeps its own)");
    return;
  }
  Flush();
}

}  // namespace irlink
