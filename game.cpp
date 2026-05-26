// =============================================================================
// game.cpp — battleship game logic (Block 3, networked)
// =============================================================================

#include "game.h"
#include "protocol.h"

// ---- Packed cell storage --------------------------------------------------
static inline uint8_t pack_idx(uint8_t r, uint8_t c) { return r * 10 + c; }

static CellState pack_get(const uint8_t* buf, uint8_t r, uint8_t c) {
  uint8_t idx = pack_idx(r, c);
  uint8_t byte = buf[idx >> 1];
  return (CellState)((idx & 1) ? (byte >> 4) : (byte & 0x0F));
}

static void pack_set(uint8_t* buf, uint8_t r, uint8_t c, CellState s) {
  uint8_t idx = pack_idx(r, c);
  uint8_t off = idx >> 1;
  if (idx & 1) buf[off] = (buf[off] & 0x0F) | ((uint8_t)s << 4);
  else         buf[off] = (buf[off] & 0xF0) | ((uint8_t)s & 0x0F);
}

CellState gs_get_my_fleet(const GameState& g, uint8_t r, uint8_t c) {
  return pack_get(g.myFleetPacked, r, c);
}
void gs_set_my_fleet(GameState& g, uint8_t r, uint8_t c, CellState s) {
  pack_set(g.myFleetPacked, r, c, s);
}
CellState gs_get_attack(const GameState& g, uint8_t r, uint8_t c) {
  return pack_get(g.attackBoardPacked, r, c);
}
void gs_set_attack(GameState& g, uint8_t r, uint8_t c, CellState s) {
  pack_set(g.attackBoardPacked, r, c, s);
}

// ===========================================================================
// Init / Reset
// ===========================================================================

// Internal: reset all per-game state. Does NOT touch boardId or gamesPlayed.
static void resetPerGameState(GameState& g) {
  g.activeView = VIEW_FLEET;
  g.cursorR = 0;
  g.cursorC = 0;
  for (uint8_t i = 0; i < 50; i++) {
    g.myFleetPacked[i]     = 0;
    g.attackBoardPacked[i] = 0;
  }
  g.currentShipIndex     = 0;
  g.ghostHorizontal      = true;
  g.partnerReady         = false;
  g.myShipCellsRemaining = 0;
  g.theirShipCellsHit    = 0;
  g.iPressedBegin        = false;
  g.partnerPressedBegin  = false;
}

void game_init(GameState& g, uint8_t board_id) {
  g.phase       = PHASE_BOOT;
  g.boardId     = board_id;
  g.gamesPlayed = 0;
  resetPerGameState(g);
}

// Called after a game ends + reset signal exchanged. Bumps gamesPlayed and
// brings us back to the title screen.
void game_reset_for_new_game(GameState& g) {
  g.gamesPlayed++;
  resetPerGameState(g);
  g.phase = PHASE_TITLE;
}

// Move from PHASE_GAME_OVER into PHASE_PLAY_AGAIN.
// Does NOT clear begin flags — partner may have already pressed A while we
// were lingering on GAME_OVER, and we must preserve their press.
void game_to_play_again(GameState& g) {
  g.phase = PHASE_PLAY_AGAIN;
}

// Player chose "play again" on PLAY_AGAIN screen → bump games counter,
// reset per-game state, return to instructions for a fresh round.
void game_to_next_round(GameState& g) {
  g.gamesPlayed++;
  resetPerGameState(g);
  g.phase = PHASE_INSTRUCTIONS;
}

// ===========================================================================
// Cursor movement
// ===========================================================================
void game_handle_move(GameState& g, MoveDir m) {
  if (g.phase != PHASE_PLACE && g.phase != PHASE_MY_TURN &&
      g.phase != PHASE_OPP_TURN && g.phase != PHASE_GAME_OVER) {
    return;
  }
  switch (m) {
    case MOVE_UP:    if (g.cursorR > 0)          g.cursorR--; break;
    case MOVE_DOWN:  if (g.cursorR < GRID_N - 1) g.cursorR++; break;
    case MOVE_LEFT:  if (g.cursorC > 0)          g.cursorC--; break;
    case MOVE_RIGHT: if (g.cursorC < GRID_N - 1) g.cursorC++; break;
    default: break;
  }
}

