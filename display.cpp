// =============================================================================
// display.cpp — OLED rendering (Block 3, networked)
// =============================================================================
// Layout (128x64):
//   row labels at x=0..5
//   10x10 grid at (7, 0)..(67, 60), 6 px/cell
//   status panel at x=70..127
//
// Special full-screen states:
//   PHASE_BOOT, PHASE_LINK_WAIT, PHASE_INSTRUCTIONS, PHASE_LINK_LOST
// =============================================================================

#include "display.h"
#include <Adafruit_GFX.h>

static const uint8_t GRID_X = 7;
static const uint8_t GRID_Y = 0;
static const uint8_t CELL   = 6;
static const uint8_t PANEL_X = 70;

static void drawLabels(Adafruit_SSD1306& oled);
static void drawGridLines(Adafruit_SSD1306& oled);
static void drawCell(Adafruit_SSD1306& oled, uint8_t r, uint8_t c, CellState s);
static void drawGhost(Adafruit_SSD1306& oled, const GameState& g);
static void drawCursor(Adafruit_SSD1306& oled, uint8_t r, uint8_t c);
static void drawStatus(Adafruit_SSD1306& oled, const GameState& g);
static void drawTitle(Adafruit_SSD1306& oled);
static void drawInstructions(Adafruit_SSD1306& oled, const GameState& g);
static void drawPlayAgain(Adafruit_SSD1306& oled, const GameState& g);
static void drawFullScreenMessage(Adafruit_SSD1306& oled,
                                  const __FlashStringHelper* line1,
                                  const __FlashStringHelper* line2);

void display_init(Adafruit_SSD1306& oled) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.display();
}

void display_render(Adafruit_SSD1306& oled, const GameState& g) {
  if (g.phase == PHASE_BOOT) {
    drawFullScreenMessage(oled, F("Battleship"), F("Booting..."));
    return;
  }
  if (g.phase == PHASE_LINK_WAIT) {
    drawFullScreenMessage(oled, F("Linking"), F("..."));
    return;
  }
  if (g.phase == PHASE_TITLE) {
    drawTitle(oled);
    return;
  }
  if (g.phase == PHASE_INSTRUCTIONS) {
    drawInstructions(oled, g);
    return;
  }
  if (g.phase == PHASE_PLAY_AGAIN) {
    drawPlayAgain(oled, g);
    return;
  }
  if (g.phase == PHASE_LINK_LOST) {
    drawFullScreenMessage(oled, F("LINK LOST"), F("reset to retry"));
    return;
  }

  oled.clearDisplay();
  drawLabels(oled);
  drawGridLines(oled);

  for (uint8_t r = 0; r < GRID_N; r++) {
    for (uint8_t c = 0; c < GRID_N; c++) {
      CellState s = (g.activeView == VIEW_FLEET)
                        ? gs_get_my_fleet(g, r, c)
                        : gs_get_attack(g, r, c);
      drawCell(oled, r, c, s);
    }
  }

  if (g.phase == PHASE_PLACE && g.activeView == VIEW_FLEET) {
    drawGhost(oled, g);
  }

  bool showCursor = (g.phase == PHASE_MY_TURN && g.activeView == VIEW_ATTACK);
  if (g.phase == PHASE_PLACE && g.activeView == VIEW_ATTACK) showCursor = true;
  if (showCursor) drawCursor(oled, g.cursorR, g.cursorC);

  drawStatus(oled, g);
  oled.display();
}

// ---- Row labels -----------------------------------------------------------
static void drawLabels(Adafruit_SSD1306& oled) {
  oled.setTextSize(1);
  for (uint8_t r = 0; r < GRID_N; r++) {
    char ch = (r < 9) ? ('1' + r) : 'J';
    oled.setCursor(1, GRID_Y + r * CELL);
    oled.print(ch);
  }
}

static void drawGridLines(Adafruit_SSD1306& oled) {
  uint8_t end = GRID_N * CELL;
  for (uint8_t i = 0; i <= GRID_N; i++) {
    oled.drawFastVLine(GRID_X + i * CELL, GRID_Y, end + 1, SSD1306_WHITE);
    oled.drawFastHLine(GRID_X, GRID_Y + i * CELL, end + 1, SSD1306_WHITE);
  }
}

