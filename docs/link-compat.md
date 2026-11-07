# Infrared link: game by game

Every app in the test folder (110 files), checked with two virtual units face to face, run by the
same code the LAN link uses (`irlink::RunPair`): each unit its own serial number, each a copy of
the card. `tools/ir_explore` finds where each app turns its infrared on (every button, long
presses and two-button chords, breadth first over the app's screens) and then links two units
through it and shows both screens.

The fixes this check turned up (0.1.26, 0.1.27):

- The receive line was upside down: it reads high with no light and low while the far side's
  emitter is on. Chocobo World never decoded a message and its sender waited for good.
- A real receiver module holds its output on a little after the light goes, and the games time
  their bits expecting that (Chocobo World takes 112 us off every lit pulse). Without it the
  Japanese Chocobo game, with its tighter timing, read every '1' as a '0'.

## Pass

| Game | How to link | Result |
|---|---|---|
| Chocobo World (FF8) | Map → Right to Status, Item, then the Vs screen. Receiver: Down (RECEIVE). Sender: Up, Enter (TRANSMIT) | Full handshake, receiver goes into the battle. Also confirmed on a PC and a Pixel 10 over the LAN |
| Chocobo World Enhanced (FF8) | As above | Full handshake (490 pulses each way) |
| Odetake Chocobo RPG (FF8, Japanese Chocobo World) | As above (Fire twice first) | Full handshake, after the receiver fix |
| Metal Gear Solid Integral | Receiver: Up 3x, Fire. Sender: Up 2x, Fire | Receiver shows the received data, sender shows FINISHED. Seen on real devices: a second send of the same unit errors (the game seems to refuse an ID it already has) |
| Pocket Digimon World (Agumon, Biyomon) | Fire, Down 3x, Fire on both | Both exchange (about 315 pulses each way) and show the result |

## Check later

| Game | What happens | Why it isn't settled |
|---|---|---|
| Chocobo Stallion | Fire 3x: a waiting-to-receive screen | No way to make the other unit send found on an empty save (probably needs a raised chocobo) |
| Go Go Digger (SaGa Frontier 2) | Up, Fire: RECEIVE ... WAIT | Same: nothing to send on an empty save |
| Pockeler | Left, Fire, Down, Fire x4: listening | Same |
| Rockman 2, 3, 4, 5 (Complete Works) | The code drives the infrared hardware | Its link mode wasn't reached from an empty save (the Versus mode may need trained Robot Masters) |
| Pocket Fighter Zero 3 / Alpha 3 (parts 1, 2) | The code drives the infrared hardware | Link mode not reached from an empty save |
| Kobun no Tokkun Zanmai, Jet de Go!, PokeHito, Doraemon 3, Brightis, MoGuuRu Dabas | The code drives (or may drive) the infrared hardware | Link mode not reached from an empty save |
| Rockman 3 part 1, BreakThru, PacClone, Sample, Sporting Clays | Don't start on their own (a part of a set, or SN dev-kit files) | Nothing to test |

## Not a unit-to-unit link

- PDA Remote 1 and 2: TV remote controls; they only send.
- Mimique: listens and sends as soon as it starts, with no partner; looks like a remote-control tool (unconfirmed).
- The Pocket MuuMuu games: their code touches the infrared only to switch it off at start.
- Everything else in the folder never turns the infrared on.
