// =============================================================================
// Battleship — Block 3 v4: networked + instructions + reset
// =============================================================================
// What's new vs v3:
//   - Instructions screen + "Press A to begin" handshake (CMD_BEGIN)
//   - Long-press B (3s) triggers in-game RESET; both boards re-init
//   - Alternating first player across games
//   - Stronger debounce on buttons; slower joystick rate
//   - Status panel: Mine: x/9, Hit: x/9
//
// === BUILD ==================================================================
//   Eva:    BOARD_ID = 0
//   Maggie: BOARD_ID = 1
// =============================================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SoftwareSerial.h>

#include "display.h"
#include "game.h"
#include "protocol.h"

// ---- [BUILD CHANGE] -------------------------------------------------------
#define BOARD_ID 1
// ---------------------------------------------------------------------------

#define DEBUG 0

// ---- OLED ------------------------------------------------------------------
#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, OLED_RESET);

// ---- UART link -------------------------------------------------------------
SoftwareSerial link(4, 5);

// ---- Pins ------------------------------------------------------------------
const uint8_t PIN_JOY_X = A0;
const uint8_t PIN_JOY_Y = A1;
const uint8_t PIN_BTN_A = 2;
const uint8_t PIN_BTN_B = 3;
const uint8_t PIN_BUZZER = 6;

// ---- Joystick calibration --------------------------------------------------
const int JOY_CX = 523;
const int JOY_CY = 514;
const int JOY_THRESH = 250;
const uint32_t MOVE_MS = 180;       // slower for more deliberate movement

// ---- Button debounce + long-press -----------------------------------------
const uint32_t DEBOUNCE_MS = 50;     // stronger
const uint32_t LONG_PRESS_MS = 3000; // 3-second hold = reset

int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

// ---- Button ISR state ------------------------------------------------------
volatile bool aPressed = false;
volatile uint32_t lastA = 0;

// Button B: needs press + release tracking for long-press detection.
// The ISR records the press timestamp on press; on release, sets one of two
// flags (bShortPressed / bLongPressed) based on duration.
volatile bool bShortPressed = false;
volatile bool bLongPressed  = false;
volatile uint32_t bDownTime = 0;     // millis() when B was last pressed
volatile bool bIsDown = false;

void isrA() {
  uint32_t t = millis();
  if (t - lastA > DEBOUNCE_MS) { aPressed = true; lastA = t; }
}

void isrB() {
  uint32_t t = millis();
  // CHANGE interrupt — read pin to figure out which edge.
  // PIN_BTN_B is D3, with INPUT_PULLUP: LOW = pressed, HIGH = released.
  bool pressed = (digitalRead(PIN_BTN_B) == LOW);
  if (pressed) {
    if (!bIsDown) {
      // Falling edge: button just went down.
      bIsDown = true;
      bDownTime = t;
    }
  } else {
    if (bIsDown) {
      // Rising edge: button just released.
      bIsDown = false;
      // Apply debounce: if up too quickly after down, ignore as bounce.
      if (t - bDownTime < DEBOUNCE_MS) return;
      uint32_t held = t - bDownTime;
      if (held >= LONG_PRESS_MS) {
        bLongPressed = true;
      } else {
        bShortPressed = true;
      }
    }
  }
}

// ---- Joystick polling ------------------------------------------------------
MoveDir readJoystick() {
  static uint32_t lastMove = 0;
  if (millis() - lastMove < MOVE_MS) return MOVE_NONE;
  int x = analogRead(PIN_JOY_X) - JOY_CX;
  int y = analogRead(PIN_JOY_Y) - JOY_CY;
  if (abs(y) > abs(x)) {
    if (y >  JOY_THRESH) { lastMove = millis(); return MOVE_RIGHT; }
    if (y < -JOY_THRESH) { lastMove = millis(); return MOVE_LEFT; }
  } else {
    if (x >  JOY_THRESH) { lastMove = millis(); return MOVE_DOWN; }
    if (x < -JOY_THRESH) { lastMove = millis(); return MOVE_UP; }
  }
  return MOVE_NONE;
}

