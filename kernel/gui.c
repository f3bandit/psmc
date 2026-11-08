// MC/PS kernel: the menu. The clock, then each program on the card: Left and Right to move,
// Fire to start one. Runs like a program (user mode, its RAM from 200h), started with a
// parameter: 3xh opens on program x (a program returning sends 30h + its own index).
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
#include "hw.h"

static inline u32 swi4(u32 n, u32 a, u32 b, u32 c) {
  register u32 r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c;
  switch (n) {   // the number is part of the instruction
    case 0x04: __asm__ volatile("swi 0x04" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "r12", "lr", "memory"); break;
    case 0x08: __asm__ volatile("swi 0x08" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "r12", "lr", "memory"); break;
    case 0x09: __asm__ volatile("swi 0x09" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "r12", "lr", "memory"); break;
    case 0x0E: __asm__ volatile("swi 0x0E" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "r12", "lr", "memory"); break;
    case 0x05: __asm__ volatile("swi 0x05" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "r12", "lr", "memory"); break;
    case 0x0D: __asm__ volatile("swi 0x0D" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "r12", "lr", "memory"); break;
  }
  return r0;
}

struct Gui {
  u32 fb[32];
  u8 apps[16];      // directory indexes of the programs on the card
  u8 count;
  u8 pos;           // 0 = the clock
  u8 prev_buttons;
  u8 pad;          // Fire pressed on a program: start it on the release
  u32 frame;
};
#define G ((struct Gui*)0x260)

// 3x5 digits and capitals (each glyph: 5 rows of 3 bits, bit 0 = left).
static const u16 small_font[] = {
  // 0-9
  0x7B6F, 0x749A, 0x73E7, 0x79E7, 0x49ED, 0x79CF, 0x7BCF, 0x4927, 0x7BEF, 0x79EF,
};

static void pix(int x, int y) {
  if (x >= 0 && x < 32 && y >= 0 && y < 32) G->fb[y] |= 1u << x;
}

static void digit3x5(int x, int y, int d) {
  u16 g = small_font[d];
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 3; c++)
      if (g >> (r * 3 + c) & 1) pix(x + c, y + r);
}

// A big digit: the 3x5 glyph at double size (6x10).
static void digit_big(int x, int y, int d) {
  u16 g = small_font[d];
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 3; c++)
      if (g >> (r * 3 + c) & 1) { pix(x + 2 * c, y + 2 * r); pix(x + 2 * c + 1, y + 2 * r); pix(x + 2 * c, y + 2 * r + 1); pix(x + 2 * c + 1, y + 2 * r + 1); }
}

static void show(void) {
  for (int i = 0; i < 32; i++) LCD_VRAM[i] = G->fb[i];
}

static void scan(void) {
  G->count = 0;
  for (u32 i = 1; i <= 15; i++) {
    u32 t = FLASH_BASE + i * 0x2000;
    if (rd8(FLASH_BASE + i * 0x80) != 0x51 || *(volatile u16*)t != ('S' | 'C' << 8)) continue;
    if (rd8(FLASH_BASE + i * 0x80 + 0x0A + 6) != 'P') continue;   // executables are named "B?????P..."
    if (*(volatile u16*)(t + 0x52) != ('M' | 'C' << 8) || rd8(t + 0x54) != 'X') continue;
    G->apps[G->count++] = (u8)i;
  }
  if (G->pos > G->count) G->pos = 0;
}

static void draw_clock(void) {
  u32 t = swi4(0x0E, 0, 0, 0), d = swi4(0x0D, 0, 0, 0);
  u32 h = (t >> 16) & 0xFF, m = (t >> 8) & 0xFF, s = t & 0xFF;
  digit_big(1, 4, h >> 4);
  digit_big(8, 4, h & 15);
  if (s & 1) { pix(15, 7); pix(15, 11); }
  digit_big(18, 4, m >> 4);
  digit_big(25, 4, m & 15);
  u32 mo = (d >> 8) & 0xFF, dy = d & 0xFF;
  digit3x5(6, 20, mo >> 4);
  digit3x5(10, 20, mo & 15);
  pix(15, 24);
  digit3x5(18, 20, dy >> 4);
  digit3x5(22, 20, dy & 15);
}

// The program's save icon (16x16, 16 colours) at double size: dark colours on.
static void draw_app(u32 index) {
  u32 t = FLASH_BASE + index * 0x2000;
  u8 dark[16];
  for (int c = 0; c < 16; c++) {
    u16 v = *(volatile u16*)(t + 0x60 + 2 * c);
    u32 r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
    dark[c] = (r * 3 + g * 6 + b) < 150;
  }
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      u8 px = rd8(t + 0x80 + y * 8 + x / 2);
      u8 c = x & 1 ? px >> 4 : px & 15;
      if (dark[c]) { pix(2 * x, 2 * y); pix(2 * x + 1, 2 * y); pix(2 * x, 2 * y + 1); pix(2 * x + 1, 2 * y + 1); }
    }
}

void gui_main(u32 param) {
  for (u32 a = 0x200; a < 0x300; a += 4) *(volatile u32*)a = 0;
  // Marks the menu's screens for the MC/PS app ("Exit game" and alerts look for it).
  *(volatile u8*)0x230 = 1;
  *(volatile u8*)0x240 = 1;
  *(volatile u8*)0x236 = 1;
  swi4(0x04, 5, 0, 0);
  scan();
  if ((param & 0xF0) == 0x30) {
    for (int i = 0; i < G->count; i++)
      if (G->apps[i] == (param & 15)) G->pos = (u8)(i + 1);
  }
  G->prev_buttons = 0x1F;   // a button still held from before isn't a press
  for (;;) {
    G->frame++;
    swi4(0x05, 0, 0, 0);   // SenseAutoCom, every frame
    if ((G->frame & 31) == 0) scan();
    u8 b = INT_INPUT & 0x1F;
    u8 pressed = b & ~G->prev_buttons;
    G->prev_buttons = b;
    u8 n = G->count + 1;
    if (pressed & (1 << I_RIGHT)) G->pos = (u8)((G->pos + 1) % n);
    if (pressed & (1 << I_LEFT)) G->pos = (u8)((G->pos + n - 1) % n);
    if ((pressed & (1 << I_FIRE)) && G->pos > 0) G->pad = 1;
    // Starts on the release: a program shouldn't find Fire still held from the menu.
    if (G->pad && !(b & (1 << I_FIRE))) {
      G->pad = 0;
      u32 idx = G->apps[G->pos - 1];
      if (G->pos > 0 && swi4(0x08, 1, idx, 0) == idx) swi4(0x09, 0, 0, 0);
    }
    for (int i = 0; i < 32; i++) G->fb[i] = 0;
    if (G->pos == 0) draw_clock();
    else draw_app(G->apps[G->pos - 1]);
    show();
    for (volatile int w = 0; w < 100; w++) {}
  }
}
