// MC/PS MemCard Pro: PS1 memory card images.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
// Directory layout from the PS1 memory card format (frame 0 header, frames 1-15 one per block,
// 0x51 first / 0x52 middle / 0x53 last / 0xA0 free, next-block link at bytes 8-9, XOR checksum in
// byte 127). PocketStation executables are recognised by "MCX" at offset 0x52 of the file, the
// same test as the loader in the PocketStation core's open BIOS.

#include "card.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace card {

static u8 XorSum(const u8* p) {
  u8 c = 0;
  for (int i = 0; i < 127; i++) c ^= p[i];
  return c;
}

std::vector<u8> Formatted() {
  std::vector<u8> c(SIZE, 0);
  c[0] = 'M'; c[1] = 'C'; c[127] = 'M' ^ 'C';
  for (int f = 1; f < 16; f++) {
    u8* p = &c[f * 128];
    p[0] = 0xA0; p[8] = 0xFF; p[9] = 0xFF;
    p[127] = XorSum(p);
  }
  for (int f = 16; f < 36; f++) {
    u8* p = &c[f * 128];
    p[0] = p[1] = p[2] = p[3] = 0xFF; p[8] = 0xFF; p[9] = 0xFF;
    p[127] = XorSum(p);
  }
  // Frame 63 is a copy of the header on a Sony-formatted card.
  c[63 * 128] = 'M'; c[63 * 128 + 1] = 'C'; c[63 * 128 + 127] = 'M' ^ 'C';
  return c;
}

// Shift-JIS full-width ASCII (0x8140 space, 0x824F..0x829A digits and letters) to ASCII.
static std::string Title(const u8* p, int n) {
  std::string s;
  for (int i = 0; i < n && p[i]; i++) {
    u8 a = p[i];
    if (a < 0x80) { s += (char)a; continue; }
    if (i + 1 >= n) break;
    u8 b = p[++i];
    if (a == 0x81 && b == 0x40) s += ' ';
    else if (a == 0x82 && b >= 0x4F && b <= 0x58) s += (char)('0' + b - 0x4F);
    else if (a == 0x82 && b >= 0x60 && b <= 0x79) s += (char)('A' + b - 0x60);
    else if (a == 0x82 && b >= 0x81 && b <= 0x9A) s += (char)('a' + b - 0x81);
    else if (a == 0x81 && b == 0x5B) s += '-';
    else if (a == 0x81 && b == 0x46) s += ':';
    else if (a == 0x81 && b == 0x44) s += '.';
    else if (a == 0x81 && b == 0x5E) s += '/';
    else if (a == 0x81 && b == 0x69) s += '(';
    else if (a == 0x81 && b == 0x6A) s += ')';
    else s += '?';
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '?')) s.pop_back();
  return s;
}

std::vector<Entry> List(const std::vector<u8>& c) {
  std::vector<Entry> out;
  if (c.size() < SIZE) return out;
  for (int b = 1; b < 16; b++) {
    const u8* d = &c[b * 128];
    if (d[0] != 0x51) continue;
    Entry e;
    e.first_block = b;
    e.product.assign((const char*)d + 0x0A, strnlen((const char*)d + 0x0A, 20));
    // Follow the chain to count blocks.
    int cur = b, n = 0;
    while (cur >= 1 && cur < 16 && n < 15) {
      n++;
      int next = c[cur * 128 + 8] | c[cur * 128 + 9] << 8;
      if (next == 0xFFFF) break;
      cur = next + 1;
    }
    e.blocks = n;
    const u8* f = &c[b * 8192];
    if (f[0] == 'S' && f[1] == 'C') e.title = Title(f + 4, 64);
    e.pocketstation = f[0x52] == 'M' && f[0x53] == 'C' && f[0x54] == 'X';
    e.mcx1 = e.pocketstation && f[0x55] == '1';
    if (e.title.empty()) e.title = e.product;
    out.push_back(e);
  }
  return out;
}

int FreeBlocks(const std::vector<u8>& c) {
  int n = 0;
  for (int b = 1; b < 16; b++) if (c[b * 128] == 0xA0) n++;
  return n;
}

static std::vector<u8> ReadAll(const std::string& path) {
  std::vector<u8> v;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return v;
  u8 buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
  fclose(f);
  return v;
}

