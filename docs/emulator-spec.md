# PocketStation emulator: hardware spec

What the software emulator for the Windows app has to model, taken block by block from Jamie Blanks' MiSTer core ([MiSTer-devel/PocketStation_MiSTer](https://github.com/MiSTer-devel/PocketStation_MiSTer), GPL-2.0-or-later, `rtl/asic/*.sv`). His core is the reference: the emulator is checked against it running in Verilator (`sim/`), same BIOS, same inputs.

The CPU is an ARM7TDMI, run by NanoBoyAdvance's interpreter ([nba-emu/NanoBoyAdvance](https://github.com/nba-emu/NanoBoyAdvance), GPL-3.0-or-later). The whole emulator is therefore GPL-3.0-or-later.

## Clocks

| Clock | Rate | Drives |
|---|---|---|
| Crystal | 32768 Hz | RTC, LCD row timing, flash program delay, wake sampling, low-voltage delay |
| System | PMFrequency table below | CPU, bus, timers, IrDA |

PMFrequency (`0x0B000000`, FREQ[3:0]): 0 = 32768, 1 = 63488, 2 = 126976, 3 = 253952, 4 = 507904, 5 = 1015808, 6 = 1998848, 7 = 3997696, 8 and above = 7995392 Hz. LOCK (bit 4) reads 1.

PMStandby (`0x0B000004`, bit 0) stops the system clock: CPU, bus and timers halt. The crystal keeps running, so the RTC, the LCD and the flash program delay keep going. Wake is an *edge* on an armed line, sampled on the crystal: Enter always, plus RTC, low voltage and dock (both edges) when their interrupt is enabled. In the emulator, standby is a fast-forward to the next crystal event.

## Memory map and bus

| Region | Address | Notes |
|---|---|---|
| SRAM | `0x01000000` (2 KB), and `0x00000000` once GENREM is set | 32-bit, byte lanes |
| Boot ROM | `0x04000000` (16 KB), and `0x00000000` before GENREM | 16-bit device |
| Virtual flash | `0x02000000`–`0x0201FFFF` | through the block mapping table; unmapped block = abort |
| Physical flash | `0x08000000` (128 KB) | 16-bit device |
| Memory controller | `0x06000000` | signature halfwords at `0x06000300` |
| Interrupts / timers | `0x0A000000` / `0x0A800000` | |
| Clock control / RTC | `0x0B000000` / `0x0B800000` | |
| Card port / IrDA | `0x0C000000` / `0x0C800000` | |
| LCD / IO | `0x0D000000` / `0x0D800000` | IO: PIO 0x00, DAC 0x10, LVD 0x20 |
| anything else | | abort |

Access cost in system clocks:

| Region | ARM fetch | Thumb fetch | Data |
|---|---|---|---|
| SRAM, memory controller | 1 | 1 | 1 |
| Boot ROM, flash | 2 | 1 | 2 |
| Other device registers | – | – | 2 |

A 32-bit access to a 16-bit device takes two halfword beats. FLASHDataController WAIT (bit 4) adds one cycle to flash; the boot wait register (`0x0600000C` bit 4) adds one to boot ROM.

A byte read of boot ROM, flash or the signature returns `prefetch | (last SRAM read & 0xFFFFFFD0)`: the last fetched opcode word (a Thumb fetch replaces only its upper half) mixed with the last SRAM data read. One commercial title depends on this.

## Memory controller (`0x06000000`)

- `+0x00` REGRemap: bit 0 GENREM (sticky once set), bit 1 FLASHVIR. Setting FLASHVIR only takes if the mapping is valid (at least one active block, no two active blocks on the same virtual slot); otherwise it reads 0. Byte writes allowed.
- `+0x04` FLASHREMAPStatus: MULTIPLE, NOACTIVE, DABORT, FABORT; clears when read.
- `+0x08` FLASHACTIVEBlocks: 16 bits, writable only while FLASHVIR is 0.
- `+0x0C` boot memory wait: 5 bits, bit 4 = one extra cycle.
- `+0x10` FLASHDataController, reset `0x14`: ENPRG 0, LOCK 1, BUSY 2 (reads the sequencer: 1 idle, 0 busy), STDBY 3, WAIT 4, LOADPAGE 5, LOADSGN 6.
- `+0x100`–`+0x13C` FLASHVIRTUALAddr0..15: indexed by *physical* block, holds the virtual block; writable only while FLASHVIR is 0. Translation searches by value, lowest physical block wins.

