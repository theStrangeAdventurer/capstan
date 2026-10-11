/* Export the exact shaded half-pixels consumed by the TUI. */
#include "start_screen.h"
#include <stdio.h>
int main(void) {
  StartScreenAnimation animation = {0};
  for (int ms = 0; ms < START_SCREEN_ASSEMBLY_WALL_MS + START_SCREEN_INTRO_MS - START_SCREEN_ASSEMBLY_MS + 2 * START_SCREEN_WAVE_PERIOD_MS; ms += 20) {
    StartScreenFrame frame;
    start_screen_render_frame(start_screen_animation_frame(&animation, 1, ms), &frame);
    printf("{\"ms\":%d,\"pixels\":[", ms);
    for (int row = 0; row < START_SCREEN_CANVAS_ROWS; row++) {
      if (row) putchar(',');
      putchar('[');
      for (int col = 0; col < START_SCREEN_CANVAS_COLUMNS; col++)
        printf("%s%d", col ? "," : "", frame.pixels[row][col]);
      putchar(']');
    }
    puts("]}");
  }
  return 0;
}