// Places a single save (directory frame + data) into free blocks.
static std::string Place(std::vector<u8>& c, const u8* frame, const u8* data, size_t len) {
  int need = (int)((len + 8191) / 8192);
  if (need < 1 || need > 15) return "that save has no data";
  std::vector<int> free;
  for (int b = 1; b < 16 && (int)free.size() < need; b++) if (c[b * 128] == 0xA0) free.push_back(b);
  if ((int)free.size() < need) return "not enough free blocks on the card (" + std::to_string(need) + " needed)";
  for (int i = 0; i < need; i++) {
    u8* p = &c[free[i] * 128];
    memset(p, 0, 128);
    if (i == 0) memcpy(p, frame, 128);
    else memcpy(p + 0x0A, frame + 0x0A, 20);
    p[0] = i == 0 ? 0x51 : (i == need - 1 ? 0x53 : 0x52);
    if (i == 0) { uint32_t size = (uint32_t)need * 8192; memcpy(p + 4, &size, 4); }
    else memset(p + 4, 0, 4);
    int next = i + 1 < need ? free[i + 1] - 1 : 0xFFFF;
    p[8] = next & 0xFF; p[9] = (next >> 8) & 0xFF;
    p[127] = XorSum(p);
    size_t off = (size_t)i * 8192;
    memset(&c[free[i] * 8192], 0, 8192);
    if (off < len) memcpy(&c[free[i] * 8192], data + off, std::min<size_t>(8192, len - off));
  }
  return "";
}

// A PocketStation executable's directory name has 'P' where a save has its hyphen
// (BASLUSP00892..., not BASLUS-00892...): the kernel only lists files named that way as apps.
// Saves copied by tools that "normalised" the name get it back here.
int FixAppNames(std::vector<u8>& c) {
  int fixed = 0;
  for (int b = 1; b < 16; b++) {
    u8* d = &c[b * 128];
    if (d[0] != 0x51 || d[0x10] != '-') continue;
    const u8* f = &c[b * 8192];
    if (!(f[0] == 'S' && f[1] == 'C' && f[0x52] == 'M' && f[0x53] == 'C' && f[0x54] == 'X')) continue;
    int cur = b, n = 0;
    while (cur >= 1 && cur < 16 && n++ < 15) {
      u8* p = &c[cur * 128];
      if (p[0x10] == '-') { p[0x10] = 'P'; p[127] = XorSum(p); }
      int next = p[8] | p[9] << 8;
      if (next == 0xFFFF) break;
      cur = next + 1;
    }
    fixed++;
  }
  return fixed;
}

// Apps that are tied to one PocketStation. When the PS1 game installs one, it reads the unit's
// serial number (BU command 5Ah) and writes it into the app's file; on launch the app compares it
// with the unit's own (kernel SWI 0Ah) and, on a mismatch, marks itself a copy (コピー) and wipes
// its data. Bringing one onto this unit is what the game's install does: this unit's serial is
// written in. A copy off the disc carries 0; one dumped from another unit carries that unit's.
struct BoundApp {
  const char* product;            // directory name prefix
  std::vector<size_t> serial_at;  // offsets within the file that hold the installed serial
};
static const BoundApp BOUND_APPS[] = {
  // Metal Gear Solid Integral, MGS∫[FOXDIE]: 0x7F0C is checked at launch, 0x7F80 is the app's
  // own copy of the serial it last ran on.
  {"BISLPMP86247", {0x7F0C, 0x7F80}},
};

static u8* FileByte(std::vector<u8>& c, int first, size_t at) {
  int cur = first;
  for (size_t i = 0; i < at / 8192; i++) {
    if (cur < 1 || cur >= 16) return nullptr;
    int next = c[cur * 128 + 8] | c[cur * 128 + 9] << 8;
    if (next == 0xFFFF) return nullptr;
    cur = next + 1;
  }
  if (cur < 1 || cur >= 16) return nullptr;
  return &c[cur * 8192 + at % 8192];
}

