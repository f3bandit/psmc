// MC/PS MemCard Pro: PS1 memory card images.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace card {

using u8 = std::uint8_t;
constexpr size_t SIZE = 131072;   // 16 blocks of 8 KiB, 1024 frames of 128 bytes

struct Entry {
  int first_block = 0;          // 1..15
  int blocks = 0;
  std::string product;          // e.g. BASLUS-00892, from the directory
  std::string title;            // from the save header, Shift-JIS folded to ASCII where possible
  bool pocketstation = false;   // an MCX executable the PocketStation can run
  bool mcx1 = false;            // MCX1: has a resumable snapshot
};

std::vector<u8> Formatted();
std::vector<Entry> List(const std::vector<u8>& card);
int FreeBlocks(const std::vector<u8>& card);

// Loads a card image from any of the usual formats: raw (.mcr .mcd .mc .srm .ps), DexDrive (.gme,
// 3904-byte header), a PS3 export (.psv) or a single save (.mcs / .psx / .ps1: 128-byte directory frame + data),
// which is placed on `into`. Returns an error message, empty on success.
std::string Load(const std::string& path, std::vector<u8>& into);
std::string LoadRaw(const std::string& path, std::vector<u8>& into);   // without FixAppNames
// Gives PocketStation apps saved under a hyphenated name the 'P' the kernel looks for.
int FixAppNames(std::vector<u8>& card);
// Writes the unit's serial into apps that bind themselves to one PocketStation (see card.cpp),
// where it is still unset, as the PS1 game's install does. Returns how many were bound.
int BindApps(std::vector<u8>& card, uint32_t serial);
bool Save(const std::string& path, const std::vector<u8>& card);
// One file as a single-save .mcs (directory frame + its blocks).
std::vector<u8> ExportSave(const std::vector<u8>& card, int first_block);

}  // namespace card
