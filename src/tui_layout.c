#include "tui_layout.h"
#include "tui.h"

int tui_layout_point_in_input(int rows, int cols, int y, int x) {
  int input_y = rows - INPUT_WIN_HEIGHT - MARGIN;
  int input_w = cols - 2 * MARGIN;
  return input_y >= 0 && input_w > 0 && y >= input_y &&
         y < input_y + INPUT_WIN_HEIGHT && x >= MARGIN &&
         x < MARGIN + input_w;
}
