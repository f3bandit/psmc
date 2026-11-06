// PocketStation emulator: the one piece of NanoBoyAdvance's scheduler the ARM core uses.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
#pragma once
#include <functional>
#include <nba/integer.hh>

namespace nba::core {

// The ARM core schedules a single event: clearing the LDM user-bank conflict two cycles after a
// user-mode LDM. Time here is in system clock cycles, advanced by the PocketStation bus.
struct Scheduler {
  enum class EventClass : u16 { ARM_ldm_usermode_conflict, Count };

  template<typename T>
  void Register(EventClass, T* object, void (T::*method)()) {
    callback = [object, method]() { (object->*method)(); };
  }

  void Add(u64 delay, EventClass) { pending = (int)delay; }

  void AddCycles(int cycles) {
    if (pending > 0 && (pending -= cycles) <= 0) { pending = 0; callback(); }
  }

  std::function<void()> callback;
  int pending = 0;
};

} // namespace nba::core
