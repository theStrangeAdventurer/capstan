#!/bin/bash
set -e
cd "$(dirname "$0")"

rm -f dracula.gif nord.gif latte.gif tokyonight.gif

echo "=== Recording 4 themes ==="
vhs dracula.tape
vhs nord.tape
vhs latte.tape
vhs tokyonight.tape

echo "=== Stitching 2x2 grid ==="
ffmpeg -y \
  -i dracula.gif -i nord.gif \
  -i latte.gif -i tokyonight.gif \
  -filter_complex "
    [0:v][1:v]hstack=2[top];
    [2:v][3:v]hstack=2[bot];
    [top][bot]vstack=2,split[v][p];
    [p]palettegen=max_colors=255:reserve_transparent=0:stats_mode=diff[palt];
    [v][palt]paletteuse=dither=floyd_steinberg
  " \
  ../themes-demo.gif

echo "=== Done: $(du -h ../themes-demo.gif | cut -f1) ==="