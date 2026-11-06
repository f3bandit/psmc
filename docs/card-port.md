# PocketStation memory card port: how it works

How a retail PocketStation answers a PS1 on the memory card bus, worked out from the J110 retail kernel (disassembly) and confirmed in the Verilator sim of the PocketStation core with our `rtl/ps_spi.sv`. Addresses below are J110 kernel addresses.

Sources: J110 kernel code; register names from [psx-spx](https://forge.chapril.org/ABelliqueux/psx-spx.github.io/src/commit/93c1c7bfff112aac6f82781d61112cdf2df31bd5/pocketstation.md) and Jamie Blanks' `ps_spi.sv` header; bus timing from Jamie's `ps_snac_link.sv`.

## Registers (0x0C000000)

| Offset | Name | Behaviour (as the kernel uses it) |
|---|---|---|
| 00h | MODE | bit 0 drive DAT; bit 1 pull ACK low. The kernel writes 1 then 3 for every byte, so setting bit 1 must produce one ACK **pulse**, not a level. Bit 2 is set while idle (06h). |
| 04h | STAT1 | bit 0 byte ready; bit 1 select released (end of command). Read once after each byte. |
| 08h | DATA | read: byte received from the console; write: reply byte sent on the console's next 8 clocks. |
| 10h | CTRL1 | 02h arm the link (idle), 03h at the start of a command, 00h after a command. |
| 14h | STAT2 | bit 0 byte ready. |
| 18h | CTRL2 | 01h clears ready (before and after each byte); 03h after a command and at init. |

Interrupt: INT_SPI (bit 6) goes to **FIQ** and fires when the console pulls select low with the link armed. Docking is INT_INPUT bit 11 (the connector's supply pin); every wait loop also bails out if it drops.

## Per-byte exchange (0x04001570)

```
CTRL2 = 1          ; clear ready
DATA  = reply      ; goes out during the console's NEXT byte
MODE  = 1          ; drive DAT
MODE  = 3          ; ACK pulse: acknowledges the byte just received
loop: docked?  STAT1.1 (released)? -> fail
      STAT2.0 (next byte in)?      -> done
CTRL2 = 1; read STAT1
```

The reply for byte N is loaded while acking byte N-1, so the data line is always one byte behind the ack, the same as a Sony card.

The sector-data loop for writes (0x040015ae) is tighter: read DATA, XOR into the checksum, DATA = 0, MODE 1 then 3, then poll STAT1 bits 0/1, with **no CTRL2 write**. The hardware has to clear "ready" when it acks, or the loop runs ahead of the console.

## Last byte (0x040007a8)

The final reply byte is loaded with an ack for the previous byte as usual. The kernel then waits for STAT1 bit 1 (select released) without acking again, so the console sees no ack after the last byte, as the protocol requires.

## Command dispatch (FIQ, 0x04001086)

1. The FIQ fires on select low. The kernel polls up to 30 times for the first byte.
2. If the byte isn't **81h** (a controller poll, for example), it walks away and the port stays silent.
3. If busy (counter at 0xF1 nonzero), it replies FFh, disarms and waits for select release. A console sees no ack and retries next frame.
4. Otherwise it replies with the FLAG byte, acks, and reads the command byte:
   - **52h read / 57h write / 53h ID**: reply 5Ah 5Dh, then the handlers at 0x0400091e / 0x04000856 / 0x0400080e
   - **58h–5Fh**: PocketStation commands, separate handlers starting 0x04000b52 (58h)
5. After the command: MODE=3, CTRL1=0, CTRL2=3 (disarm). The link is re-armed from normal code (MODE=6, DATA=FFh, CTRL1=2).

## Measured in the sim (J110)

| Command | Result |
|---|---|
| 53h ID | `FF 08 5A 5D 5C 5D 04 00 00 80`, identical to a Sony card |
| 52h read | `FF 08 5A 5D 00 00 5C 5D MSB LSB [128] CHK 47`, checksum good |
| 57h write | ends `5C 5D 47`; frame in flash; FLAG goes 08h → 00h afterwards |
| After a write | no answer for **about 45 ms** (2 frames) while the kernel commits flash; the console's normal retry covers it |

## Our hardware block (`rtl/ps_spi.sv`)

Same register map and ports as Jamie's module, so it drops into the core unchanged.

- Console bits: presented on the falling edge of the clock, sampled on the rising edge, LSB first. Inputs pass through 2-flop synchronisers.
- DAT is driven only while MODE bit 0 is set, select is low and the link is armed.
- ACK is one pulse of `ACK_US` (default 3 µs) each time MODE bit 1 goes from 0 to 1. The pulse also clears ready.
- The FIQ request is a level while select is low. `ps_intc` samples its sources only on CPU clock enables, so a one-cycle pulse gets missed.

## Still to confirm on a real unit (Pico SNAC tap)

- ACK pulse width and the latency from the 8th clock to the ack, at each CPU clock setting
- whether reading DATA, rather than the ack, is what clears ready
- 58h–5Fh replies, for building our own firmware against
