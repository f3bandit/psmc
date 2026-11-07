# MC/PS MemCard Pro — PocketStation app

Plays PocketStation mini-games on the PC and serves the virtual card on the network. By f3bandit.

- PocketStation: the emulator in `../emu` (hardware after Jamie Blanks' MiSTer core, ARM7TDMI from NanoBoyAdvance).
- Window, sound, controllers: SDL2 (zlib). UI: Dear ImGui (MIT, `third_party/imgui`). Font: Inter (SIL OFL 1.1, `fonts/`).
- The BIOS is your own: put a 16 KB dump beside `MCPS.exe` (`J110.bin`, `boot.rom`, ...), drop it on the window, or choose it in Cards.

## Use

- **Play**: arrows or the d-pad, Z / Enter / Space or controller A for Fire. F2 or controller Back: Reset. LB / RB: Play / Cards.
- **Cards**: the card lives in `PocketStation.mcd` beside the exe and saves itself. Drop `.mcs` saves or mini-games on the window to add them; drop a whole card (`.mcd .mcr .gme ...`) to swap it in (the old one is kept as `.bak`).
- **Alerts**: when a game calls (it beeps out of a quiet spell while nobody is playing, or lights its LED), the app shows who's calling and flashes in the taskbar. Chocobo World is read from its save and says what happened: an event is waiting, Boko levelled up, ranked up, found an item, lost a battle. The rules live in `alerts.cpp`; `tools/alerts_run` plays an app untouched and prints its alerts.
- **Infrared link**: two units on the same network (phones or PCs running this app) link for games that use infrared (MGS Integral's IDs and others). One side presses **Wait for a link**; the other picks it from the list (or types its address). At the start each sends the other its unit once; from then on both sides run both units in lockstep with their infrared wired together, and only button presses cross the network, so Wi-Fi lag doesn't matter. Each keeps its own unit when the link ends. TCP and UDP port 7551 (Windows asks to let the app on the network the first time). Resetting, swapping the card or a console using the card ends the link. `link.cpp`; `tools/link_test` runs an MGS transfer over it.
- **Network card**: TCP port 7550 (`port=` in `mcps.ini`). Whole commands come in as `card_raw HEX` and run through the emulated card bus at console timing; replies match Phosphor's API. The unit docks while a console talks to it and undocks 3 s later.

## Build

```
make                                              # Linux, for testing
make win SDL2=/path/to/SDL2-2.30.9/x86_64-w64-mingw32   # Windows exe
```
