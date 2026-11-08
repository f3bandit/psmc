// MC/PS kernel: the kernel calls, interrupts, flash, clock and starting programs.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// Written from the public PocketStation documentation (nocash's PSX specifications) and from
// watching how games behave; see docs/kernel-spec.md.
#include "hw.h"

extern void launch(u32 entry, u32 param) __attribute__((noreturn));
extern void swi_custom2(void);
extern void swi_reset(void);
extern void gui_main(u32 param);
extern const u8 font8x8[128][8];

// ComFlags bits
#define CF_DOCK_IRQ (1u << 8)
#define CF_COM_ON (1u << 9)
#define CF_PROTECT (1u << 10)

// --- helpers -------------------------------------------------------------------------------

static int docked(void) { return (INT_INPUT >> I_DOCK) & 1; }

// --- directory -----------------------------------------------------------------------------

// A file's first block (1..15) is its directory index. Returns the number of blocks it chains
// through and fills blocks[] (physical block numbers, in order); 0 if it isn't a file's start.
static int file_blocks(u32 index, u8 blocks[15]) {
  if (index < 1 || index > 15) return 0;
  if (rd8(FLASH_BASE + index * 0x80) != 0x51) return 0;
  int n = 0;
  u32 b = index;
  while (n < 15) {
    blocks[n++] = (u8)b;
    u32 next = *(volatile u16*)(FLASH_BASE + b * 0x80 + 8);
    if (next == 0xFFFF || next > 14) break;
    b = next + 1;
  }
  return n;
}

// An executable: "SC" with MCX0 or MCX1 in its title sector.
static int is_executable(u32 index) {
  u8 bl[15];
  if (!file_blocks(index, bl)) return 0;
  u32 t = FLASH_BASE + index * 0x2000;
  if (*(volatile u16*)t != ('S' | 'C' << 8)) return 0;
  u32 id = *(volatile u16*)(t + 0x52) | (u32)*(volatile u16*)(t + 0x54) << 16;
  return id == ('M' | 'C' << 8 | 'X' << 16 | (u32)'0' << 24) || id == ('M' | 'C' << 8 | 'X' << 16 | (u32)'1' << 24);
}

// Maps a file at 02000000h (its blocks in order).
static int map_file(u32 index) {
  u8 bl[15];
  int n = file_blocks(index, bl);
  if (!n) return 0;
  F_CTRL = 1;   // mapping off while it changes (keeps RAM at 0)
  u32 active = 0;
  for (int i = 0; i < 16; i++) F_BANK_VAL(i) = 0;
  for (int i = 0; i < n; i++) {
    F_BANK_VAL(bl[i]) = i;
    active |= 1u << bl[i];
  }
  F_BANK_FLG = active;
  F_CTRL = 3;
  return n;
}

// --- flash ---------------------------------------------------------------------------------

static u32 flash_write_physical(u32 sector, const void* src) {
  if (sector > 0x3FF) return 1;
  if ((K_COMFLAGS & CF_PROTECT) && sector >= 16 && sector <= 55) return 1;
  u32 s = (u32)src;   // may itself be in flash: read a halfword at a time
  u32 old_irq = INT_MASK;
  F_WAIT2 = (F_WAIT2 & 0x10) | 0x21;              // program enable, page load
  *(volatile u16*)0x080055AA = 0xFFAA;
  *(volatile u16*)0x08002A54 = 0xFF55;
  *(volatile u16*)0x080055AA = 0xFFA0;
  volatile u16* dst = (volatile u16*)(0x08000000 + sector * 0x80);
  for (int i = 0; i < 64; i++) dst[i] = rd8(s + 2 * i) | rd8(s + 2 * i + 1) << 8;
  F_WAIT2 = F_WAIT2 & 0x10;                        // ends the page: it programs now
  for (u32 n = 0; n < 2000 && (F_WAIT2 & 4); n++) {}         // until it starts (busy)
  for (u32 n = 0; n < 2000000 && !(F_WAIT2 & 4); n++) {}     // and until it's done
  (void)old_irq;
  for (int i = 0; i < 64; i++)
    if (*(volatile u16*)(FLASH_BASE + sector * 0x80 + 2 * i) != (rd8(s + 2 * i) | rd8(s + 2 * i + 1) << 8)) return 1;
  return 0;
}