int BindApps(std::vector<u8>& c, uint32_t serial) {
  int bound = 0;
  for (int b = 1; b < 16; b++) {
    const u8* d = &c[b * 128];
    if (d[0] != 0x51) continue;
    for (auto& a : BOUND_APPS) {
      if (memcmp(d + 0x0A, a.product, strlen(a.product)) != 0) continue;
      u8* check = FileByte(c, b, a.serial_at[0]);
      if (!check) continue;
      uint32_t have;
      memcpy(&have, check, 4);
      if (have == serial) continue;
      if (have == 0xFFFFFFFFu) continue;   // already wiped as a copy: nothing left to bind
      for (size_t at : a.serial_at) {
        u8* p = FileByte(c, b, at);
        if (!p) continue;
        uint32_t cur;
        memcpy(&cur, p, 4);
        if (at == a.serial_at[0] || cur == have) memcpy(p, &serial, 4);
      }
      bound++;
    }
  }
  return bound;
}

std::string Load(const std::string& path, std::vector<u8>& into) {
  std::string err = LoadRaw(path, into);
  if (err.empty()) FixAppNames(into);
  return err;
}

std::string LoadRaw(const std::string& path, std::vector<u8>& into) {
  auto d = ReadAll(path);
  if (d.empty()) return "can't read " + path;
  if (into.size() != SIZE) into = Formatted();
  // DexDrive: 3904-byte header, then the card.
  // Some copies in circulation are cut short; keep what's there if every block the directory uses
  // is present, and leave the rest of the card free.
  if (d.size() > 3904 + 8192 && d.size() <= SIZE + 3904 && memcmp(d.data(), "123-456-STD", 11) == 0) {
    size_t have = d.size() - 3904;
    std::vector<u8> c = Formatted();
    std::copy(d.begin() + 3904, d.end(), c.begin());
    for (int b = 1; b < 16; b++) {
      bool used = (c[b * 128] & 0xF0) == 0x50;
      if (used && (size_t)(b + 1) * 8192 > have) return "the .gme file is cut short and missing saves";
      if (!used && (size_t)b * 8192 >= have) {  // directory entry lies beyond nothing: mark free
        std::fill(c.begin() + b * 128, c.begin() + b * 128 + 128, 0);
        c[b * 128] = 0xA0;
        c[b * 128 + 8] = c[b * 128 + 9] = 0xFF;
        u8 x = 0;
        for (int i = 0; i < 127; i++) x ^= c[b * 128 + i];
        c[b * 128 + 127] = x;
      }
    }
    into = c;
    return "";
  }
  if (d.size() == SIZE) {
    if (d[0] != 'M' || d[1] != 'C') return "not a memory card image (no MC header)";
    into = d;
    return "";
  }
  // PS3 export of a PS1 save (.psv): "\0VSP" header, file name at 0x64, data size at 0x40 and
  // data offset at 0x44. The directory frame is rebuilt from the name.
  if (d.size() > 0x84 && memcmp(d.data(), "\0VSP", 4) == 0) {
    uint32_t size, off;
    memcpy(&size, &d[0x40], 4);
    memcpy(&off, &d[0x44], 4);
    if (size == 0 || size % 8192 || (size_t)off + size > d.size()) return "unsupported .psv (PS2 save?)";
    u8 frame[128] = {};
    frame[0] = 0x51;
    memcpy(frame + 4, &size, 4);
    frame[8] = frame[9] = 0xFF;
    memcpy(frame + 0x0A, &d[0x64], 20);
    return Place(into, frame, d.data() + off, size);
  }
  // A single save: 128-byte directory frame (first block, 0x51) then its blocks.
  if (d.size() > 128 && (d.size() - 128) % 8192 == 0 && d[0] == 0x51)
    return Place(into, d.data(), d.data() + 128, d.size() - 128);
  return "unknown file format (" + std::to_string(d.size()) + " bytes)";
}

bool Save(const std::string& path, const std::vector<u8>& c) {
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(c.data(), 1, c.size(), f) == c.size();
  ok = fclose(f) == 0 && ok;
  if (!ok) { remove(tmp.c_str()); return false; }
  remove(path.c_str());
  return rename(tmp.c_str(), path.c_str()) == 0;
}

std::vector<u8> ExportSave(const std::vector<u8>& c, int first) {
  std::vector<u8> out(&c[first * 128], &c[first * 128] + 128);
  int cur = first, n = 0;
  while (cur >= 1 && cur < 16 && n < 15) {
    out.insert(out.end(), &c[cur * 8192], &c[cur * 8192] + 8192);
    n++;
    int next = c[cur * 128 + 8] | c[cur * 128 + 9] << 8;
    if (next == 0xFFFF) break;
    cur = next + 1;
  }
  return out;
}

}  // namespace card