static void drawCell(Adafruit_SSD1306& oled, uint8_t r, uint8_t c, CellState s) {
  uint8_t x = GRID_X + c * CELL + 1;
  uint8_t y = GRID_Y + r * CELL + 1;
  switch (s) {
    case CELL_EMPTY: break;
    case CELL_SHIP:
      oled.fillRect(x + 1, y + 1, 3, 3, SSD1306_WHITE);
      break;
    case CELL_MISS:
      oled.drawPixel(x + 2, y + 2, SSD1306_WHITE);
      break;
    case CELL_HIT:
      oled.drawRect(x + 1, y + 1, 3, 3, SSD1306_WHITE);
      oled.drawPixel(x + 2, y + 2, SSD1306_WHITE);
      break;
    case CELL_SHIP_HIT:
      oled.fillRect(x + 1, y + 1, 3, 3, SSD1306_WHITE);
      oled.drawPixel(x + 2, y + 2, SSD1306_BLACK);
      break;
  }
}

static void drawGhostCellHatched(Adafruit_SSD1306& oled, uint8_t r, uint8_t c) {
  uint8_t x = GRID_X + c * CELL + 1;
  uint8_t y = GRID_Y + r * CELL + 1;
  for (uint8_t dy = 0; dy < 5; dy++)
    for (uint8_t dx = 0; dx < 5; dx++)
      if ((dx + dy) & 1)
        oled.drawPixel(x + dx, y + dy, SSD1306_WHITE);
}

static void drawGhostCellInvalid(Adafruit_SSD1306& oled, uint8_t r, uint8_t c) {
  uint8_t x = GRID_X + c * CELL + 1;
  uint8_t y = GRID_Y + r * CELL + 1;
  oled.fillRect(x, y, 5, 5, SSD1306_WHITE);
}

static void drawGhost(Adafruit_SSD1306& oled, const GameState& g) {
  uint8_t len = game_current_ship_length(g);
  if (len == 0) return;
  bool fits = game_ghost_fits(g, g.cursorR, g.cursorC, len, g.ghostHorizontal);
  if (!fits) {
    drawGhostCellInvalid(oled, g.cursorR, g.cursorC);
    return;
  }
  for (uint8_t k = 0; k < len; k++) {
    uint8_t r = g.ghostHorizontal ? g.cursorR : g.cursorR + k;
    uint8_t c = g.ghostHorizontal ? g.cursorC + k : g.cursorC;
    drawGhostCellHatched(oled, r, c);
  }
}

static void drawCursor(Adafruit_SSD1306& oled, uint8_t r, uint8_t c) {
  uint8_t x = GRID_X + c * CELL + 1;
  uint8_t y = GRID_Y + r * CELL + 1;
  oled.drawRect(x, y, 5, 5, SSD1306_INVERSE);
}

static void drawFullScreenMessage(Adafruit_SSD1306& oled,
                                  const __FlashStringHelper* line1,
                                  const __FlashStringHelper* line2) {
  oled.clearDisplay();
  oled.setTextSize(2);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 12);
  oled.print(line1);
  oled.setTextSize(1);
  oled.setCursor(0, 36);
  oled.print(line2);
  oled.display();
}

// ===========================================================================
// Title screen
// ===========================================================================
static void drawTitle(Adafruit_SSD1306& oled) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(8, 12);
  oled.print(F("Battleship"));
  oled.setTextSize(1);
  oled.setCursor(58, 32);
  oled.print(F(":)"));
  oled.setCursor(10, 52);
  oled.print(F("Press A to start"));
  oled.display();
}

// ===========================================================================
// Instructions screen
// ===========================================================================
static void drawInstructions(Adafruit_SSD1306& oled, const GameState& g) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print(F("BATTLESHIP"));

  oled.setCursor(0, 12);
  oled.print(F("A=left  B=right"));

  oled.setCursor(0, 22);
  oled.print(F("PLACE: A set B rot"));

  oled.setCursor(0, 32);
  oled.print(F("FIRE:  A fire B view"));

  oled.setCursor(0, 42);
  oled.print(F("Hold B 3s = reset"));

  oled.setCursor(0, 56);
  if (g.iPressedBegin && g.partnerPressedBegin) {
    oled.print(F("Starting!"));
  } else if (g.iPressedBegin) {
    oled.print(F("Waiting for opp..."));
  } else {
    oled.print(F("Press A to begin"));
  }

  oled.display();
}

