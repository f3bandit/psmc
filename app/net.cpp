// MC/PS MemCard Pro: the virtual card on the network.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.

#include "net.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
static void sclose(long long s) { closesocket((SOCKET)s); }
static void nonblock(long long s) { u_long one = 1; ioctlsocket((SOCKET)s, FIONBIO, &one); }
static bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#define MSG_NOSIGNAL 0
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
static void sclose(long long s) { close((int)s); }
static void nonblock(long long s) { fcntl((int)s, F_SETFL, O_NONBLOCK); }
static bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

namespace net {

static std::string Hex(const std::vector<unsigned char>& v) {
  static const char* h = "0123456789abcdef";
  std::string o;
  for (unsigned char b : v) { o += h[b >> 4]; o += h[b & 15]; }
  return o;
}

bool Server::Open(int p) {
#ifdef _WIN32
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
#endif
  long long s = (long long)socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return false;
  int one = 1;
  setsockopt((sock_t)s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)p);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind((sock_t)s, (sockaddr*)&a, sizeof a) != 0 || listen((sock_t)s, 1) != 0) { sclose(s); return false; }
  nonblock(s);
  srv = s;
  port = p;
  return true;
}

void Server::Close() {
  if (cli >= 0) sclose(cli);
  if (srv >= 0) sclose(srv);
  cli = srv = -1;
}

static void Send(Server& s, const std::string& line) {
  std::string l = line + "\n";
  size_t off = 0;
  int spins = 0;
  while (off < l.size() && s.cli >= 0) {
    int n = (int)send((sock_t)s.cli, l.data() + off, (int)(l.size() - off), MSG_NOSIGNAL);
    if (n > 0) { off += n; continue; }
    if (n < 0 && would_block() && ++spins < 100000) continue;
    sclose(s.cli);
    s.cli = -1;
  }
}

void Server::Poll() {
  if (srv < 0) return;
  if (cli < 0) {
    sockaddr_in a{};
    socklen_t al = sizeof a;
    long long c = (long long)accept((sock_t)srv, (sockaddr*)&a, &al);
    if (c >= 0) {
      nonblock(c);
      cli = c;
      in.clear();
      char ip[64] = "";
      inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
      peer = ip;
    }
    return;
  }
  char buf[4096];
  int n = (int)recv((sock_t)cli, buf, sizeof buf, 0);
  if (n == 0 || (n < 0 && !would_block())) { sclose(cli); cli = -1; return; }
  if (n > 0) in.append(buf, n);
  size_t nl;
  while ((nl = in.find('\n')) != std::string::npos) {
    std::string line = in.substr(0, nl);
    in.erase(0, nl + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.rfind("card_raw ", 0) == 0) {
      std::string hx = line.substr(9);
      if (hx.empty() || hx.size() % 2 || hx.size() > 512) { Send(*this, "{\"ok\":false,\"error\":\"data is 1..256 bytes of hex\"}"); continue; }
      std::vector<unsigned char> tx;
      bool bad = false;
      for (size_t i = 0; i < hx.size(); i += 2) {
        char* end;
        std::string b = hx.substr(i, 2);
        long v = strtol(b.c_str(), &end, 16);
        if (*end) { bad = true; break; }
        tx.push_back((unsigned char)v);
      }
      if (bad) { Send(*this, "{\"ok\":false,\"error\":\"data is 1..256 bytes of hex\"}"); continue; }
      bool complete = false;
      auto rx = card_raw(tx, complete);
      commands++;
      const char* stop = complete ? "none" : (rx.size() <= 1 ? "no card" : "no ack");
      Send(*this, "{\"ok\":true,\"count\":" + std::to_string(rx.size()) + ",\"stop\":\"" + stop + "\",\"data\":\"" + Hex(rx) + "\"}");
    } else if (line == "status") {
      Send(*this, status());
    } else if (!line.empty()) {
      Send(*this, "{\"ok\":false,\"error\":\"unknown command\"}");
    }
  }
}

}  // namespace net