## Flash program sequence

1. Set LOADPAGE and ENPRG (LOCK clear).
2. `FFAA` to `080055AA`, `FF55` to `08002A54`, `FFA0` to `080055AA`.
3. Up to 64 halfwords into one 128-byte sector; the first write picks the sector. Halfwords not written come back `FFFF`.
4. The sector commits after 4 crystal ticks with no write (about 100 µs), on a write outside the sector, or when ENPRG/LOADPAGE drop. Programming then takes 655 crystal ticks (about 20 ms) with BUSY reading 0.

With LOADSGN instead of LOADPAGE, writes to the first 256 bytes go to the signature (serial low `+0x00`, serial high `+0x02`, LCD calibration `+0x08`).

## Interrupt controller (`0x0A000000`)

`+0x00` latched status, `+0x04` raw levels, `+0x08` enable set (reads the mask), `+0x0C` enable clear, `+0x10` latch clear.

Sources: 0 Enter, 1 Right, 2 Left, 3 Down, 4 Up, 5 unused, 6 card port, 7 timer 0, 8 timer 1, 9 RTC, 10 low voltage, 11 dock (supply line), 12 IR, 13 timer 2. **6 and 13 go to FIQ**, the rest to IRQ.

Every source latches on its rising edge; 11 and 12 also on the falling edge. A button's latch is a pulse: it clears by itself after 1023 system clocks and also clears on release. Edges are sampled on system-clock cycles, so they stop in standby.

## Timers (`0x0A800000 + n*0x10`, n = 0..2)

`+0x0` load (reset `FFFF`), `+0x4` value, `+0x8` control (bits 1:0 divider: 2, 32, 512 system clocks; bit 2 enable).

- Load P expires every P+1 ticks (zero is a real state). Reload happens at expiry, independent of the handler.
- While stopped, the value follows the load register. Writing load while running takes effect at the next reload.
- Enabling restarts the divider but keeps the value.
- Registers are 16 bits; the upper half of a wider write is dropped.

## RTC (`0x0B800000`)

`+0x0` control: PRGSEL bit 0 (programming mode), CNTSEL bits 3:1 (field). `+0x4` INCCNT: two writes of 1 step the selected field. `+0x8` clock {day of week, hour, min, sec}, `+0xC` calendar {year, month, date}, all BCD. Reset: Wednesday 00:00:00, 1998-01-01.

The interrupt line is a square wave: 1 Hz running, 4096 Hz in programming mode. The time advances on the falling edge of the 1 Hz wave. The first pulse after a mode change is skipped.

## LCD (`0x0D000000`)

`+0x000` control: DISMOD 2:0 (blank 8-row segments), CPEN 3, FR 5:4 (1 = 64 Hz, 2 = 32 Hz, 3 = 16 Hz, 0 = stopped), DISON 6, ROT 7 (180° turn: reverses row order and bit order). `+0x004` calibration (6 bits, from the signature at reset). `+0x100`–`+0x17C` 32 rows of 32 bits, word writes only (narrow writes are dropped).

The picture is on when DISON, CPEN and the band-gap reference (LVD) are all on. In the emulator: draw from VRAM once per frame.

## Sound (`0x0D800010`)

DACControl bit 0 active; DACData bits 15:6 are a 10-bit signed level (write only). Audible only while DAC active **and** PIO port 5 low (speaker not in standby). No tone generator: software writes every sample, usually from a timer interrupt.

## PIO (`0x0D800000`)

`+0x0` direction for ports 0–3, `+0x4` set, `+0x8` clear, `+0xC` read. Port 1 = LED (low = on), port 4 = dock supply (input), port 5 = speaker standby (high = off), port 6 = IR output level. Byte writes allowed.

## Low voltage (`0x0D800020`)

Control reset `3`: bit 0 band-gap standby (also blanks the LCD), bit 1 comparator standby, bit 2 periodic. With a healthy battery it never fires.

## IrDA (`0x0C800000`)

Static control (reset `2`) and dynamic control registers; with no partner the receiver idles and interrupt 12 stays low.

## Card port (`0x0C000000`)

Our `rtl/ps_spi.sv`, see `card-port.md`. In the emulator the console side runs in-process from whole commands (`card_raw`), the same as the network server.