static u32 swi_flash_write_virtual(u32 sector, const void* src) {
  u32 vblock = sector >> 6;
  if (!(F_CTRL & 2)) return 1;
  for (u32 p = 0; p < 16; p++)
    if (((F_BANK_FLG >> p) & 1) && F_BANK_VAL(p) == vblock) return flash_write_physical(p * 64 + (sector & 63), src);
  return 1;   // past the file's end
}

static u32 swi_flash_write_physical(u32 sector, const void* src) { return flash_write_physical(sector, src); }

static u32 swi_flash_read_serial(void) { return F_SN_LO | (u32)F_SN_HI << 16; }

static u32 swi_flash_write_serial(u32 sn) { (void)sn; return 0; }   // the old BIOS only

static u32 swi_flash_read_whatever(u32 sector) { return rd8(FLASH_BASE + (sector & 0x3FF) * 0x80 + 0x7E); }

// --- clock ---------------------------------------------------------------------------------

static u32 swi_get_bcd_time(void) { return RTC_TIME; }

static u32 swi_get_bcd_date(void) { return (RTC_DATE & 0xFFFFFF) | (u32)KV->century << 24; }

// Programming mode: each counter stepped (two writes a step) until it reads what's wanted.
static void rtc_step_to(u32 sel, u32 shift, u32 want, int is_time) {
  RTC_MODE = 1 | sel << 1;
  for (int n = 0; n < 200; n++) {
    u32 v = ((is_time ? RTC_TIME : RTC_DATE) >> shift) & 0xFF;
    if (v == want) break;
    RTC_ADJUST = 1;
    RTC_ADJUST = 1;
  }
}

static u32 swi_set_bcd_date_time(u32 date, u32 time) {
  rtc_step_to(0, 0, time & 0xFF, 1);           // seconds
  rtc_step_to(1, 8, (time >> 8) & 0xFF, 1);    // minutes
  rtc_step_to(2, 16, (time >> 16) & 0xFF, 1);  // hours
  rtc_step_to(3, 24, (time >> 24) & 0xFF, 1);  // day of the week
  rtc_step_to(6, 16, (date >> 16) & 0xFF, 0);  // year (before the day: February)
  rtc_step_to(5, 8, (date >> 8) & 0xFF, 0);    // month
  rtc_step_to(4, 0, date & 0xFF, 0);           // day
  RTC_MODE = 0;
  KV->century = (u8)(date >> 24);
  K_ALARM[0] |= 1u << 23;
  return RTC_DATE >> 16;
}

void kernel_reset_clock(void) {
  swi_set_bcd_date_time(0x19990101, 0x05000000);   // 1999-01-01 (a Friday) 00:00:00
  K_ALARM[0] &= ~((1u << 16) | (1u << 23));        // alarm off, clock not set
}

// --- CPU speed -----------------------------------------------------------------------------

static u32 swi_set_cpu_speed(u32 speed) {
  u32 old = CLK_MODE & 0xF;
  speed &= 0xF;
  CLK_MODE = speed;
  while ((CLK_MODE & 0x10) == 0) {}   // locked
  return old;
}

// --- callbacks and communication -----------------------------------------------------------

static u32 swi_set_callbacks(u32 index, u32 proc) {
  if (index > 3) return 0;
  u32 old = K_CALLBACK[index];
  K_CALLBACK[index] = proc;
  return old;
}

static void com_enable(int on) {
  KV->com_on = (u8)on;
  if (on) {
    INT_MASK = 1u << I_COM;
    if (docked()) {
      K_COMFLAGS |= CF_COM_ON;
      swi_set_cpu_speed(7);
    }
  } else {
    INT_MASK_CLR = 1u << I_COM;
    K_COMFLAGS &= ~CF_COM_ON;
  }
}

