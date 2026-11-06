# MC/PS MemCard Pro — PocketStation app

Plays PocketStation mini-games on the PC and serves the virtual card on the network. By f3bandit.

- PocketStation: the emulator in `../emu` (hardware after Jamie Blanks' MiSTer core, ARM7TDMI from NanoBoyAdvance).
- Window, sound, controllers: SDL2 (zlib). UI: Dear ImGui (MIT, `third_party/imgui`). Font: Inter (SIL OFL 1.1, `fonts/`).
- The BIOS is your own: put a 16 KB dump beside `MCPS.exe` (`J110.bin`, `boot.rom`, ...), drop it on the window, or choose it in Cards.

## Use

- **Play**: arrows or the d-pad, Z / Enter / Space or controller A for Fire. F2 or controller Back: Reset. LB / RB: Play / Cards.
- **Cards**: the card lives in `PocketStation.mcd` beside the exe and saves itself. Drop `.mcs` saves or mini-games on the window to add them; drop a whole card (`.mcd .mcr .gme ...`) to swap it in (the old one is kept as `.bak`).
- **Network card**: TCP port 7550 (`port=` in `mcps.ini`). Whole commands come in as `card_raw HEX` and run through the emulated card bus at console timing; replies match Phosphor's API. The unit docks while a console talks to it and undocks 3 s later.

## Build

```
make                                              # Linux, for testing
make win SDL2=/path/to/SDL2-2.30.9/x86_64-w64-mingw32   # Windows exe
```
