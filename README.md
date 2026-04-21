# IDUN Doom

Doom for the [IDUN cartridge](https://github.com/idun-project/idun-cartridge), ported from [RAD Doom](https://github.com/frntc/RAD-Doom).

Runs on a **Raspberry Pi Zero 2 W** (Arch Linux ARM) inside the IDUN cartridge, paired with a **Commodore 64** (tested on Ultimate64 Elite II).  The Doom engine executes entirely on the Pi; the C64 side receives compressed frame updates and renders them in multicolor bitmap mode at up to ~30 fps.

---

## How it works

```
┌─────────────────────────────────────────────────────────────┐
│  Raspberry Pi Zero 2 W  (Arch Linux ARM)                    │
│                                                             │
│  doom-idun binary                                           │
│   • full Doom engine (from RAD Doom / doomgeneric)         │
│   • doomgeneric_idun.c:                                     │
│       – converts 320×200 RGB → C64 multicolor bitmap       │
│       – dirty-block delta encoding (only changed cells sent)│
│       – writes frame packets to /tmp/idun_doom_out pipe     │
│       – reads joystick/key input from /tmp/idun_doom_in     │
│                                                             │
│  main.lua  (IDUN Lua device, started by C64 app)           │
│   • creates the named pipes                                 │
│   • launches doom-idun                                      │
│   • relays  pipe ↔ minisock (IDUN TTY ↔ $DE00 on C64)     │
└─────────────────┬───────────────────────────────────────────┘
                  │  IDUN Propeller bridge  ($DE00/$DE01)
┌─────────────────▼───────────────────────────────────────────┐
│  Commodore 64  (Ultimate64 Elite II)                        │
│                                                             │
│  doom.app  (6502 assembly, IDUN app at $6000)              │
│   • opens l:.d/main.lua  → starts Pi-side Lua/doom-idun    │
│   • sets VIC-II: multicolor bitmap, bank 2 ($8000–$BFFF)   │
│   • receives frame packets from $DE00                       │
│   • blits dirty 4×8 blocks to screen/bitmap/color RAM      │
│   • sends joystick state to Pi after each frame            │
└─────────────────────────────────────────────────────────────┘
```

### Memory layout (C64 side)

| Region | Address | Purpose |
|---|---|---|
| App code | `$6000–$7FFF` | doom.app (IDUN app) |
| Screen RAM | `$8000–$83E7` | VIC-II character matrix |
| Bitmap | `$A000–$BF3F` | VIC-II multicolor bitmap |
| Color RAM | `$D800–$DBE7` | always at this address |

### Protocol

**Pi → C64** (frame packet):
```
$01                  frame-start marker
[count_lo count_hi]  number of dirty blocks (little-endian u16)
per block x count:
  [idx_lo idx_hi]    block index 0-999  (row*40 + col)
  [bm0..bm7]         8 bitmap bytes (one per pixel row)
  [screen]           screen RAM byte  (hi-nybble=color1, lo=color2)
  [color]            color  RAM byte  (lo-nybble=color3)
[audio_count]        audio samples following (v1: always 0)
```

**C64 -> Pi** (input packet):
```
$10                  input marker
[joy]                joystick port 2 byte (bits 0-4, active-high after invert)
[keys]               keyboard flags (bit0=Escape, bit1=Enter)
```

---

## Requirements

- IDUN cartridge with **Raspberry Pi Zero 2 W**, running the IDUN Arch Linux image
- **Doom 1 WAD** (`doom1.wad` shareware or full) placed at `~/doom1.wad` on the Pi
- C64 or compatible (Ultimate64 recommended) with the IDUN cartridge installed

---

## Installation

On the Pi (SSH in or use the IDUN web browser):

```bash
git clone https://github.com/sidchoudhuri/idun_doom.git
cd idun_doom
bash setup.sh
```

`setup.sh` installs build deps via `pacman`, clones the RAD Doom engine source, compiles `doom-idun`, and deploys the C64 app into your IDUN cartridge directory.

Then on the C64 / IDUN shell:

```
go doom
```

---

## Controls

| Input | Action |
|---|---|
| Joystick port 2 up | Move forward |
| Joystick port 2 down | Move backward |
| Joystick port 2 left | Turn left |
| Joystick port 2 right | Turn right |
| Joystick fire | Shoot |
| RUN/STOP | Escape / menu |

---

## Performance notes

- **~15-30 fps** depending on how much of the screen changes per frame
- Action scenes (lots of motion): ~15 fps typical
- Static scenes (menus, corridors): ~25-30 fps
- Game logic always runs at Doom's native 35 Hz; only the display rate varies
- Audio: v1 ships without SID audio (planned for v2)

---

## Architecture notes

RAD Doom runs **bare-metal** on the Pi using the Circle C++ framework and replaces
the C64 CPU entirely (Ultimax mode).  IDUN Doom is architecturally different:

- The C64 CPU still runs normally; IDUN is an expansion cartridge, not a CPU replacement
- The Pi runs full Arch Linux; no bare-metal or Circle dependency
- Communication is via the IDUN Propeller bridge (`$DE00`/`$DE01`) rather than direct DMA
- Only the Doom engine `.c` files from RAD Doom are reused; all platform glue is new

---

## License

GPLv3 - see [LICENSE](LICENSE).
Doom engine source (C) id Software / Chocolate Doom contributors.
RAD Doom color conversion algorithms (C) Carsten Dachsbacher (GPLv3).
