// PocketStation emulator: the bus the ARM core talks to.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
// Same interface as NanoBoyAdvance's GBA bus (src/nba/src/bus/bus.hh), backed by the
// PocketStation memory map in pocketstation.cpp.
#pragma once
#include <nba/integer.hh>

namespace ps { struct PocketStation; }

namespace nba::core {

struct Bus {
  enum Access {
    Nonsequential = 0,
    Sequential = 1,
    Code = 2,
    Dma = 4,
    Lock = 8
  };

  explicit Bus(ps::PocketStation& machine) : m(machine) {}

  auto ReadByte(u32 address, int access) ->  u8;
  auto ReadHalf(u32 address, int access) -> u16;
  auto ReadWord(u32 address, int access) -> u32;

  void WriteByte(u32 address, u8  value, int access);
  void WriteHalf(u32 address, u16 value, int access);
  void WriteWord(u32 address, u32 value, int access);

  void Idle();

  // Set by an access to an unmapped region; the core turns it into an abort.
  bool abort = false;

  ps::PocketStation& m;
};

} // namespace nba::core