// ===========================================================================
// Ship-fit + commit
// ===========================================================================
uint8_t game_current_ship_length(const GameState& g) {
  if (g.currentShipIndex >= NUM_SHIPS) return 0;
  return SHIP_SIZES[g.currentShipIndex];
}

bool game_ghost_fits(const GameState& g, uint8_t r, uint8_t c,
                     uint8_t length, bool horiz) {
  if (horiz) {
    if ((uint16_t)c + length > GRID_N) return false;
    for (uint8_t k = 0; k < length; k++)
      if (gs_get_my_fleet(g, r, c + k) != CELL_EMPTY) return false;
  } else {
    if ((uint16_t)r + length > GRID_N) return false;
    for (uint8_t k = 0; k < length; k++)
      if (gs_get_my_fleet(g, r + k, c) != CELL_EMPTY) return false;
  }
  return true;
}

static void commitShip(GameState& g, uint8_t r, uint8_t c,
                       uint8_t length, bool horiz) {
  for (uint8_t k = 0; k < length; k++) {
    if (horiz) gs_set_my_fleet(g, r, c + k, CELL_SHIP);
    else       gs_set_my_fleet(g, r + k, c, CELL_SHIP);
  }
  g.myShipCellsRemaining += length;
}

bool game_can_fire_at(const GameState& g, uint8_t r, uint8_t c) {
  if (g.phase != PHASE_MY_TURN) return false;
  return gs_get_attack(g, r, c) == CELL_EMPTY;
}

// ===========================================================================
// Button A / B
// ===========================================================================
GameEvent game_handle_button_a(GameState& g) {
  if (g.phase == PHASE_PLACE) {
    uint8_t len = game_current_ship_length(g);
    if (!game_ghost_fits(g, g.cursorR, g.cursorC, len, g.ghostHorizontal))
      return EV_DENY;
    commitShip(g, g.cursorR, g.cursorC, len, g.ghostHorizontal);
    g.currentShipIndex++;
    if (g.currentShipIndex >= NUM_SHIPS) {
      g.phase = PHASE_SENDING_READY;
      g.activeView = VIEW_FLEET;
      g.cursorR = 0;
      g.cursorC = 0;
    }
    return EV_PLACED;
  }
  // MY_TURN: actual fire is handled in sketch (needs network).
  return EV_NONE;
}

void game_handle_button_b(GameState& g) {
  if (g.phase == PHASE_PLACE) {
    g.ghostHorizontal = !g.ghostHorizontal;
    uint8_t len = game_current_ship_length(g);
    if (g.ghostHorizontal) {
      if ((uint16_t)g.cursorC + len > GRID_N) g.cursorC = GRID_N - len;
    } else {
      if ((uint16_t)g.cursorR + len > GRID_N) g.cursorR = GRID_N - len;
    }
    return;
  }
  if (g.phase == PHASE_MY_TURN || g.phase == PHASE_OPP_TURN ||
      g.phase == PHASE_GAME_OVER) {
    g.activeView = (g.activeView == VIEW_FLEET) ? VIEW_ATTACK : VIEW_FLEET;
  }
}

// ===========================================================================
// First-turn assignment (alternating across games)
// ===========================================================================
// Game 0: lower BOARD_ID goes first.
// Game 1: higher BOARD_ID goes first.
// Game 2: lower again, etc.
//
// Both boards compute the same answer because both see the same gamesPlayed
// counter (each board increments locally on game-over).
static bool iGoFirst(const GameState& g) {
  uint8_t firstId = (g.gamesPlayed & 1) ? 1 : 0;
  return g.boardId == firstId;
}

static void enterFirstTurn(GameState& g) {
  if (iGoFirst(g)) {
    g.phase = PHASE_MY_TURN;
    g.activeView = VIEW_ATTACK;
    g.cursorR = 0;
    g.cursorC = 0;
  } else {
    g.phase = PHASE_OPP_TURN;
    g.activeView = VIEW_FLEET;
  }
}

