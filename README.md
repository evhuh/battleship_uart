# Muiltiplayer Battleship over UART

**A two-player Battleship game running on two independent Arduino Unos, linked by a single 3-wire serial cable.**

Each board is a self-contained station — OLED display, analog joystick, two buttons, and a buzzer — and the two boards stay perfectly in sync through a custom framed packet protocol with CRC-8 error detection. No game master, no shared clock: both boards run identical state machines and synchronize *only* through the packets they exchange.

This was our final project for **ECE 3481**, built from the chip up: real A/D interfacing, hardware I²C, external interrupts, and an interrupt-driven UART link, all running concurrently on a 16 MHz ATmega328P with 2 KB of RAM.

---

## 🎥 Demo

[![Watch the demo](https://img.youtube.com/vi/XLcXqlhncbc/hqdefault.jpg)](https://www.youtube.com/watch?v=XLcXqlhncbc)

A live two-player match: placing ships by dragging a ghost outline with the joystick, alternating attack turns with a cursor-selected coordinate, and hit/miss feedback over the buzzer — running on two physically separate boards.

---

## Table of Contents

- [Highlights](#highlights)
- [How to Play](#how-to-play)
- [Hardware](#hardware)
  - [Bill of Materials](#bill-of-materials)
  - [Pin Map](#pin-map)
  - [Wiring the Link](#wiring-the-link)
- [Software Architecture](#software-architecture)
- [The Custom Packet Protocol](#the-custom-packet-protocol)
- [The Game State Machine](#the-game-state-machine)
- [The Concurrency Model](#the-concurrency-model)
- [Analog Input (ADC) & I²C Display](#analog-input-adc--ic-display)
- [Build & Flash](#build--flash)
- [Repository Layout](#repository-layout)
- [Engineering Notes](#engineering-notes)
- [Team](#team)

---

## Highlights

- **Custom 7-byte framed packet protocol** with CRC-8 checksums, a byte-at-a-time receiver state machine, and stop-and-wait reliability (per-command timeouts and retries).
- **Two boards, one source of truth, zero shared clock.** Synchronization is achieved entirely through idempotent packet exchanges — both boards independently compute the same game state.
- **True concurrency on bare metal:** an interrupt-driven UART receive buffer accumulates incoming bytes in the background while the foreground loop reads the joystick ADC, drives the I²C display, and runs game logic.
- **Real A/D interfacing:** a 2-axis analog joystick read through the 10-bit ADC with center calibration, a dead-zone, and rate-limited movement.
- **External-interrupt buttons** with debounce and press-duration detection (a 3-second hold triggers a coordinated mid-game reset).
- **A 12-state finite state machine** spanning boot, link handshake, ship placement, networked turns, game-over, and play-again — implemented as a readable `switch`, not a framework.

**Tech:** C++, Arduino (ATmega328P), I²C, UART, ADC, external interrupts, CRC-8, Adafruit SSD1306 / GFX

---

## How to Play

| Control | In placement | In gameplay |
| --- | --- | --- |
| **Joystick** | Move the ship "ghost" around the grid | Move the firing cursor |
| **Button A** (D2) | Place the current ship | Fire at the selected cell |
| **Button B** (D3) | Rotate the ship (horizontal ↔ vertical) | Toggle between your **Fleet** view and your **Attack** view |
| **Hold Button B (3 s)** | — | Reset both boards back to the title screen |

Each player places three ships (lengths **4, 3, 2** → 9 cells total) on a 10×10 grid, then takes turns firing. First to sink all of the opponent's ships wins. After a game ends, both players vote to play again (A) or quit (B); the first player alternates each round.

---

## Hardware

### Bill of Materials

Per board (×2 for a full setup):

| Part | Notes |
| --- | --- |
| Arduino Uno (ATmega328P) | The microcontroller |
| SSD1306 OLED, 128×64 | I²C, address `0x3C` |
| 2-axis analog joystick | Two potentiometer axes → ADC |
| 2× tactile push button | Cursor confirm / rotate / view / reset |
| Piezo buzzer | Feedback tones (`tone()` on a PWM pin) |
| 3× jumper wire | The inter-board link (RX, TX, GND) |


### Pin Map

Single board (both boards are wired identically):

| Uno pin | Connects to | Notes |
| --- | --- | --- |
| **5V** | OLED VCC, Joystick VCC | Most SSD1306 modules accept 5 V via an onboard regulator — *verify your module's silkscreen*; if it says "3.3 V only," use the 3.3 V pin. |
| **GND** | OLED GND, Joystick GND, both buttons (one leg), buzzer (−) | Star-grounded on the rail |
| **A4** | OLED SDA | Hardware I²C data — fixed pin, do not reuse |
| **A5** | OLED SCL | Hardware I²C clock — fixed pin |
| **A0** | Joystick VRx | Analog input |
| **A1** | Joystick VRy | Analog input |
| **D2** | Button A (other leg → GND) | `INPUT_PULLUP`, external interrupt **INT0** |
| **D3** | Button B (other leg → GND) | `INPUT_PULLUP`, external interrupt **INT1** |
| **D4** | Inter-board UART **RX** | `SoftwareSerial` |
| **D5** | Inter-board UART **TX** | `SoftwareSerial` |
| **D6** | Buzzer (+) | PWM, driven by `tone()` |

### Wiring the Link

The two boards are joined by a **3-wire UART cable**, crossed over:

```
       Board 0                       Board 1
   ┌────────────┐                ┌────────────┐
   │ D5 (TX) ───┼────────────────┼──→ D4 (RX) │
   │ D4 (RX) ←──┼────────────────┼─── D5 (TX) │
   │ GND     ───┼────────────────┼─── GND     │
   └────────────┘                └────────────┘
```

TX on one board goes to RX on the other, and the grounds are tied together so both boards share a voltage reference. The link runs at **9600 baud**.

---

## Software Architecture

The firmware is split into four modules so the hardware, the wire protocol, and the game rules can each be reasoned about (and tested) in isolation.

| Module | Responsibility |
| --- | --- |
| `battleship.ino` | The main sketch: pin setup, ISRs, the boot handshake, and the `loop()` that wires everything together. |
| `protocol.{h,cpp}` | The packet framer: CRC-8, packet building, the byte-at-a-time receive parser, and the stop-and-wait send/reply logic. Knows nothing about Battleship. |
| `game.{h,cpp}` | The game state, the FSM transitions, ship placement, hit detection, and turn flipping. Knows nothing about pixels or wires. |
| `display.{h,cpp}` | All OLED rendering — the grid, ghost outline, firing cursor, status panel, and full-screen state messages. |

The application talks to the protocol layer through a set of `onX()` callbacks (`onFire`, `onResult`, `onReady`, …) declared in `protocol.h` and implemented in the sketch. This keeps the design clean: **the FSM treats networking like local function calls**, and the protocol layer handles framing, acking, and retries underneath it.

---

## The Custom Packet Protocol

We didn't want to trust raw UART bytes, so every byte that crosses the cable is part of a framed packet with a checksum.

### Wire format

```
┌──────┬─────┬─────┬───────────┬──────┬──────┐
│ 0xAA │ CMD │ LEN │  PAYLOAD  │ CRC8 │ 0x55 │
│ start│ 1B  │ 1B  │   LEN B   │  1B  │ end  │
└──────┴─────┴─────┴───────────┴──────┴──────┘
```

| Field | Size | Purpose |
| --- | --- | --- |
| **START** | 1 B (`0xAA`) | Sync byte. The `10101010` pattern is distinctive enough that the parser can re-find frame boundaries even after garbage on the line. |
| **CMD** | 1 B | Command ID (see table below). |
| **LEN** | 1 B | Payload length, `0–4`. |
| **PAYLOAD** | `LEN` B | Command-specific data (coordinates, outcomes, IDs). |
| **CRC8** | 1 B | Checksum over `[CMD, LEN, PAYLOAD]`. Polynomial `0x07` (CRC-8/SMBus), init `0x00`. |
| **END** | 1 B (`0x55`) | Belt-and-suspenders sanity byte. |

Fixed overhead is **5 bytes**. A `FIRE` packet (2-byte payload) is 7 bytes total — roughly 7 ms on a 9600-baud link.

### Commands

| ID | Name | Payload | Reply | When it fires |
| --- | --- | --- | --- | --- |
| `0x01` | `HELLO` | `board_id` (1 B) | `ACK` | Boot handshake |
| `0x02` | `ACK` | `cmd_acked` (1 B) | — | Any blocking send waits for this |
| `0x03` | `READY` | (none) | `ACK` | After ships are placed |
| `0x04` | `FIRE` | `row, col` (2 B) | `RESULT` | Player attacks |
| `0x05` | `RESULT` | `row, col, outcome` (3 B) | (none) | Reply to a `FIRE` |
| `0x06` | `RESET` | (none) | `ACK` | Long-hold B, or quit at game-over |
| `0x07` | `NACK` | `reason` (1 B) | — | Reserved (not used in the MVP) |
| `0x08` | `BEGIN` | (none) | `ACK` | "Press A to start" handshake |

`RESULT` outcomes: `MISS (0x00)`, `HIT (0x01)`, `HIT_SUNK (0x02)`, `HIT_GAMEOVER (0x03)`.

### Worked example — firing at row 4, column 2

The exact bytes on the wire:

```
AA 04 02 04 02 D4 55
↑  ↑  ↑  ↑  ↑  ↑  ↑
│  │  │  │  │  │  └─ END
│  │  │  │  │  └──── CRC-8 over [0x04, 0x02, 0x04, 0x02] = 0xD4  ✓ (verified)
│  │  │  │  └─────── col = 2
│  │  │  └────────── row = 4
│  │  └───────────── LEN = 2
│  └──────────────── CMD = FIRE (0x04)
└─────────────────── START
```

The receiver computes the same CRC over the same bytes and compares. Match → valid; mismatch → drop the packet and let the sender's retry handle it.

### The receiver state machine (the "framer")

The parser reads one byte at a time and walks these states:

```
WAIT_START ──(0xAA)──► READ_CMD ──► READ_LEN ──► READ_PAYLOAD ──► READ_CRC ──► READ_END ──► DELIVER
    ▲                                  │              │             │            │
    │                          LEN > 4 │      (LEN bytes)   CRC mismatch    byte ≠ 0x55
    └──────────────────────────────────┴──────────────┴─────────────┴────────────┘
                              frame error → reset, bump error counter
```

Any frame error (oversized `LEN`, bad CRC, wrong end byte) just resets the parser to `WAIT_START` and increments a counter. We don't try to salvage bytes — the sender's retry covers it.

### Reliability: stop-and-wait

Never more than one packet in flight. When the code calls `protocol_send_fire(r, c)` it builds the packet, blocks in a tight loop calling `protocol_poll()`, waits for a matching `RESULT`, and retries on timeout. Each command type has its own timeout and retry budget:

| Command | Reply | Timeout | Retries |
| --- | --- | --- | --- |
| `HELLO` | `ACK` | 2 s | 3 |
| `READY` | `ACK` | 5 s | 3 |
| `BEGIN` | `ACK` | 30 s | 1 *(no retry — the partner is reading the instructions)* |
| `FIRE` | `RESULT` | 3 s | 3 |
| `RESET` | `ACK` | 2 s | 2 |

If every retry fails, the send returns `false` and the board transitions to `PHASE_LINK_LOST`.

### What makes it robust

- **Auto-ACK in the protocol layer.** `HELLO`, `READY`, `BEGIN`, and `RESET` are acknowledged *inside* the protocol module before the application callback even runs. (This fixed a real bug in an earlier version where the app forgot to ACK `READY` and the link timed out.)
- **`RESULT` echoes its `(row, col)`.** The sender matches a reply to the `FIRE` it's currently waiting on by coordinate, so a late or duplicate `RESULT` for an old shot is simply ignored.
- **Duplicate `FIRE`s are idempotent.** If a `RESULT` is lost and the sender retries, the receiver sees the same `FIRE` again, detects that the cell already has an outcome, and re-sends the identical `RESULT` without double-counting the hit.

---

## The Game State Machine

The whole game is a `Phase` enum with hand-written `switch` transitions in `game.cpp` and `battleship.ino` — **12 phases** spanning bootup, networking, gameplay, and game-end. Both boards run the *same* machine and stay synchronized through packet exchanges, not wall-clock time.

```
        ┌──────────┐
        │   BOOT   │  setup()
        └──────────┘
              │
        ┌────────────┐                  ┌──────────────┐
        │ LINK_WAIT  │── link fails ───►│  LINK_LOST   │ (terminal)
        │ HELLO/ACK  │                  └──────────────┘
        └────────────┘
              │ link OK
        ┌────────────┐
        │   TITLE    │  "Press A to start"  (local, no network)
        └────────────┘
              │ press A
        ┌──────────────┐
        │ INSTRUCTIONS │  both players press A  (BEGIN/ACK handshake)
        └──────────────┘
              │ both pressed
        ┌────────────┐
        │   PLACE    │  place 3 ships (4,3,2); A = confirm, B = rotate
        └────────────┘
              │ all placed
        ┌────────────────┐     ACK + partner already READY
        │ SENDING_READY  │──────────────┐
        └────────────────┘              │
              │ ACK, partner not yet     ▼
              ▼                     enterFirstTurn()
        ┌────────────┐                   │
        │  WAIT_OPP  │── partner READY ──┤
        └────────────┘                   │
                                          ▼
        ┌──────────┐   I fire (FIRE→RESULT)   ┌───────────┐
        │ MY_TURN  │ ───────────────────────► │ OPP_TURN  │
        │          │ ◄─────────────────────── │           │
        └──────────┘    partner's FIRE        └───────────┘
              │                                     │
              │ all opponent ships sunk  │ all my ships sunk
              ▼                          ▼
        ┌─────────────────────────────────────┐
        │             GAME_OVER               │  "YOU WON / LOST" (2.5 s linger)
        └─────────────────────────────────────┘
              │
        ┌──────────────┐  both press A → INSTRUCTIONS (gamesPlayed++)
        │  PLAY_AGAIN  │  either presses B → TITLE (RESET sent to partner)
        └──────────────┘
```

**Cross-cutting transitions** can fire from any active phase:

- **Long-hold B (3 s)** → `TITLE` (sends `RESET`; both boards re-init).
- **Partner sends `RESET`** → `TITLE`.
- **Any network call fails after retries** → `LINK_LOST` (terminal).

### Who goes first

After both `READY`s are exchanged, each board independently computes:

```
firstId = (gamesPlayed % 2 == 0) ? 0 : 1
if (my boardId == firstId)  → MY_TURN
else                        → OPP_TURN
```

So game 0: board 0 first; game 1: board 1 first; game 2: board 0 again. The boards don't negotiate this — they compute the same answer because each one locally increments `gamesPlayed` on game-over, keeping the counter deterministic across rounds.

---

## The Concurrency Model

Three things happen "at once" on a single-core MCU, and the design keeps them from stepping on each other:

**1. Background — UART receive (interrupt-driven).**
`SoftwareSerial` uses pin-change interrupts to clock incoming bytes into its internal ring buffer while the rest of the program runs. The foreground never blocks on the wire to receive; it just drains whatever has accumulated.

**2. Background — button presses (external interrupts).**
- `isrA` fires on the **falling edge** of D2 (INT0): debounced (50 ms), sets a `volatile bool aPressed`.
- `isrB` fires on **any change** of D3 (INT1): it reads the pin to tell press from release, timestamps the press, and on release classifies it as a **short press** or a **long press** (≥ 3 s). This is how the same button does "rotate / toggle view" *and* "hold to reset."

All shared ISR state is `volatile`, and the ISRs only set flags — the heavy work happens in the loop.

**3. Foreground — the main loop.**
```
loop():
  protocol_poll()        # drain + parse the UART RX buffer, dispatch callbacks
  handle reset request   # partner asked to reset
  play queued buzzer tone # from the last callback's GameEvent
  phase-specific work    # GAME_OVER linger, READY send, begin/play-again votes
  read joystick (ADC)    # rate-limited, dead-zone filtered
  service button flags   # A press, B short, B long
  if dirty: render OLED  # only redraw when something changed
```

The one place the foreground deliberately *blocks* is a stop-and-wait send (`FIRE`, `READY`, `HELLO`…). Even then it isn't idle: it spins on `protocol_poll()`, so incoming packets and auto-ACKs keep flowing while it waits for its reply. A `g_dirty` flag means the (relatively slow) I²C OLED is only redrawn when state actually changes.

---

## Analog Input (ADC) & I²C Display

**Joystick → ADC.** The two joystick axes are read with `analogRead()` on A0/A1 (the ATmega's 10-bit ADC, `0–1023`). Raw readings are turned into discrete grid moves with three pieces of conditioning:

- **Center calibration** — measured rest points (`≈523`, `≈514`), subtracted so "centered" reads as zero.
- **Dead-zone** — a movement only registers past a threshold of `250`, so a resting thumb doesn't drift the cursor.
- **Rate limiting** — at most one move per `180 ms`, for deliberate, one-cell-at-a-time navigation instead of a runaway cursor.

The axis-to-direction mapping reflects how the joystick is physically mounted (the Y axis drives left/right and the X axis drives up/down), which is worth knowing if you rebuild the rig in a different orientation.

**OLED → I²C.** The SSD1306 talks over hardware I²C on A4 (SDA) / A5 (SCL) at address `0x3C`. We bump the bus to **400 kHz** (`Wire.setClock(400000)`) — fast mode — because the full-screen redraws (10×10 grid + ghost/cursor + status panel) need the bandwidth to feel responsive. Rendering goes through Adafruit's `SSD1306`/`GFX` libraries; the board is stored packed at **4 bits per cell** (50 bytes per grid) to stay friendly to the Uno's 2 KB of RAM.

---

## Build & Flash

**Requirements**

- Arduino IDE (or `arduino-cli`)
- Board: **Arduino Uno**
- Libraries: `Adafruit GFX Library`, `Adafruit SSD1306` (install via Library Manager). `Wire` and `SoftwareSerial` ship with the IDE.

**Steps**

1. Place all source files (`battleship.ino`, `protocol.*`, `game.*`, `display.*`) in a folder named `battleship/` so the IDE recognizes the sketch.
2. **Set the board ID.** Near the top of `battleship.ino`:
   ```cpp
   #define BOARD_ID 0   // Board 0  — initiates the HELLO handshake
   // #define BOARD_ID 1   // Board 1 — waits for HELLO
   ```
   Flash **one board as `0` and the other as `1`.** They are *not* interchangeable — board 0 initiates the link handshake and board 1 waits for it, and the IDs decide turn order.
3. Compile and upload to each Uno.
4. Power both boards, connect the 3-wire link, and you should see `Linking…` resolve to the `Battleship :)` title screen on both.

> Set `#define DEBUG 1` to get boot logs and a free-RAM report over the USB serial monitor (9600 baud).

---

## Repository Layout

```
battleship/
├── battleship.ino          # main sketch: setup(), loop(), ISRs, boot handshake
├── protocol.h / .cpp       # packet framer: CRC-8, builder, parser FSM, stop-and-wait
├── game.h / .cpp           # game state, the phase FSM, placement, hit logic
├── display.h / .cpp        # OLED rendering (grid, ghost, cursor, status, screens)
└── docs/
    ├── pinmap.pdf              # single-board pin assignments
    ├── project_proposal.pdf    # original ECE 3481 proposal
    └── protocol_and_fsm.pdf    # protocol + state-machine reference sheet
```

---

## Engineering Notes

A few decisions worth calling out:

- **Switch-based FSM, not a table.** For ~12 states with hand-written transitions, a plain `switch` is more readable, easier to step through in a debugger, and has zero abstraction overhead. We didn't reach for a framework when straight-line code does the job in fewer lines.
- **No separate "waiting for result" phase.** `protocol_send_fire()` already blocks (≤ 3 s) polling for the `RESULT`, so `MY_TURN` transitions straight to `OPP_TURN` — the display flashes "firing" too briefly to matter.
- **Stale-input guard.** When both players press A to leave the instructions screen, a finger still on the button could let a queued press leak into `PLACE` and auto-place a ship. `clearInputFlags()` is called on every transition into an input-taking state to prevent that.
- **Synchronization without a shared clock.** The core trick: never *assume* the two boards are in step. Every state transition is made idempotent, and the packet exchanges themselves act as the synchronization points.
- **Tested the framer in isolation** — the parser was unit-tested on one board (including corrupted CRCs and back-to-back packets) before either board ever drove the wire.

---

## Team

**Eva & Maggie's ECE 3481 Final Project**

*Built because the best games are the ones that put two people in the same room.*
