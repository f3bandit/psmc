# MC/PS MemCard Pro

A DIY PocketStation and MemCard PRO-style memory card for the PS1, PS2 and the MiSTer PSX core over SNAC. It plays PocketStation mini-games standalone and loads them from Phosphor. By f3bandit.

Based on the MiSTer PocketStation core by Jamie Blanks ([MiSTer-devel/PocketStation_MiSTer](https://github.com/MiSTer-devel/PocketStation_MiSTer), GPL-2.0-or-later).

- `rtl/ps_spi.sv`: the memory card port. Upstream it is an undocked stub; this one answers a console like a retail unit.
- `docs/card-port.md`: how the card port works, taken from the retail kernel
- `sim/`: Verilator simulation with a console-side card-bus harness

Retail BIOS dumps are not included. Users supply their own.
