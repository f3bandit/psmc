# PocketStation device — Verilator sim

Simulation of the PocketStation core for the DIY PocketStation / memory card device, by f3bandit.

Core: [MiSTer-devel/PocketStation_MiSTer](https://github.com/MiSTer-devel/PocketStation_MiSTer) by Jamie Blanks (GPL-2.0-or-later). Everything is used unmodified except `ps_spi.sv`, the memory card port, which upstream is an undocked stub and is replaced by `../rtl/ps_spi.sv` (see `../docs/card-port.md`). `ps_sim_top.sv` wires `ps_clocks` + `pocketstation_board` the same way the MiSTer top does, minus the MiSTer framework.

## Build and run

```
git clone --depth 1 https://github.com/MiSTer-devel/PocketStation_MiSTer.git ../../PocketStation_MiSTer
make
./ps_sim --seconds 3 --snap 0.5 --press enter@1.0 --out out
```

- `--bios FILE` boot ROM (default: the core's open enhanced BIOS; a retail dump works too)
- `--press BTN@T[+DUR]` enter/right/left/down/up, times in seconds
- LCD snapshots land in `out/` as 160×160 PGM (5× scale); final frame is also printed as ASCII
- Logs LED, CPU clock select, standby and speaker changes
- `--card FILE` loads a 128 KiB card image (default: freshly formatted)
- `--card-test T` acts as a PS1 at T seconds: ID, read frames 0/1, write frame 63, read it back, check flash
- `--cmd-gap MS`, `--dump-card FILE`, `--dock T`
- `make ps_sim_trace` builds a variant that logs every COM register access

The open BIOS doesn't answer the card bus (out of scope upstream), so card tests need a retail BIOS:

```
./ps_sim --bios J110.bin --seconds 1.4 --card-test 1.0
```

Retail BIOS dumps stay out of the repo; users supply their own.

Speed: `ps_sim` (32 MHz system clock, as on MiSTer) runs about 20× slower than real time. `make ps_sim_8m` builds with an 8 MHz system clock (244 × the crystal, the CPU's top rate) and runs about 6× slower, with the same card-test results.

## Virtual PocketStation on the network

```
./ps_sim_8m --bios J110.bin --serve 7550
../tools/ps_probe.py --tcp 127.0.0.1:7550 --bios dump.bin
```

`--serve` runs the unit docked and takes whole commands over TCP (`card_raw HEX`, `press`, `lcd`, `status`, `dock`; replies match Phosphor's API). A console gives a card about 100 µs to ack each byte, so bytes can't cross the network one at a time: each command goes through the emulated card bus here, at console timing.

## Next

- 58h–5Fh PocketStation commands in the harness
- Replay of Pico SNAC-tap captures from a real PocketStation on the SS1
