// MC/PS MemCard Pro: watching the app on a phone from the PC.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
#include "monitor.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

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
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
typedef int sock_t;
static void sclose(long long s) { close((int)s); }
static void nonblock(long long s) { fcntl((int)s, F_SETFL, O_NONBLOCK); }
static bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }
#endif

namespace fs = std::filesystem;

namespace monitor {

namespace {

enum : uint8_t { T_HELLO = 'H', T_LOG = 'L', T_FILE = 'F', T_CMD = 'C' };

void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
uint32_t Get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void Frame(std::vector<uint8_t>& out, uint8_t type, const std::vector<uint8_t>& p) {
  out.push_back(type);
  Put32(out, (uint32_t)p.size());
  out.insert(out.end(), p.begin(), p.end());
}

// Sends what it can; false if the connection is gone.
bool Flush(long long s, std::vector<uint8_t>& out) {
  while (!out.empty()) {
    int n = (int)send((sock_t)s, (const char*)out.data(), (int)std::min<size_t>(out.size(), 65536), MSG_NOSIGNAL);
    if (n > 0) { out.erase(out.begin(), out.begin() + n); continue; }
    return n < 0 && would_block();
  }
  return true;
}

// Reads what's there; false if the connection is gone.
bool Fill(long long s, std::vector<uint8_t>& in) {
  uint8_t b[65536];
  for (;;) {
    int n = (int)recv((sock_t)s, (char*)b, sizeof b, 0);
    if (n > 0) { in.insert(in.end(), b, b + n); continue; }
    return n < 0 && would_block();
  }
}

bool Take(std::vector<uint8_t>& in, uint8_t& type, std::vector<uint8_t>& p) {
  if (in.size() < 5) return false;
  uint32_t len = Get32(&in[1]);
  if (in.size() < 5 + len) return false;
  type = in[0];
  p.assign(in.begin() + 5, in.begin() + 5 + len);
  in.erase(in.begin(), in.begin() + 5 + len);
  return true;
}

std::string Stamp() {
  char ts[32];
  time_t now = time(nullptr);
  strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
  return ts;
}

}  // namespace

// --- the phone ---------------------------------------------------------------------------------

Server::~Server() {
  if (client_ >= 0) sclose(client_);
  if (listen_ >= 0) sclose(listen_);
}

void Server::Start(const std::string& name) {
  name_ = name;
  if (listen_ >= 0) return;
  long long s = (long long)socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return;
  int one = 1;
  setsockopt((sock_t)s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind((sock_t)s, (sockaddr*)&a, sizeof a) != 0 || listen((sock_t)s, 1) != 0) { sclose(s); return; }
  nonblock(s);
  listen_ = s;
}

void Server::Queue(uint8_t type, const std::vector<uint8_t>& p) {
  if (client_ >= 0) Frame(out_, type, p);
}

void Server::Log(const std::string& line) {
  std::string l = Stamp() + "  " + line;
  backlog_.push_back(l);
  if (backlog_.size() > 3000) backlog_.pop_front();
  Queue(T_LOG, std::vector<uint8_t>(l.begin(), l.end()));
}

void Server::SendFile(const std::string& name, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> p;
  Put32(p, (uint32_t)name.size());
  p.insert(p.end(), name.begin(), name.end());
  p.insert(p.end(), data.begin(), data.end());
  Queue(T_FILE, p);
}

std::vector<std::string> Server::Poll() {
  std::vector<std::string> cmds;
  if (listen_ < 0) return cmds;
  if (client_ < 0) {
    long long c = (long long)accept((sock_t)listen_, nullptr, nullptr);
    if (c >= 0) {
      nonblock(c);
      client_ = c;
      in_.clear();
      out_.clear();
      Frame(out_, T_HELLO, std::vector<uint8_t>(name_.begin(), name_.end()));
      for (auto& l : backlog_) Frame(out_, T_LOG, std::vector<uint8_t>(l.begin(), l.end()));   // what it missed
    }
  }
  if (client_ < 0) return cmds;
  bool ok = Fill(client_, in_);
  uint8_t t;
  std::vector<uint8_t> p;
  while (Take(in_, t, p))
    if (t == T_CMD) cmds.push_back(std::string(p.begin(), p.end()));
  if (!ok || !Flush(client_, out_)) {
    sclose(client_);
    client_ = -1;
  }
  return cmds;
}

// --- the PC ------------------------------------------------------------------------------------

Client::~Client() {
  for (auto& [ip, c] : conns_) if (c.sock >= 0) sclose(c.sock);
}

void Client::Poll(double now, const std::map<std::string, std::string>& units, const std::string& dir) {
  // Connect to units we aren't reading yet (a PC has no monitor port, so those just fail).
  for (auto& [ip, name] : units) {
    if (conns_.count(ip) || now < retry_[ip]) continue;
    retry_[ip] = now + 10;
    long long s = (long long)socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) continue;
    nonblock(s);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) { sclose(s); continue; }
    int r = connect((sock_t)s, (sockaddr*)&a, sizeof a);
    if (r != 0 && !would_block()) { sclose(s); continue; }
    Conn c;
    c.sock = s;
    c.since = now;
    c.name = name;
    c.ip = ip;
    conns_[ip] = c;
  }
  for (auto it = conns_.begin(); it != conns_.end();) {
    Conn& c = it->second;
    bool drop = false;
    if (!c.connected) {
      fd_set w;
      FD_ZERO(&w);
      FD_SET((sock_t)c.sock, &w);
      timeval tv{0, 0};
      if (select((int)c.sock + 1, nullptr, &w, nullptr, &tv) > 0) {
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt((sock_t)c.sock, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
        if (err) drop = true;
        else c.connected = true;
      } else if (now - c.since > 4) drop = true;
    }
    if (c.connected && !drop) {
      bool ok = Fill(c.sock, c.in);
      std::string folder = dir + "remote/" + c.name + "/";
      uint8_t t;
      std::vector<uint8_t> p;
      while (Take(c.in, t, p)) {
        std::error_code ec;
        if (t != T_HELLO) fs::create_directories(folder, ec);
        if (t == T_HELLO) {
          c.name = std::string(p.begin(), p.end());
          folder = dir + "remote/" + c.name + "/";
          fs::create_directories(folder, ec);
          if (FILE* f = fopen((folder + "app.log").c_str(), "ab")) {
            fprintf(f, "\n===== %s connected (%s) =====\n", c.name.c_str(), c.ip.c_str());
            fclose(f);
          }
        } else if (t == T_LOG) {
          if (FILE* f = fopen((folder + "app.log").c_str(), "ab")) { fwrite(p.data(), 1, p.size(), f); fputc('\n', f); fclose(f); }
        } else if (t == T_FILE && p.size() >= 4) {
          uint32_t n = Get32(&p[0]);
          if (4 + n <= p.size()) {
            std::string name(p.begin() + 4, p.begin() + 4 + n);
            for (auto& ch : name) if (ch == '/' || ch == '\\' || ch == ':') ch = '_';   // stays in the folder
            if (FILE* f = fopen((folder + name).c_str(), "wb")) { fwrite(p.data() + 4 + n, 1, p.size() - 4 - n, f); fclose(f); }
          }
        }
      }
      // A request dropped in the folder goes to the phone.
      std::string req = folder + "request.txt";
      if (fs::exists(req)) {
        std::string cmd;
        if (FILE* f = fopen(req.c_str(), "rb")) {
          char b[256];
          size_t n = fread(b, 1, sizeof b - 1, f);
          b[n] = 0;
          cmd = b;
          fclose(f);
        }
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();
        Frame(c.out, T_CMD, std::vector<uint8_t>(cmd.begin(), cmd.end()));
        std::error_code ec;
        fs::rename(req, folder + "request.done", ec);
      }
      if (!ok || !Flush(c.sock, c.out)) drop = true;
    }
    if (drop) {
      sclose(c.sock);
      it = conns_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace monitor