// ===========================================================================
// Network event handlers
// ===========================================================================
void game_handle_ready_acked(GameState& g) {
  if (g.phase != PHASE_SENDING_READY) return;
  if (g.partnerReady) {
    enterFirstTurn(g);
  } else {
    g.phase = PHASE_WAIT_OPP;
    g.activeView = VIEW_FLEET;
  }
}

void game_handle_ready(GameState& g) {
  g.partnerReady = true;
  if (g.phase == PHASE_WAIT_OPP) enterFirstTurn(g);
  // Otherwise the flag persists until our own READY's ACK lands.
}

GameEvent game_handle_incoming_fire(GameState& g, uint8_t r, uint8_t c,
                                    uint8_t* outOutcome) {
  if (r >= GRID_N || c >= GRID_N) {
    if (outOutcome) *outOutcome = OUTCOME_MISS;
    return EV_GOT_MISSED;
  }

  CellState here = gs_get_my_fleet(g, r, c);

  if (here == CELL_SHIP) {
    gs_set_my_fleet(g, r, c, CELL_SHIP_HIT);
    if (g.myShipCellsRemaining > 0) g.myShipCellsRemaining--;
    bool gameOver = (g.myShipCellsRemaining == 0);
    if (outOutcome) *outOutcome = gameOver ? OUTCOME_HIT_GAMEOVER : OUTCOME_HIT;
    if (gameOver) {
      g.phase = PHASE_GAME_OVER;
      g.activeView = VIEW_FLEET;
      game_clear_begin_flags(g);    // ready for play-again handshake
      return EV_GAME_LOST;
    }
    g.phase = PHASE_MY_TURN;
    g.activeView = VIEW_ATTACK;
    return EV_GOT_HIT;
  }

  if (here == CELL_EMPTY) gs_set_my_fleet(g, r, c, CELL_MISS);
  if (outOutcome) *outOutcome = OUTCOME_MISS;
  g.phase = PHASE_MY_TURN;
  g.activeView = VIEW_ATTACK;
  return EV_GOT_MISSED;
}

GameEvent game_handle_incoming_result(GameState& g, uint8_t r, uint8_t c,
                                      uint8_t outcome) {
  if (r >= GRID_N || c >= GRID_N) return EV_NONE;

  if (outcome == OUTCOME_MISS) {
    gs_set_attack(g, r, c, CELL_MISS);
    g.phase = PHASE_OPP_TURN;
    g.activeView = VIEW_FLEET;
    return EV_MISS;
  }

  gs_set_attack(g, r, c, CELL_HIT);
  g.theirShipCellsHit++;

  if (outcome == OUTCOME_HIT_GAMEOVER) {
    g.phase = PHASE_GAME_OVER;
    g.activeView = VIEW_ATTACK;
    game_clear_begin_flags(g);    // ready for play-again handshake
    return EV_GAME_WON;
  }

  g.phase = PHASE_OPP_TURN;
  g.activeView = VIEW_FLEET;
  return EV_HIT;
}

// ===========================================================================
// Begin / play-again handshake
// ===========================================================================
// Same flag pair (iPressedBegin / partnerPressedBegin) is reused across both
// the instructions screen and the post-game-over "play again" prompt.
// game_clear_begin_flags() is called when entering either of those phases.

void game_clear_begin_flags(GameState& g) {
  g.iPressedBegin       = false;
  g.partnerPressedBegin = false;
}

void game_note_my_begin(GameState& g) {
  g.iPressedBegin = true;
}

void game_note_partner_begin(GameState& g) {
  g.partnerPressedBegin = true;
}

bool game_both_pressed_begin(const GameState& g) {
  return g.iPressedBegin && g.partnerPressedBegin;
}

void game_start_placing(GameState& g) {
  g.phase = PHASE_PLACE;
  g.activeView = VIEW_FLEET;
  g.cursorR = 0;
  g.cursorC = 0;
}

void game_title_to_instructions(GameState& g) {
  // Don't clear begin flags — partner may have already advanced and sent
  // BEGIN while we were on the title screen. Their flag should persist.
  g.phase = PHASE_INSTRUCTIONS;
}
