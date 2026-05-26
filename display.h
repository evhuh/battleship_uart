// =============================================================================
// display.h — OLED rendering for battleship
// =============================================================================

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Adafruit_SSD1306.h>
#include "game.h"

void display_init(Adafruit_SSD1306& oled);
void display_render(Adafruit_SSD1306& oled, const GameState& g);

#endif
