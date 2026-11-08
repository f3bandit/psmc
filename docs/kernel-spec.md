# Our own PocketStation kernel: what it has to do

Written from the public PocketStation documentation (nocash's PSX specifications, psx-spx) and
from watching the 110 test apps run on the real BIOS (`tools/kernel_trace`, see kernel-trace.md).
Nothing here comes from the BIOS's code. This is the interface our kernel must match: the same
calls, the same RAM layout where apps look at it, the same state when an app starts.

## Memory the apps share with the kernel

| Address | What | Seen in the apps |
|---|---|---|
| 000-01Fh | Exception vectors, each `LDR PC,[PC,#18h]` (jumps through the table below) | MGS writes a byte at 000h |
| 020-03Fh | Handler addresses for those vectors (ARM code) | Doraemon 3 installs its own undefined/abort handlers at 024h, 02Ch, 030h, 034h. The Rockman games write a 0 byte at 028h (the SWI handler's address, so its low byte must already be 0) |
| 0C0h | ComFlags (32 bits; SWI 06h returns this address) | 65 apps read it |
| 0C8h | FUNC3 address (16 bits; SWI 17h) | - |
| 0CAh | Byte set by BU command 50h, cleared by SWI 05h | - |
| 0D8h | Alarm setting, 8 bytes (SWI 13h returns this address): minute, hour (BCD), bit 16 alarm on, 17 button lock, 18-19 volume (0 loud, 1 quiet, 2 off), 23 clock set; 0DCh = address of the 8x8 character set | 99 apps read 0D8h (mostly the volume bits); 6 read the character set address |
| 0E0h | Address of the SWI table (SWI 14h returns the address of this word) | 15 apps read it, so they call kernel functions through the table directly: the table and its calling convention must match the SWIs |
| 10C-17Fh | IRQ and SWI stack (top 180h) | Callbacks go up to 68h deep (Doraemon 3: F0h, with its own handlers) |
| 180-1FFh | FIQ stack (top 200h) | Up to 6Ch deep. The Capcom games write 0 at 1FCh |
| 200-7FFh | The app's RAM and stack (stack top 800h) | - |

## When an app starts

Seen identically in all 105 apps that start: user mode, IRQs and FIQs enabled, r0 = the parameter
(0 from the menu), sp = 800h, nothing in r14 to return to. Interrupt mask 0A00h (RTC and docking),
CPU speed 7, all timers stopped, LCD on, infrared in stand-by, sound off. The app's file is mapped
at 02000000h (the whole file, header included); its entry point is the word at +5Ch of its first
block (bit 0 set = Thumb). Callbacks are cleared and communication is off (ComFlags.9 clear).

## Kernel calls (SWI number in the instruction; arguments r0-r2, result r0)

| SWI | Name | Apps using it | Notes |
|---|---|---|---|
| 00h | Reset | 0 | Resets the clock to 1999-01-01 00:00, alarm off, jumps to 0 |
| 01h | SetCallbacks(index, proc) | 104 | 0 SWI2, 1 IRQ, 2 FIQ, 3 download notification; returns the old one. The kernel saves r0, r1, r12 around IRQ/FIQ callbacks |
| 02h | CustomSwi2(...) | 0 | Calls the SWI2 callback in a privileged mode |
| 03h | FlashWriteVirtual(sector, src) | 32 | 80h bytes to a sector of the running file (through the bank mapping); 0 ok, 1 failed (also when past the file's end) |
| 04h | SetCpuSpeed(speed) | 101 | Sets the flash wait states first, then the clock; waits for it; returns the old speed |
| 05h | SenseAutoCom() | 6 | On a docking change: clears ComFlags.8; when docked resets FUNC3 and [0CAh] and turns communication on. The menu calls it every frame |
| 06h | GetPtrToComFlags() | 65 | Returns 0C0h |
| 07h | ChangeAutoDocking(flags) | 0 | Copies bits 16-18 into ComFlags |
| 08h | PrepareExecute(flag, dir_index, param) | 2 | flag 0: returns the current index; flag 1: accepts 0 (the menu) or an MCX0/MCX1 executable file, stores index and param |
| 09h | DoExecute(snapshot_flag) | 0 (apps) | Starts the prepared file or the menu; fails while ComFlags.9 is set |
| 0Ah | FlashReadSerial() | 16 | The unit's serial number |
| 0Bh | ClearComFlagsBit10() | 0 | |
| 0Ch | SetBcdDateTime(date, time) | 0 | |
| 0Dh | GetBcdDate() | 11 | Day, month, 4-digit year (BCD); the century is kept in kernel RAM |
| 0Eh | GetBcdTime() | 35 | Seconds, minutes, hours, day of week (BCD) |
| 0Fh | FlashWriteSerial | 0 | Old BIOS only |
| 10h | FlashWritePhysical(sector, src) | 2 | 80h bytes to a physical sector (0-3FFh), verified; 0 ok, 1 failed |
| 11h | SetComOnOff(flag) | 11 | 0 off (clears ComFlags.9); 1 on: enables the card port interrupt, and when docked sets ComFlags.9 and speed 7 |
| 12h | TestSnapshot(dir_index) | 0 | 1 if the file is MCX1 with a usable snapshot |
| 13h | GetPtrToAlarmSetting() | 99 | Returns 0D8h |
| 14h | GetPtrToPtrToSwiTable() | 15 | Returns 0E0h |
| 15h | MakeAlternateDirIndex(flag, index) | 0 | |
| 16h | GetDirIndex() | 9 | The running file's directory index |
| 17h | GetPtrToFunc3addr() | 0 | Returns 0C8h |
| 18h | FlashReadWhateverByte(sector) | 0 | Byte +7Eh of a flash sector |

Returning to the menu: PrepareExecute(1, 0, GetDirIndex() + 30h), then DoExecute(0). The menu
treats param bits 0-7 as a command (0 erase the clock, 1/20h time screens, 2xh set date and time,
3xh file list) and bit 8 as "don't store the alarm".

## ComFlags (0C0h)

Bits 0-3 docking state (changed by BU commands), 8 docking interrupt seen, 9 communication on and
docked (blocks DoExecute), 10 refuse writes to sectors 16-55, 11 start-a-file request, 16-18
automatic sound / infrared / LCD rotation on dock and undock, 28 a standard memory card command
arrived, 29 set-date request, 30 erase-clock-and-start-menu request.

## Memory card side (docked, BU commands)

Standard: 52h read sector, 53h get ID, 57h write sector. PocketStation: 50h, 58h (version: 2 bytes
01h 01h), 59h prepare execute (index + parameter; FFFEh erase clock and start the menu, FFFFh read
only), 5Ah status (index, ComFlags bits, serial, date, time: 18 bytes), 5Bh/5Ch run a function and
send data to / from the console (FUNC 00h date and time, 01h memory, 02h alarm and flags, 03h the
FUNC3 address, 80h-FFh the file's own function table), 5Dh download notification, 5Eh/5Fh
ComFlags bits.

## Files

Title sector +50h icon frames for the file viewer, +52h "MCX0" or "MCX1", +56h number of icons in
the executable icon list, +57h number of BU 5Bh/5Ch functions, +5Ch entry point. Executables have
"P" as the 7th character of the directory name. MCX1 files carry a snapshot (registers, then user
RAM 200h-7FFh) the kernel can resume from.

## Still to learn by testing

- Timings the apps might depend on (how long SetCpuSpeed waits; how often the kernel's IRQ handler
  runs the RTC work).
- What the menu shows and keeps (ours can differ; MC/PS's "Exit game" check reads the menu's RAM
  marker and will change to match).
- MCX1 snapshot resume (no test app uses it yet).

## Status (0.1.28)

`kernel/` builds `mcps-kernel.bin`, built into the app (no BIOS needed; a BIOS file can still be
chosen). In the 110-file sweep, every app that starts on the real BIOS starts on ours, none
crashes, and the infrared link checks pass the same (Chocobo World, Chocobo World Enhanced,
Odetake Chocobo, MGS Integral, Pocket Digimon World). Remaining differences in the sweep are
games taking other paths for the same timed presses (the two kernels don't take exactly the same
time per call).

Found on the way, by watching the real BIOS: the program's RAM is cleared before it starts; the
callbacks live at 0F8h (Rubato writes the IRQ one itself); IRQ callbacks see the interrupted r2-r12
(games keep a base address in r12); kernel calls mustn't touch the program's stack (MGS reads what
is left below its stack pointer); ComFlags starts at 70000h; programs find timer reloads 20h, 34Fh,
FFFFh and the speaker in stand-by; no flash or ROM wait states at any speed; flash and ROM are 16
bits wide (a byte read is garbage); a program starts when Fire is let go.

Still to do: the memory card side when docked (a console can't use the card yet with ours),
MCX1 snapshots, setting the clock and alarm in the menu.