static u32 swi_set_com_on_off(u32 flag) {
  com_enable(flag & 1);
  return 0;
}

static u32 swi_sense_auto_com(void) {
  int d = docked();
  if (d != KV->docked_was) {
    KV->docked_was = (u8)d;
    K_COMFLAGS &= ~CF_DOCK_IRQ;
    K_COMFLAGS &= ~CF_PROTECT;
    if (d) {
      K_FUNC3 = 0;
      K_BU50 = 0;
      com_enable(1);
    } else {
      com_enable(0);
    }
  }
  return K_COMFLAGS & 0x70000;
}

static u32 swi_get_ptr_to_com_flags(void) { return 0xC0; }

static u32 swi_change_auto_docking(u32 flags) {
  K_COMFLAGS = (K_COMFLAGS & ~0x70000u) | (flags & 0x70000);
  return flags & 0x70000;
}

static u32 swi_clear_com_flags_bit10(void) {
  K_COMFLAGS &= ~CF_PROTECT;
  return K_COMFLAGS;
}

static u32 swi_get_ptr_to_alarm(void) { return 0xD8; }
static u32 swi_get_ptr_to_ptr_to_swi_table(void) { return 0xE0; }
static u32 swi_get_ptr_to_func3(void) { return 0xC8; }

// --- starting programs ---------------------------------------------------------------------

static u32 swi_prepare_execute(u32 flag, u32 index, u32 param) {
  if (!(flag & 1)) return KV->dir_index;
  index &= 0xFFFF;
  if (index != 0 && !is_executable(index)) return KV->next_index;
  KV->next_index = (u16)index;
  KV->param = param;
  return index;
}

static u32 swi_get_dir_index(void) { return KV->dir_index == 0 ? KV->alt_index : KV->dir_index; }

static u32 swi_make_alternate_dir_index(u32 flag, u32 index) {
  if (!(flag & 1)) return KV->alt_index;
  if (index == 0 || index == KV->dir_index) KV->alt_index = (u16)index;
  return KV->alt_index;
}

static u32 swi_test_snapshot(u32 index) {
  if (!is_executable(index)) return 0;
  return rd8(FLASH_BASE + index * 0x2000 + 0x55) == '1';   // the snapshot itself: still to do
}

// The hardware as every program finds it.
static void fresh_hardware(void) {
  INT_MASK_CLR = 0x3FFF;
  INT_ACK = 0x3FFF;
  for (int i = 0; i < 3; i++) TMR_CTRL(i) = 0;
  // The reload values a program finds (the real menu leaves its own there; some programs start
  // a timer without setting one).
  TMR_LOAD(0) = 0x20;
  TMR_LOAD(1) = 0x34F;
  TMR_LOAD(2) = 0xFFFF;
  IOP_CLR = 0x7F;
  IOP_SET = 0x62;             // speaker in stand-by
  IRDA_MODE = 2;
  DAC_CTRL = 0;
  LCD_MODE = 0x58;            // on, frame rate 1
  swi_set_cpu_speed(7);
  for (int i = 0; i < 4; i++) K_CALLBACK[i] = 0;
  com_enable(0);
  INT_MASK = (1u << I_RTC) | (1u << I_DOCK);
}

static u32 swi_do_execute(u32 snapshot_flag) {
  (void)snapshot_flag;
  if (K_COMFLAGS & CF_COM_ON) return 0;   // communication on: refused
  u32 index = KV->next_index, param = KV->param;
  __asm__ volatile("msr cpsr_c, #0xD3");   // IRQ and FIQ off from here
  fresh_hardware();
  if (index != 0 && map_file(index)) {
    KV->dir_index = (u16)index;
    u32 entry = *(volatile u32*)0x0200005C;
    launch(entry, param);
  }
  KV->dir_index = 0;
  F_CTRL = 1;
  launch((u32)gui_main, param);
}