// ---- Audio -----------------------------------------------------------------
void playHit()       { tone(PIN_BUZZER, 1760, 200); }
void playMiss()      { tone(PIN_BUZZER, 220,  200); }
void playDeny()      { tone(PIN_BUZZER, 150,  80);  }
void playPlaced()    { tone(PIN_BUZZER, 880,  80);  }
void playToggle()    { tone(PIN_BUZZER, 440,  40);  }
void playGotHit()    { tone(PIN_BUZZER,  90,  300); }
void playGotMissed() { tone(PIN_BUZZER, 330,   80); }
void playWin()       { tone(PIN_BUZZER, 2100, 600); }
void playLose()      { tone(PIN_BUZZER,  60,  600); }
void playReset()     { tone(PIN_BUZZER, 660,  400); }
void playBegin()     { tone(PIN_BUZZER, 1320, 100); }

void playForEvent(GameEvent ev) {
  switch (ev) {
    case EV_PLACED:        playPlaced();    break;
    case EV_HIT:           playHit();       break;
    case EV_MISS:          playMiss();      break;
    case EV_DENY:          playDeny();      break;
    case EV_GOT_HIT:       playGotHit();    break;
    case EV_GOT_MISSED:    playGotMissed(); break;
    case EV_GAME_WON:      playWin();       break;
    case EV_GAME_LOST:     playLose();      break;
    case EV_BEGIN_PRESSED: playBegin();     break;
    default: break;
  }
}

// ---- Game state ------------------------------------------------------------
GameState game;
static bool g_dirty = false;
static GameEvent g_pendingEv = EV_NONE;
static volatile bool g_helloSeen = false;
static volatile bool g_resetRequested = false;   // partner asked for reset

// ===========================================================================
// Protocol callbacks
// ===========================================================================
// All four of these (HELLO, READY, BEGIN, RESET) are auto-ACKed by the
// protocol module — we don't need to send ACKs ourselves.
void onHello(uint8_t their_board_id) {
  (void)their_board_id;
  g_helloSeen = true;
}

void onAck(uint8_t cmd_acked) {
  (void)cmd_acked;
}

void onReady() {
  game_handle_ready(game);
  protocol_send_ack(CMD_READY);
  g_dirty = true;
}

void onBegin() {
  game_note_partner_begin(game);
  g_dirty = true;
}

void onFire(uint8_t r, uint8_t c) {
  uint8_t outcome = 0;
  GameEvent ev = game_handle_incoming_fire(game, r, c, &outcome);
  protocol_send_result(r, c, outcome);
  g_pendingEv = ev;
  g_dirty = true;
}

void onResult(uint8_t, uint8_t, uint8_t) {}

void onReset() {
  g_resetRequested = true;
}

void onNack(uint8_t) {}
void onFrameError() {}

// ===========================================================================
// Setup
// ===========================================================================
void setup() {
  Serial.begin(9600);
  delay(1500);
#if DEBUG
  Serial.println(F("=== Battleship Block 3 v4 ==="));
  Serial.print(F("BOARD_ID = ")); Serial.println(BOARD_ID);
  Serial.print(F("Free RAM at boot: ")); Serial.println(freeRam());
#endif

  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_A), isrA, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_B), isrB, CHANGE);
  pinMode(PIN_BUZZER, OUTPUT);
  aPressed = false;
  bShortPressed = false;
  bLongPressed = false;
  bIsDown = false;

  Wire.begin();
  delay(100);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
#if DEBUG
    Serial.println(F("OLED init FAILED"));
