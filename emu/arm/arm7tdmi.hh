// SPDX-FileCopyrightText: Copyright 2026 The NanoBoyAdvance Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <nba/save_state.hh>
#include <nba/scheduler.hh>
#include <array>

#include "bus/bus.hh"
#include "arm/state.hh"

/**
 * Some TODOs:
 * - test LDR, STR, LDM and STM behavior when writing back the address to r15
 * - test which bits of CPSR and SPSR are actually writable
 * - more accurately model when CPSR.mask_irq and the IRQ line are sampled
 * - test if Thumb CMP r15, rX loads SPSR into CPSR
 */

namespace nba::core::arm {

struct ARM7TDMI {
  using Access = Bus::Access;

  ARM7TDMI(Scheduler& scheduler, Bus& bus)
      : scheduler(scheduler)
      , bus(bus) {
    scheduler.Register(Scheduler::EventClass::ARM_ldm_usermode_conflict, this, &ARM7TDMI::ClearLDMUsermodeConflictFlag);

    Reset();
  }

  auto IRQLine() -> bool& { return irq_line; }

  // PocketStation: the card port and timer 2 raise FIQ, which the GBA never used.
  auto FIQLine() -> bool& { return fiq_line; }

  void Reset() {
    state.Reset();
    SwitchMode(state.cpsr.f.mode);

    pipe.opcode[0] = 0xF0000000;
    pipe.opcode[1] = 0xF0000000;
    pipe.access = Access::Code | Access::Nonsequential;
    irq_line = false;
    fiq_line = false;
    pipe.abort[0] = pipe.abort[1] = false;
    bus.abort = false;
    latch_irq_disable = state.cpsr.f.mask_irq;
    ldm_usermode_conflict = false;
    cpu_mode_is_invalid = false;
  }

  auto GetFetchedOpcode(int slot) -> u32 {
    return pipe.opcode[slot];
  }

  void Run() {
    // PocketStation: FIQ outranks IRQ.
    if(fiq_line && !state.cpsr.f.mask_fiq) SignalFIQ();
    else if(IRQLine()) SignalIRQ();

    auto instruction = pipe.opcode[0];
    bool fetch_aborted = pipe.abort[0];

    latch_irq_disable = state.cpsr.f.mask_irq;

    state.r15 &= ~1;

    // PocketStation: the address the executing instruction sees as PC, and its state, for the
    // abort return addresses.
    u32 exec_pc = state.r15;
    bool thumb = state.cpsr.f.thumb;

    if(thumb) {
      pipe.opcode[0] = pipe.opcode[1];
      pipe.abort[0] = pipe.abort[1];
      bus.abort = false;
      pipe.opcode[1] = ReadHalf(state.r15, pipe.access);
      pipe.abort[1] = bus.abort;
      bus.abort = false;

      if(fetch_aborted) { SignalPrefetchAbort(exec_pc, true); return; }
      (this->*s_opcode_lut_16[instruction >> 6])(instruction);
    } else {
      pipe.opcode[0] = pipe.opcode[1];
      pipe.abort[0] = pipe.abort[1];
      bus.abort = false;
      pipe.opcode[1] = ReadWord(state.r15, pipe.access);
      pipe.abort[1] = bus.abort;
      bus.abort = false;

      if(fetch_aborted) { SignalPrefetchAbort(exec_pc, false); return; }
      if(CheckCondition(static_cast<Condition>(instruction >> 28))) {
        int hash = ((instruction >> 16) & 0xFF0) |
                   ((instruction >>  4) & 0x00F);
        (this->*s_opcode_lut_32[hash])(instruction);
      } else {
        pipe.access = Access::Code | Access::Sequential;
        state.r15 += 4;
      }
    }

    // PocketStation: a load or store into an unmapped region (an unmapped virtual flash block,
    // or nothing at all) aborts. Return address: the aborted instruction + 8.
    if(bus.abort) {
      bus.abort = false;
      SignalDataAbort(thumb ? exec_pc + 4 : exec_pc);
    }
  }

  // PocketStation: exception entry shared by FIQ and the aborts.
  void EnterException(Mode mode, Bank bank, u32 vector, u32 lr, bool mask_fiq) {
    state.spsr[bank].v = state.cpsr.v;
    SwitchMode(mode);
    state.cpsr.f.mask_irq = 1;
    if(mask_fiq) state.cpsr.f.mask_fiq = 1;
    state.cpsr.f.thumb = 0;
    SetReg(14, lr);
    state.r15 = vector;
    ReloadPipeline32();
  }

