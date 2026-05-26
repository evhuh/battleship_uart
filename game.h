// =============================================================================
// game.h — battleship game state and logic (Block 3, networked)
// =============================================================================

#ifndef GAME_H
#define GAME_H

#include <Arduino.h>

// ---- Constants -------------------------------------------------------------
const uint8_t GRID_N    = 10;
const uint8_t NUM_SHIPS = 3;
const uint8_t SHIP_SIZES[NUM_SHIPS] = {4, 3, 2};
const uint8_t TOTAL_SHIP_CELLS = 9;     // 4+3+2

// ---- Cell states -----------------------------------------------------------
enum CellState : uint8_t {
  CELL_EMPTY    = 0,
  CELL_SHIP     = 1,
  CELL_MISS     = 2,
  CELL_HIT      = 3,
  CELL_SHIP_HIT = 4,
};

// ---- Game phases (the FSM) -------------------------------------------------
enum Phase : uint8_t {
  PHASE_BOOT,
  PHASE_LINK_WAIT,
  PHASE_TITLE,          // "Battleship :)" — local A press → INSTRUCTIONS
  PHASE_INSTRUCTIONS,   // wait for both players to press A (BEGIN handshake)
  PHASE_PLACE,
  PHASE_SENDING_READY,
  PHASE_WAIT_OPP,
  PHASE_MY_TURN,
  PHASE_OPP_TURN,
  PHASE_GAME_OVER,
  PHASE_PLAY_AGAIN,     // post-game-over prompt: A=yes, B=back to title
  PHASE_LINK_LOST,
};

enum View : uint8_t {
  VIEW_FLEET,
  VIEW_ATTACK,
};

enum MoveDir : uint8_t {
  MOVE_NONE,
  MOVE_UP, MOVE_DOWN, MOVE_LEFT, MOVE_RIGHT,
};

enum GameEvent : uint8_t {
  EV_NONE,
  EV_PLACED,
  EV_HIT,
  EV_MISS,
  EV_DENY,
  EV_GOT_HIT,
  EV_GOT_MISSED,
  EV_GAME_WON,
  EV_GAME_LOST,
  EV_BEGIN_PRESSED,    // we pressed A on instructions
};

struct GameState {
  Phase     phase;
  View      activeView;
  uint8_t   cursorR;
  uint8_t   cursorC;

  uint8_t   myFleetPacked[50];
  uint8_t   attackBoardPacked[50];

  uint8_t   currentShipIndex;
  bool      ghostHorizontal;

  uint8_t   boardId;
  bool      partnerReady;
  uint8_t   myShipCellsRemaining;
  uint8_t   theirShipCellsHit;

  // Begin / play-again handshakes (same flag pair, reused per phase)
  bool      iPressedBegin;             // I pressed A on instructions or play-again
  bool      partnerPressedBegin;       // partner sent BEGIN

  // Game lifecycle
  uint8_t   gamesPlayed;               // increments each game-over -> reset
};

// Cell accessors
CellState gs_get_my_fleet(const GameState& g, uint8_t r, uint8_t c);
void      gs_set_my_fleet(GameState& g, uint8_t r, uint8_t c, CellState s);
CellState gs_get_attack(const GameState& g, uint8_t r, uint8_t c);
void      gs_set_attack(GameState& g, uint8_t r, uint8_t c, CellState s);

// Init: full reset (gamesPlayed = 0).
void game_init(GameState& g, uint8_t board_id);

// Reset to title screen (preserves and increments gamesPlayed).
// Used by long-hold-B mid-game and B-press at PLAY_AGAIN.
void game_reset_for_new_game(GameState& g);

// Game ended → show "play again?" prompt.
void game_to_play_again(GameState& g);

// Player chose "play again" → fresh round, go to instructions.
void game_to_next_round(GameState& g);

// Cursor / button handlers
void      game_handle_move(GameState& g, MoveDir m);
GameEvent game_handle_button_a(GameState& g);
void      game_handle_button_b(GameState& g);

// Network event handlers
GameEvent game_handle_incoming_fire(GameState& g, uint8_t r, uint8_t c,
                                    uint8_t* outOutcome);
GameEvent game_handle_incoming_result(GameState& g, uint8_t r, uint8_t c,
                                      uint8_t outcome);
void game_handle_ready(GameState& g);
void game_handle_ready_acked(GameState& g);

// Begin handshake: I just pressed A on instructions.
// Caller must also call protocol_send_begin().
void game_note_my_begin(GameState& g);
// Partner just sent BEGIN.
void game_note_partner_begin(GameState& g);
// Returns true if both have pressed for the current handshake.
bool game_both_pressed_begin(const GameState& g);
// Transition out of instructions into PLACE.
void game_start_placing(GameState& g);
// Transition from title to instructions (clears begin handshake flags).
void game_title_to_instructions(GameState& g);
// Reset begin-handshake flags (used when transitioning into INSTRUCTIONS or PLAY_AGAIN).
void game_clear_begin_flags(GameState& g);

// Helpers
bool    game_ghost_fits(const GameState& g, uint8_t r, uint8_t c,
                        uint8_t length, bool horiz);
uint8_t game_current_ship_length(const GameState& g);
bool    game_can_fire_at(const GameState& g, uint8_t r, uint8_t c);

#endif