#endif
    while (1) {}
  }
  Wire.setClock(400000);

  link.begin(9600);
  protocol_init(link, BOARD_ID);

  game_init(game, BOARD_ID);
  display_init(oled);

  // ---- Handshake (HELLO/ACK) ---------------------------------------------
  game.phase = PHASE_LINK_WAIT;
  display_render(oled, game);
  delay(2000);

  bool linkOk;
  if (BOARD_ID == 0) {
    linkOk = protocol_send_hello();
  } else {
    uint32_t deadline = millis() + 10000;
    linkOk = false;
    while (millis() < deadline) {
      protocol_poll();
      if (g_helloSeen) { linkOk = true; break; }
    }
  }

  if (!linkOk) {
    game.phase = PHASE_LINK_LOST;
    display_render(oled, game);
    return;
  }

  // ---- Enter TITLE -------------------------------------------------------
  game.phase = PHASE_TITLE;
  display_render(oled, game);

#if DEBUG
  Serial.print(F("Free RAM after setup: ")); Serial.println(freeRam());
#endif
}

// ===========================================================================
// Input flag helpers
// ===========================================================================
// Clear pending button presses. Called whenever the FSM transitions into a
// state that takes input — prevents the previous state's stale press from
// being processed by the new state.
//
// Bug it fixes: when both players press A on instructions, both boards
// transition to PLACE. If a player's finger is still on the button when the
// transition happens, the ISR may have queued another aPressed=true that
// gets processed in PLACE → ship auto-placed.
static void clearInputFlags() {
  aPressed = false;
  bShortPressed = false;
  bLongPressed = false;
  // Reset the joystick rate limiter too, by setting a fresh "last move" time.
  // (Not strictly needed but avoids weird carry-over behavior.)
}

// ===========================================================================
// Reset handler — called when partner requested reset OR we triggered it
// ===========================================================================
static void doFullReset() {
  game_reset_for_new_game(game);
  g_pendingEv = EV_NONE;
  clearInputFlags();
  g_dirty = true;
}

// Used at end of each game: GAME_OVER → linger briefly → PLAY_AGAIN.
// Tracks when GAME_OVER was entered so we can do the delay non-blocking.
static uint32_t g_gameOverEnteredAt = 0;
static bool     g_gameOverNoticed = false;
const uint32_t  GAME_OVER_LINGER_MS = 2500;