  void SignalFIQ() {
    // Same prefetch and return address as SignalIRQ.
    if(state.cpsr.f.thumb) {
      ReadHalf(state.r15 & ~1, pipe.access);
      EnterException(MODE_FIQ, BANK_FIQ, 0x1C, state.r15, true);
    } else {
      ReadWord(state.r15 & ~3, pipe.access);
      EnterException(MODE_FIQ, BANK_FIQ, 0x1C, state.r15 - 4, true);
    }
  }

  // exec_pc: the PC the aborted instruction saw (instruction + 8 in ARM, + 4 in Thumb).
  // Return address: the aborted instruction + 4.
  void SignalPrefetchAbort(u32 exec_pc, bool thumb) {
    EnterException(MODE_ABT, BANK_ABT, 0x0C, thumb ? exec_pc : exec_pc - 4, false);
  }

  void SignalDataAbort(u32 lr) {
    EnterException(MODE_ABT, BANK_ABT, 0x10, lr, false);
  }

  void SwitchMode(Mode new_mode) {
    auto old_bank = GetRegisterBankByMode(state.cpsr.f.mode);
    auto new_bank = GetRegisterBankByMode(new_mode);

    state.cpsr.f.mode = new_mode;

    if(new_bank != BANK_NONE) {
      p_spsr = &state.spsr[new_bank];
    } else {
      /* In system/user mode reading from SPSR returns the current CPSR value.
       * However, writes to SPSR appear to do nothing.
       * We take care of this fact in the MSR implementation.
       */
      p_spsr = &state.cpsr;
    }

    if(old_bank == new_bank) {
      return;
    }

    if(old_bank == BANK_FIQ) {
      for(int i = 0; i < 5; i++) {
        state.bank[BANK_FIQ][i] = state.reg[8 + i];
      }

      for(int i = 0; i < 5; i++) {
        state.reg[8 + i] = state.bank[BANK_NONE][i];
      }
    } else if(new_bank == BANK_FIQ) {
      for(int i = 0; i < 5; i++) {
        state.bank[BANK_NONE][i] = state.reg[8 + i];
      }

      for(int i = 0; i < 5; i++) {
        state.reg[8 + i] = state.bank[BANK_FIQ][i];
      }
    }

    state.bank[old_bank][5] = state.r13;
    state.bank[old_bank][6] = state.r14;

    if(new_bank != BANK_INVALID) [[likely]] {
      state.r13 = state.bank[new_bank][5];
      state.r14 = state.bank[new_bank][6];
      cpu_mode_is_invalid = false;
    } else {
      for(int i = 0; i < 7; i++) {
        state.reg[8 + i] = 0u;
      }
      state.spsr[BANK_INVALID] = 0u;
      cpu_mode_is_invalid = true;
    }
  }

  void LoadState(SaveState const& save_state);
  void CopyState(SaveState& save_state);

  RegisterFile state;

  typedef void (ARM7TDMI::*Handler16)(u16);
  typedef void (ARM7TDMI::*Handler32)(u32);

private:
  friend struct TableGen;

  auto GetReg(int id) -> u32 {
    u32 result = state.reg[id];

    if(ldm_usermode_conflict && id >= 8 && id != 15) [[unlikely]] {
      // This array holds the current user/sys bank value only if the CPU wasn't in user or system mode all along during the user mode LDM instruction.
      // We take care in the LDM implementation that this branch is only taken if that was the case.
      result |= state.bank[BANK_NONE][id - 8];
    }

    return result;
  }

  void SetReg(int id, u32 value) {
    bool is_banked = id >= 8 && id != 15;

    if(ldm_usermode_conflict && is_banked) [[unlikely]] {
      // This array holds the current user/sys bank value only if the CPU wasn't in user or system mode all along during the user mode LDM instruction.
      // We take care in the LDM implementation that this branch is only taken if that was the case.
      state.bank[BANK_NONE][id - 8] = value;
    }

    if(!cpu_mode_is_invalid || !is_banked) [[likely]] {
      state.reg[id] = value;
    }
  }

  auto GetSPSR() -> StatusRegister {
    // CPSR/SPSR bit4 is forced to one on the ARM7TDMI:
    u32 spsr = p_spsr->v | 0x00000010u;

    if(ldm_usermode_conflict) [[unlikely]] {
      /* TODO: current theory is that the value gets OR'd with CPSR,
       * because in user and system mode SPSR reads return the CPSR value.
       * But this needs to be confirmed.
       */
      spsr |= state.cpsr.v;
    }

    return StatusRegister{spsr};
  }