void kernel_crash(void) {
  KV->next_index = 0;
  KV->param = 0;
  K_COMFLAGS &= ~CF_COM_ON;
  swi_do_execute(0);
}

// --- the SWI table (its address is kept at 0E0h; some programs call through it) -------------

typedef u32 (*swi_fn)();
const swi_fn swi_table[0x19] = {
  (swi_fn)swi_reset,                       // 00
  (swi_fn)swi_set_callbacks,               // 01
  (swi_fn)swi_custom2,                     // 02
  (swi_fn)swi_flash_write_virtual,         // 03
  (swi_fn)swi_set_cpu_speed,               // 04
  (swi_fn)swi_sense_auto_com,              // 05
  (swi_fn)swi_get_ptr_to_com_flags,        // 06
  (swi_fn)swi_change_auto_docking,         // 07
  (swi_fn)swi_prepare_execute,             // 08
  (swi_fn)swi_do_execute,                  // 09
  (swi_fn)swi_flash_read_serial,           // 0A
  (swi_fn)swi_clear_com_flags_bit10,       // 0B
  (swi_fn)swi_set_bcd_date_time,           // 0C
  (swi_fn)swi_get_bcd_date,                // 0D
  (swi_fn)swi_get_bcd_time,                // 0E
  (swi_fn)swi_flash_write_serial,          // 0F
  (swi_fn)swi_flash_write_physical,        // 10
  (swi_fn)swi_set_com_on_off,              // 11
  (swi_fn)swi_test_snapshot,               // 12
  (swi_fn)swi_get_ptr_to_alarm,            // 13
  (swi_fn)swi_get_ptr_to_ptr_to_swi_table, // 14
  (swi_fn)swi_make_alternate_dir_index,    // 15
  (swi_fn)swi_get_dir_index,               // 16
  (swi_fn)swi_get_ptr_to_func3,            // 17
  (swi_fn)swi_flash_read_whatever,         // 18
};

// --- interrupts ----------------------------------------------------------------------------

void kernel_irq(void) {
  u32 pending = INT_LATCH & INT_MASK;
  if (pending & (1u << I_DOCK)) K_COMFLAGS |= CF_DOCK_IRQ;
  // Ours to clear: the clock tick and docking. The program's callback clears its own.
  INT_ACK = pending & ((1u << I_RTC) | (1u << I_DOCK));
  if (!K_CALLBACK[1]) INT_ACK = pending;   // no program handler: nothing else will
}

void kernel_fiq(void) {
  // The card port, when communication is on: still to do. Otherwise nothing is listening.
  u32 pending = INT_LATCH & INT_MASK & ((1u << I_COM) | (1u << I_TMR2));
  if (!K_CALLBACK[2]) {
    INT_ACK = pending;
    if (pending & (1u << I_COM)) COM_MODE = 0;
  }
}

// --- power-on ------------------------------------------------------------------------------

void kernel_boot(void) {
  for (u32 a = 0x40; a < 0x200; a += 4) *(volatile u32*)a = 0;   // (this runs on the stack at 800h)
  K_COMFLAGS = 0x70000;      // sound, infrared and LCD rotation handled on docking (the default)
  *(volatile u32*)0xC4 = 0;
  K_FUNC3 = 0;
  K_BU50 = 0;
  K_SWITABLE = (u32)swi_table;
  KV->century = 0x19;
  K_ALARM[0] = 1u << 23;     // the clock counts as set: MC/PS sets it from the PC
  K_ALARM[1] = (u32)font8x8;
  KV->docked_was = (u8)docked();
  RTC_MODE = 0;
  BATT_CTRL = 0;              // the LCD's supply on
  F_WAIT1 = 0;                // no extra wait states for the ROM or the flash
  F_WAIT2 = 0;
  IOP_DIR = 0xF;
  IOP_CLR = 0x7F;
  IOP_SET = 0x42;
  KV->next_index = 0;
  KV->param = 0;
  swi_do_execute(0);
  for (;;) {}
}
