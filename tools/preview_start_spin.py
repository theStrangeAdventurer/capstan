"""Render native exporter JSONL; requires Pillow only for preview generation."""
import json
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
frames = []
for line in (root / 'preview/frames.jsonl').read_text().splitlines():
    data = json.loads(line)
    image = Image.new('RGB', (840, 290), '#14191e')
    draw = ImageDraw.Draw(image)
    draw.text((96, 34), 'CAPSTAN / PIXEL ASSEMBLY', fill='#b4bec7')
    for y, row in enumerate(data['pixels']):
        for x, shade in enumerate(row):
            if shade:
                gray = 255 if shade == 7 else 128 + shade * 10
                draw.rectangle((72+x*12, 66+y*12, 83+x*12, 77+y*12), fill=(gray,)*3)
    draw.text((96, 240), 'Assembly / contact flicker / ignition / original light wave', fill='#88939d')
    frames.append(image)
frames[0].save(root / 'preview/capstan-pixel-assembly.gif', save_all=True,
               append_images=frames[1:], duration=20, loop=0, disposal=2)
# Assembly, dim logo, faulty-contact flash, ignition and original wave.
selected = [20, 45, 75, 105, 135, 195]
sheet = Image.new('RGB', (840, 290*len(selected)))
for i, n in enumerate(selected):
    sheet.paste(frames[n], (0, i*290))
sheet.save(root / 'preview/capstan-pixel-assembly-frames.png')
