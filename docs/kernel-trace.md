# What each app needs from the kernel

Recorded with the real BIOS by `tools/kernel_trace` (15 s per app: launched from the menu, then a
press every 0.4 s). Only what crosses between an app and the kernel: the kernel calls (SWI) it
makes, the kernel RAM (0..1FFh) its own code reads or writes (w = writes), and how deep its
interrupt callbacks reach into the kernel's IRQ stack (top 180h) and FIQ stack (top 200h).
Every app starts the same way: user mode, interrupts on, r0 = 0, sp = 800h, interrupt mask 0A00h
(RTC, docking), CPU speed 7, timers off, LCD on, infrared in stand-by. Raw output:
kernel-trace-raw.txt.

| App | Kernel calls | Kernel RAM it touches | IRQ / FIQ stack |
|---|---|---|---|
| 15 Puzzle (19xx)(Nissie) | 01 04 0e 13 | 0d8 | 50h / 0h |
| Aliens (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Ana-Digi Clock (19xx)(Mets) | 01 04 0d 0e 13 | 0d8 | 50h / 0h |
| Ana-Digi Clock (19xx)(Mets)[a] | 01 04 0d 0e 13 | 0d8 | 50h / 0h |
| Apple (1999)(SN Systems) | 01 04 11 13 | 0d8 | 38h / 20h |
| Apple v0.2 (19xx)(Kami) | 01 04 11 13 | 0d8 | 38h / 20h |
| Astro Fighter (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Beat Timer v0.02 (19xx)(-) | 01 04 0d 0e | - | 50h / 0h |
| Black Jack (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| Block Out 1 (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| Block Out 2 (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| Block Out 3 (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| BreakThru (1999)(Jason) (SN Systems dev-kit file, not a PocketStation file) | - | - | 0h / 0h |
| Brightis (1999)(SCEI)(JP)[Brightis] | 01 04 0a 0e 10 13 16 | 0d8 | 3Ch / 18h |
| Calculator (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Cars (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Chaku v0.02t (19xx)(-) | 01 04 | - | 38h / 0h |
| Chivas 1-2-3 (1999)(Jaleco)[SeaBass 1-2-3] | 01 06 13 | 0c0 0d8 | 2Ch / 50h |
| Chocobo Stallion (1999)(Square)[Chocobo Stallion] | 01 04 0a 0e 11 13 16 | 044 0d8 | 3Ch / 38h |
| Chocobo World (1999)(Square)[Final Fantasy 8] | 01 03 04 05 06 0a 0e 11 13 | 0c0 0d8 | 30h / 0h |
| Chocobo World Enhanced (1999)(Square)[Final Fantasy 8] | 01 03 04 05 06 0a 0e 11 13 | 0c0 0d8 | 30h / 0h |
| Copy Dance (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Curling (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| DTMF Hack Beta 2 (19xx)(Error) | 01 04 13 | 0d8 | 50h / 48h |
| Dance Dance Revolution 2nd Mix (2000)(Konami)(JP)[Dance Dance Revolution 2nd Mix] | 01 03 04 08 11 13 16 | 0d8 | 68h / 64h |
| Darts (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Denki (19xx)(-) | 01 04 0e 13 | 0d8 | 50h / 0h |
| Dice (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Doreamon 3 (2000)(Epoch)(JP)[Doraemon 3 - Makai no Dungeon] | 01 04 06 13 | 024w 02cw 030w 034w 0c0 0d8 | F0h / 0h |
| Drive 2000 (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Drop Ball (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Drop Zone (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Durama (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| EnPKclock v1.4 (19xx)(-)(JP) | 01 04 0d 0e 13 | 0d8 | 50h / 0h |
| Fishing (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Fishing 2 (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Go Go Digger (2000)(Square)[Saga Frontier 2] | 01 04 13 14 | 0d8 0e0 | 34h / 0h |
| Golf (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Hammer (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Harvest Moon (2001)(Natsume)[Harvest Moon] | 01 04 0a 0e 13 16 | 0d8 | 38h / 0h |
| Hata v0.02 (19xx)(-) | 01 04 0e 13 | 0d8 | 50h / 58h |
| Hit & Blow (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Hit And Blow v0.81 (19xx)(P. Katsu) | 01 04 0e 13 | 0d8 | 50h / 0h |
| Hitman (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| Integral v0.02 (19xx)(-) | 01 04 13 | 0d8 | 50h / 58h |
| Japanese Quiz (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| Jet De Go! (2000)(Taito)(JP)[Jet De Go!] | 01 04 06 13 14 | 0c0 0d8 0dc 0e0 | 44h / 60h |
| Kobun No Tokkun Zanmai (1999)(Capcom)(JP)[Tron ni Kobun] | 01 04 06 13 14 | 0c0 0d8 0e0 | 44h / 20h |
| Lights Out (19xx)(-) | 01 04 11 13 | 0d8 | 38h / 20h |
| Line Art v0.01t (19xx)(-) | 01 04 0e | - | 38h / 0h |
| Lo (1999)(SN Systems) | 01 04 13 | 0d8 | 34h / 20h |
| MGS Integral (1999)(Konami)[Metal Gear Solid Integral] | 01 03 04 06 0a 13 14 16 | 000w 0c0 0d8 0e0 110w 180w | 58h / 44h |
| Math Quiz (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| Maze Game (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Mimique (1999)(K-Comm) | 01 04 13 | 0d8 | 38h / 28h |
| Mine Sweeper (19xx)(P. Katsu)(PD) | 01 04 0e 13 | 0d8 | 50h / 0h |
| MoGuuRu Dabas (1999)(SCEI)(JP)[Legend Of Dragoon] | 01 04 0e 13 | 0d8 | 34h / 28h |
| Monkey See Monkey Do (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| Number (19xx)(-) | 01 03 04 0e 13 | 0d8 | 50h / 0h |
| Odetake Chocobo (1999)(Square)[Final Fantasy 8] | 01 03 04 05 06 0a 0e 11 13 | 0c0 0d8 | 30h / 0h |
| PDA Calculator (1998)(SCEI) | 01 04 06 13 | 0c0 0d8 | 48h / 0h |
| PDA Remote 1 (1998)(SCEI)(JP) | 01 04 05 11 13 | 0d8 | 48h / 0h |
| PDA Remote 2 (1998)(SCEI)(JP) | 01 04 05 11 13 | 0d8 | 48h / 0h |
| PDA Timer (1998)(SCEI) | 01 04 05 06 08 11 13 16 | 0c0 0d8 | 48h / 0h |
| PacClone (1999)(Jason) (SN Systems dev-kit file, not a PocketStation file) | - | - | 0h / 0h |
| Pairs (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Parumui Parumui (19xx)(-)[Parumui Parumui] | 01 04 06 10 13 | 0c0 0d8 | 34h / 44h |
| Phone Book (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| Pinball (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Pockeler (1999)(Atlus)(JP)[Pockeler] | 01 03 04 0a 0d 0e 13 16 | 0d8 | 4Ch / 0h |
| Pocket Digimon World - Agumon (2000)(Bandai)[Pocket Digimon World] | 01 04 06 0d 0e 13 14 | 0c0 0d8 0e0 | 30h / 2Ch |
| Pocket Digimon World - Biyomon (2000)(Bandai)[Pocket Digimon World] | 01 04 06 0d 0e 13 14 | 0c0 0d8 0e0 | 30h / 2Ch |
| Pocket Fighter Alpha 3 - Part 1 (1999)(Capcom)[Street Fighter Alpha 3] | 01 03 04 06 0a 0e 13 14 | 0c0 0d8 0e0 1fcw | 48h / 28h |
| Pocket Fighter Alpha 3 - Part 2 (1999)(Capcom)[Street Fighter Alpha 3] | 01 03 04 06 0a 0e 13 14 | 0c0 0d8 0e0 1fcw | 48h / 28h |
| Pocket Fighter Zero 3 (1998)(Capcom)(JP)[Street Fighter Alpha 3] | 01 03 04 06 0a 0e 13 14 | 0c0 0d8 0e0 1fcw | 48h / 28h |
| Pocket Ray (2000)(Taito)[RayCrisis] | 01 04 06 13 14 | 0c0 0d8 0dc 0e0 | 1Ch / 48h |
| Pocket Sokoban (19xx)(Mets) | 01 04 13 | 0d8 | 38h / 0h |
| Pocket Timer v1.11 (19xx)(-) | 01 04 13 | 0d8 | 54h / 0h |
| PokeHito (1999)(SCEI)(JP)[PokeHito - Grey] | 01 04 06 0e 13 16 | 0c0 0d8 | 2Ch / 40h |
| Poketro v1.1 (2001)(Credo) | 01 | - | 50h / 0h |
| Pop n Music 2 (1999)(Konami)(JP)[Pop n Music 2] | 01 04 06 13 | 0c0 0d8 | 34h / 48h |
| Pumping Crash (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Punch Block (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Push (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Push Battle (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Pyramid (19xx)(P. Katsu)(JP) | 01 04 0e 13 | 0d8 | 50h / 0h |
| R4 Pocketstation (1998)(Namco)[R4 Ridge Racer Type 4] | 01 04 06 13 | 0c0 0d8 | 38h / 50h |
| RPG Dungeon (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 30h / 28h |
| Rewrite ID (1999)(K-Comm) | 01 04 0a 13 | 0d8 | 38h / 0h |
| Ring Ring Land (1999)(Square)(JP)[Seiken Densetsu - Legend of Mana] | 01 04 06 13 14 | 0c0 0d8 0e0 | 3Ch / 0h |
| Rocket (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Rockman 2 (1999)(Capcom)(JP)[Rockman 2] | 01 03 04 06 0a 0d 0e 13 14 | 028w 0c0 0d8 0dc 0e0 1fcw | 48h / 28h |
| Rockman 3 - Part 2 (1999)(Capcom)(JP)[Rockman 3] | 01 03 04 06 0a 0d 0e 13 14 | 028w 0c0 0d8 0dc 0e0 1fcw | 48h / 28h |
| Rockman 4 (1999)(Capcom)(JP)[Rockman 4] | 01 03 04 06 0a 0d 0e 13 14 | 028w 0c0 0d8 0dc 0e0 1fcw | 48h / 28h |
| Rockman 5 (1999)(Capcom)(JP)[Rockman 5] | 01 03 04 06 0a 0d 0e 13 14 | 028w 0c0 0d8 0dc 0e0 1fcw | 48h / 28h |
| Rubato v0.03 (1999)(K-Comm) | 16 | 028w 0fcw 180w | 1Ch / 0h |
| Sample (1999)(SN Systems) (SN Systems dev-kit file, not a PocketStation file) | - | - | 0h / 0h |
| Ski Escape (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Slot Machine (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Smash Game (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Sporting Clays (1999)(Charles Doty) (SN Systems dev-kit file, not a PocketStation file) | - | - | 0h / 0h |
| Tank (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 04 06 13 | 0c0 0d8 | 30h / 28h |
| Tasumania (19xx)(-) | 01 04 0e | - | 54h / 0h |
| Tetris (1999)(SN Systems) | 01 04 0e 13 | 0d8 | 50h / 0h |
| Tetris (19xx)(-) | 01 04 0e 13 | 0d8 | 50h / 0h |
| Tic-Tac-Toe v1.00 (19xx)(-) | 01 13 | 0d8 | 50h / 6Ch |
| World Time (1999)(SCEI)(JP)[Pocket MuuMuu] | 01 03 04 06 13 | 0c0 0d8 | 3Ch / 28h |
| Wose (19xx)(-)(JP) | 01 04 0e 13 | 0d8 | 50h / 0h |
| Yubi v0.60 (19xx)(P. Katsu)(JP) | 01 04 0e 13 | 0d8 | 50h / 0h |