  void SignalIRQ() {
    if(latch_irq_disable) {
      return;
    }

    // Prefetch the next instruction
    // The result will be discarded because we flush the pipeline.
    // But this is important for timing nonetheless.
    if(state.cpsr.f.thumb) {
      ReadHalf(state.r15 & ~1, pipe.access);
    } else {
      ReadWord(state.r15 & ~3, pipe.access);
    }

    // Save current program status register.
    state.spsr[BANK_IRQ].v = state.cpsr.v;

    // Enter IRQ mode and disable IRQs.
    SwitchMode(MODE_IRQ);
    state.cpsr.f.mask_irq = 1;

    // Save current program counter and disable Thumb.
    if(state.cpsr.f.thumb) {
      state.cpsr.f.thumb = 0;
      SetReg(14, state.r15);
    } else {
      SetReg(14, state.r15 - 4);
    }

    // Jump to IRQ exception vector.
    state.r15 = 0x18;
    ReloadPipeline32();
  }

  bool CheckCondition(Condition condition) {
    if(condition == COND_AL)
      return true;
    return s_condition_lut[(static_cast<int>(condition) << 4) | (state.cpsr.v >> 28)];
  }

  void ReloadPipeline16() {
    bus.abort = false;
    pipe.opcode[0] = bus.ReadHalf(state.r15 + 0, Access::Code | Access::Nonsequential);
    pipe.abort[0] = bus.abort; bus.abort = false;
    pipe.opcode[1] = bus.ReadHalf(state.r15 + 2, Access::Code | Access::Sequential);
    pipe.abort[1] = bus.abort; bus.abort = false;
    pipe.access = Access::Code | Access::Sequential;
    state.r15 += 4;

    latch_irq_disable = state.cpsr.f.mask_irq;
  }

  void ReloadPipeline32() {
    bus.abort = false;
    pipe.opcode[0] = bus.ReadWord(state.r15 + 0, Access::Code | Access::Nonsequential);
    pipe.abort[0] = bus.abort; bus.abort = false;
    pipe.opcode[1] = bus.ReadWord(state.r15 + 4, Access::Code | Access::Sequential);
    pipe.abort[1] = bus.abort; bus.abort = false;
    pipe.access = Access::Code | Access::Sequential;
    state.r15 += 8;

    latch_irq_disable = state.cpsr.f.mask_irq;
  }

  auto GetRegisterBankByMode(Mode mode) -> Bank {
    switch(mode) {
      case MODE_USR:
      case MODE_SYS:
        return BANK_NONE;
      case MODE_FIQ:
        return BANK_FIQ;
      case MODE_IRQ:
        return BANK_IRQ;
      case MODE_SVC:
        return BANK_SVC;
      case MODE_ABT:
        return BANK_ABT;
      case MODE_UND:
        return BANK_UND;
    }

    return BANK_INVALID;
  }

public:
  // PocketStation: savestate fields, visited by `io(field)` for both saving and loading.
  template<typename IO> void StateFields(IO&& io) {
    io(state); io(pipe); io(irq_line); io(fiq_line); io(latch_irq_disable);
    io(ldm_usermode_conflict); io(cpu_mode_is_invalid);
  }

  // PocketStation: after a savestate restored `state`, point the SPSR alias at the right bank.
  void RestoreSpsrPointer() {
    auto bank = GetRegisterBankByMode(state.cpsr.f.mode);
    p_spsr = bank != BANK_NONE ? &state.spsr[bank] : &state.cpsr;
  }
private:

  void ClearLDMUsermodeConflictFlag() {
    ldm_usermode_conflict = false;
  }

  #include "handlers/arithmetic.inl"
  #include "handlers/handler16.inl"
  #include "handlers/handler32.inl"
  #include "handlers/memory.inl"

  Scheduler& scheduler;
  Bus& bus;
  StatusRegister* p_spsr;
  bool ldm_usermode_conflict;
  bool cpu_mode_is_invalid;

  struct Pipeline {
    int access;
    u32 opcode[2];
    bool abort[2];   // PocketStation: the fetch of this slot aborted
  } pipe;

  bool irq_line;
  bool fiq_line;   // PocketStation

 public:
  // PocketStation: for tracing which kernel calls an app makes (tools/kernel_trace). A SWI as it's
  // taken (its number and where it came from), and every return from an exception (where to and
  // r0). Null in the apps.
  void (*swi_hook)(void* ctx, u32 number, u32 from) = nullptr;
  void (*exret_hook)(void* ctx, u32 to, u32 r0) = nullptr;
  void* hook_ctx = nullptr;

  bool latch_irq_disable;

  static std::array<bool, 256> s_condition_lut;
  static std::array<Handler16, 1024> s_opcode_lut_16;
  static std::array<Handler32, 4096> s_opcode_lut_32;
};

} // namespace nba::core::arm
