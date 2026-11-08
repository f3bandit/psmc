// MC/PS kernel: the PocketStation's registers, from the public documentation.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
#pragma once
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

#define REG(a) (*(volatile u32*)(a))

// Interrupts (bit numbers)
#define I_FIRE 0
#define I_RIGHT 1
#define I_LEFT 2
#define I_DOWN 3
#define I_UP 4
#define I_COM 6      // card port (FIQ)
#define I_TMR0 7
#define I_TMR1 8
#define I_RTC 9
#define I_DOCK 11
#define I_IRDA 12
#define I_TMR2 13    // (FIQ)
#define INT_LATCH REG(0x0A000000)
#define INT_INPUT REG(0x0A000004)
#define INT_MASK REG(0x0A000008)      // read: the mask; write: set bits
#define INT_MASK_CLR REG(0x0A00000C)
#define INT_ACK REG(0x0A000010)

#define TMR_LOAD(n) REG(0x0A800000 + (n) * 0x10)
#define TMR_VALUE(n) REG(0x0A800004 + (n) * 0x10)
#define TMR_CTRL(n) REG(0x0A800008 + (n) * 0x10)

#define CLK_MODE REG(0x0B000000)
#define CLK_STOP REG(0x0B000004)

#define RTC_MODE REG(0x0B800000)
#define RTC_ADJUST REG(0x0B800004)
#define RTC_TIME REG(0x0B800008)
#define RTC_DATE REG(0x0B80000C)

#define COM_MODE REG(0x0C000000)
#define COM_STAT REG(0x0C000004)
#define COM_DATA REG(0x0C000008)
#define COM_CTRL1 REG(0x0C000010)
#define COM_CTRL2 REG(0x0C000018)

#define IRDA_MODE REG(0x0C800000)
#define IRDA_DATA REG(0x0C800004)

#define LCD_MODE REG(0x0D000000)
#define LCD_CAL REG(0x0D000004)
#define LCD_VRAM ((volatile u32*)0x0D000100)

#define IOP_DIR REG(0x0D800000)
#define IOP_SET REG(0x0D800004)
#define IOP_CLR REG(0x0D800008)
#define IOP_DATA REG(0x0D80000C)
#define DAC_CTRL REG(0x0D800010)
#define DAC_DATA REG(0x0D800014)
#define BATT_CTRL REG(0x0D800020)

#define F_CTRL REG(0x06000000)
#define F_STAT REG(0x06000004)
#define F_BANK_FLG REG(0x06000008)
#define F_WAIT1 REG(0x0600000C)
#define F_WAIT2 REG(0x06000010)
#define F_BANK_VAL(n) REG(0x06000100 + (n) * 4)
#define F_SN_LO (*(volatile u16*)0x06000300)
#define F_SN_HI (*(volatile u16*)0x06000302)

// Flash and this ROM are 16 bits wide: a byte read returns garbage, so bytes come out of a
// halfword read.
static inline u8 rd8(u32 a) {
  u16 h = *(volatile u16*)(a & ~1u);
  return (a & 1) ? (u8)(h >> 8) : (u8)h;
}
#define FLASH_BASE 0x08000000u
#define FLASH16 ((volatile u16*)0x08000000)
#define VFLASH ((volatile u8*)0x02000000)

// Kernel RAM, as the documentation lays it out (the parts apps see), and ours after 040h.
#define K_COMFLAGS (*(volatile u32*)0xC0)
#define K_FUNC3 (*(volatile u16*)0xC8)
#define K_BU50 (*(volatile u8*)0xCA)
#define K_ALARM ((volatile u32*)0xD8)
#define K_SWITABLE (*(volatile u32*)0xE0)
// The callbacks (SWI 01h) live at 0F8h..107h: SWI2, IRQ, FIQ, download notification. Some
// programs (Rubato) write the IRQ one at 0FCh directly.
#define K_CALLBACK ((volatile u32*)0xF8)

struct KVars {               // at 040h
  u32 param;                 // 040h: for the next DoExecute
  u16 dir_index;             // 044h: the running file (0 = the menu)
  u16 next_index;            // 046h: prepared by PrepareExecute
  u16 alt_index;             // 048h
  u8 century;                // 04Ah: BCD
  u8 com_on;                 // 04Bh
  u8 docked_was;             // 04Ch
  u8 pad[3];
};                           // 050h..05Fh spare; 060h..0BFh the SWI stack
#define KV ((volatile struct KVars*)0x40)