// ===========================================================================
// Main loop
// ===========================================================================
void loop() {
  protocol_poll();

  // ---- Handle incoming reset -----------------------------------------------
  if (g_resetRequested) {
    g_resetRequested = false;
    playReset();
    doFullReset();
  }

  // ---- Pending event from a callback --------------------------------------
  if (g_pendingEv != EV_NONE) {
    playForEvent(g_pendingEv);
    g_pendingEv = EV_NONE;
  }

  // ---- LINK_LOST is a dead end --------------------------------------------
  if (game.phase == PHASE_LINK_LOST) {
    if (g_dirty) { display_render(oled, game); g_dirty = false; }
    return;
  }

  // ---- GAME_OVER → linger briefly → PLAY_AGAIN ----------------------------
  if (game.phase == PHASE_GAME_OVER) {
    if (!g_gameOverNoticed) {
      g_gameOverEnteredAt = millis();
      g_gameOverNoticed = true;
    }
    if (millis() - g_gameOverEnteredAt > GAME_OVER_LINGER_MS) {
      game_to_play_again(game);
      clearInputFlags();
      g_gameOverNoticed = false;
      g_dirty = true;
    }
  } else {
    g_gameOverNoticed = false;
  }

  // ---- INSTRUCTIONS phase: check for both-pressed → start placing ---------
  if (game.phase == PHASE_INSTRUCTIONS && game_both_pressed_begin(game)) {
    display_render(oled, game);  // shows "Starting!"
    delay(400);
    game_start_placing(game);
    clearInputFlags();
    g_dirty = true;
  }

  // ---- PLAY_AGAIN: if both A-pressed → start next round -------------------
  if (game.phase == PHASE_PLAY_AGAIN && game_both_pressed_begin(game)) {
    display_render(oled, game);  // shows "Next round!"
    delay(400);
    game_to_next_round(game);
    clearInputFlags();
    g_dirty = true;
  }

  // ---- SENDING_READY -------------------------------------------------------
  if (game.phase == PHASE_SENDING_READY) {
    display_render(oled, game);
    bool ok = protocol_send_ready();
    if (ok) {
      game_handle_ready_acked(game);
    } else {
      game.phase = PHASE_LINK_LOST;
    }
    clearInputFlags();
    g_dirty = true;
  }

  // ---- Joystick ------------------------------------------------------------
  MoveDir m = readJoystick();
  if (m != MOVE_NONE) {
    game_handle_move(game, m);
    g_dirty = true;
  }

  // ---- Long-press B = RESET to title --------------------------------------
  // Only allowed mid-game. Title/instructions/play-again use B for other
  // things (or have it be a no-op).
  if (bLongPressed) {
    bLongPressed = false;
    bShortPressed = false;
    bool inGame = (game.phase == PHASE_PLACE ||
                   game.phase == PHASE_SENDING_READY ||
                   game.phase == PHASE_WAIT_OPP ||
                   game.phase == PHASE_MY_TURN ||
                   game.phase == PHASE_OPP_TURN ||
                   game.phase == PHASE_GAME_OVER);
    if (inGame) {
      bool ok = protocol_send_reset();
      playReset();
      doFullReset();
      (void)ok;  // we reset locally regardless of partner ACK
    }
  }

  // ---- Button A ------------------------------------------------------------
  if (aPressed) {
    aPressed = false;

    if (game.phase == PHASE_TITLE) {
      // Title screen: A advances to instructions. Local-only, no networking.
      game_title_to_instructions(game);
      playForEvent(EV_BEGIN_PRESSED);
      clearInputFlags();
      g_dirty = true;
    }
    else if (game.phase == PHASE_INSTRUCTIONS) {
      if (!game.iPressedBegin) {
        game_note_my_begin(game);
        playForEvent(EV_BEGIN_PRESSED);
        g_dirty = true;
        bool ok = protocol_send_begin();
        if (!ok) {
          game.phase = PHASE_LINK_LOST;
        }
      }
    }
    else if (game.phase == PHASE_PLACE) {
      GameEvent ev = game_handle_button_a(game);
      playForEvent(ev);
      g_dirty = true;
    }
    else if (game.phase == PHASE_MY_TURN) {
      if (game.activeView != VIEW_ATTACK) {
        playDeny();
      } else {
        uint8_t r = game.cursorR, c = game.cursorC;
        if (!game_can_fire_at(game, r, c)) {
          playDeny();
        } else {
          display_render(oled, game);
          uint8_t outcome = 0;
          bool ok = protocol_send_fire(r, c, &outcome);
          if (!ok) {
            game.phase = PHASE_LINK_LOST;
          } else {
            GameEvent ev = game_handle_incoming_result(game, r, c, outcome);
            playForEvent(ev);
          }
          g_dirty = true;
        }
      }
    }
    else if (game.phase == PHASE_PLAY_AGAIN) {
      // A = vote "yes, play again." Same handshake as instructions BEGIN.
      if (!game.iPressedBegin) {
        game_note_my_begin(game);
        playForEvent(EV_BEGIN_PRESSED);
        g_dirty = true;
        bool ok = protocol_send_begin();
        if (!ok) {
          game.phase = PHASE_LINK_LOST;
        }
      }
    }
  }

  // ---- Button B (short press) ----------------------------------------------
  if (bShortPressed) {
    bShortPressed = false;
    if (game.phase == PHASE_PLAY_AGAIN) {
      // B at game over = quit to title. Send RESET; both go to title.
      bool ok = protocol_send_reset();
      playReset();
      doFullReset();
      (void)ok;
    }
    else if (game.phase != PHASE_INSTRUCTIONS && game.phase != PHASE_TITLE) {
      // Normal B: rotate during PLACE, view toggle elsewhere.
      game_handle_button_b(game);
      playToggle();
      g_dirty = true;
    }
  }

  if (g_dirty) {
    display_render(oled, game);
    g_dirty = false;
  }
}