// ===========================================================================
// Play-again screen (after game over)
// ===========================================================================
static void drawPlayAgain(Adafruit_SSD1306& oled, const GameState& g) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(0, 4);
  // Show win/loss message big
  bool won = (g.theirShipCellsHit >= TOTAL_SHIP_CELLS);
  oled.print(won ? F("YOU WON!") : F("YOU LOST"));

  oled.setTextSize(1);
  oled.setCursor(0, 28);
  oled.print(F("Play again?"));

  oled.setCursor(0, 40);
  oled.print(F("A=yes  B=quit"));

  oled.setCursor(0, 56);
  if (g.iPressedBegin && g.partnerPressedBegin) {
    oled.print(F("Next round!"));
  } else if (g.iPressedBegin) {
    oled.print(F("Waiting for opp..."));
  } else {
    oled.print(F("(any to choose)"));
  }

  oled.display();
}

// ===========================================================================
// Status panel
// ===========================================================================
static void drawStatus(Adafruit_SSD1306& oled, const GameState& g) {
  oled.setTextSize(1);

  // Line 1: phase label
  oled.setCursor(PANEL_X, 0);
  switch (g.phase) {
    case PHASE_PLACE:         oled.print(F("PLACE"));   break;
    case PHASE_SENDING_READY: oled.print(F("READY")); break;
    case PHASE_WAIT_OPP:      oled.print(F("WAIT"));    break;
    case PHASE_MY_TURN:       oled.print(F("YOU"));     break;
    case PHASE_OPP_TURN:      oled.print(F("OPP"));     break;
    case PHASE_GAME_OVER:     oled.print(F("DONE"));    break;
    default:                  oled.print(F("..."));     break;
  }

  // Line 2: cursor pos when meaningful
  if (g.phase == PHASE_PLACE || g.phase == PHASE_MY_TURN) {
    oled.setCursor(PANEL_X, 10);
    char col = 'A' + g.cursorC;
    char row = (g.cursorR < 9) ? ('1' + g.cursorR) : 'J';
    oled.print(F("Pos:"));
    oled.print(col);
    oled.print(row);
  }

  if (g.phase == PHASE_PLACE) {
    oled.setCursor(PANEL_X, 22);
    oled.print(F("Ship "));
    oled.print(g.currentShipIndex + 1);
    oled.print(F("/"));
    oled.print(NUM_SHIPS);

    oled.setCursor(PANEL_X, 32);
    oled.print(F("Len:"));
    oled.print(game_current_ship_length(g));

    oled.setCursor(PANEL_X, 42);
    oled.print(g.ghostHorizontal ? F("Horiz") : F("Vert"));
    return;
  }

  if (g.phase == PHASE_MY_TURN || g.phase == PHASE_OPP_TURN ||
      g.phase == PHASE_GAME_OVER) {
    oled.setCursor(PANEL_X, 22);
    oled.print((g.activeView == VIEW_FLEET) ? F("Fleet") : F("Attack"));

    oled.setCursor(PANEL_X, 34);
    oled.print(F("Mine:"));
    oled.print(g.myShipCellsRemaining);
    oled.print(F("/"));
    oled.print(TOTAL_SHIP_CELLS);

    oled.setCursor(PANEL_X, 44);
    oled.print(F("Hit:"));
    oled.print(g.theirShipCellsHit);
    oled.print(F("/"));
    oled.print(TOTAL_SHIP_CELLS);

    if (g.phase == PHASE_GAME_OVER) {
      oled.setCursor(PANEL_X, 54);
      oled.print(g.theirShipCellsHit >= TOTAL_SHIP_CELLS
                     ? F("YOU WON!") : F("YOU LOST"));
    }
    return;
  }

  if (g.phase == PHASE_SENDING_READY || g.phase == PHASE_WAIT_OPP) {
    oled.setCursor(PANEL_X, 22);
    oled.print(F("waiting"));
    oled.setCursor(PANEL_X, 32);
    oled.print(F("for opp"));
  }
}
