# PairPaint

[![CI](../../actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)

A cross-platform, layered raster image editor in the spirit of Photoshop, written in C++17 with Qt 6.
Runs on Linux, Windows and macOS from the same source.

## Features

- **Layers**: add, duplicate, delete, reorder, merge down, flatten, rename, show/hide, opacity,
  13 blend modes (Multiply, Screen, Overlay, Soft/Hard Light, Difference, …)
- **Layer masks**: reveal all / from selection, paint on the mask with any tool, disable, apply, delete
- **Adjustment layers** (non-destructive, with masks): Brightness/Contrast, Levels, Curves,
  Hue/Saturation, Invert, Threshold, Posterize. Double-click one to edit it.
- **Editable text layers**: font, size, color, bold/italic; click with the Text tool or double-click
  the layer to edit; moving keeps it editable, painting on it rasterizes it
- **Tools**: Move, Free Transform (scale/rotate/move), Rectangular/Elliptical Marquee, Lasso, Magic Wand,
  Crop, Eyedropper, Brush, Eraser, Clone Stamp, Healing Brush, Paint Bucket, Gradient (linear/radial),
  Line, Rectangle, Ellipse, Text, Hand, Zoom
- **Pen tablets**: pressure controls brush size and/or opacity (Brush, Eraser, Clone, Healing)
- **Selections**: pixel masks with anti-aliasing, add / subtract / intersect, invert, marching ants;
  every paint tool and filter respects the selection; Move and Free Transform act on selected pixels
- **Adjustments**: Levels, Curves (with histogram), Brightness/Contrast, Hue/Saturation, Desaturate,
  Invert, Threshold, Posterize
- **Filters**: Gaussian Blur, Unsharp Mask, Add Noise, Pixelate, all with live preview, multi-threaded
- **Image**: Image Size, Canvas Size (with anchor), Crop, Rotate, Flip
- **Undo** (80 steps) with a History panel, multiple documents in tabs
- **Files**: native `.pairpaint` project format (keeps everything); **Photoshop PSD** import (RGB, grayscale,
  CMYK; 8/16-bit; raw/RLE/ZIP; layers, masks, blend modes) and layered export; open/export PNG, JPEG,
  WebP, BMP, TIFF, GIF, …; clipboard copy/paste; drag and drop

## Building

Requirements: CMake ≥ 3.19, a C++17 compiler and Qt ≥ 6.2 (Widgets, Concurrent).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/pairpaint [image files...]
```

- **Arch**: `sudo pacman -S qt6-base cmake` (add `qt6-imageformats` for WebP/TIFF)
- **Debian/Ubuntu**: `sudo apt install qt6-base-dev cmake build-essential qt6-image-formats-plugins`
- **Fedora**: `sudo dnf install qt6-qtbase-devel cmake gcc-c++ qt6-qtimageformats`
- **Windows**: install Qt 6 with the online installer (MSVC kit), build from a Qt command prompt,
  then run `windeployqt build\pairpaint.exe` to bundle the DLLs.
- **macOS**: `brew install qt cmake`, build, then `macdeployqt build/pairpaint.app`.

## Keyboard shortcuts

| Tool | Key | | Action | Key |
|---|---|---|---|---|
| Move / Free Transform | V / Ctrl+T | | Swap / reset colors | X / D |
| Rect / Ellipse marquee | M / Shift+M | | Brush size | [ / ] |
| Lasso | L | | Pan (temporary) | hold Space |
| Magic Wand | W | | Zoom | Ctrl+wheel, Ctrl +/-, Ctrl+0 fit, Ctrl+1 100% |
| Crop | C | | Select all / deselect / inverse | Ctrl+A / Ctrl+D / Ctrl+Shift+I |
| Eyedropper | I | | New layer / duplicate / merge | Ctrl+Shift+N / Ctrl+J / Ctrl+E |
| Brush / Eraser | B / E | | Fill fg / bg | Alt+Backspace / Ctrl+Backspace |
| Clone Stamp / Healing | S / J | | Levels / Curves | Ctrl+L / Ctrl+M |
| | | | Toggle editing mask / layer | Ctrl+\\ |
| Paint Bucket / Gradient | K / G | | Hue/Sat, Invert, Desaturate | Ctrl+U, Ctrl+I, Ctrl+Shift+U |
| Line / Rect / Ellipse | N / U / Shift+U | | Export | Ctrl+Shift+E |
| Text / Hand / Zoom | T / H / Z | | | |

Selection modifiers: Shift adds, Alt or Ctrl subtracts, Shift+Alt intersects.
Clone Stamp / Healing Brush: Alt-click (or Ctrl-click) sets the source point.
Free Transform: drag handles to scale (Shift keeps proportions), outside the box to rotate (Shift snaps
to 15°), inside to move; Enter applies, Esc cancels.

## Code layout

| File | Purpose |
|---|---|
| `Document` | Layers, masks, adjustment/text layers, selection, compositing, all undoable operations (snapshot undo using Qt's implicitly shared images) |
| `Adjustments` | The adjustment types shared by the Adjustments menu and adjustment layers |
| `Psd` | Photoshop file reader/writer |
| `Canvas` | Zoom/pan view, cached composite, marching ants, routes input to tools |
| `Tools` | One class per tool plus `ToolManager` |
| `Filters` | Pixel algorithms (blur, levels, curves, healing, flood fill), parallelised with QtConcurrent |
| `FileIO` | `.pairpaint` project format and flat image import/export |
| `MainWindow`, `LayersPanel`, `ColorWidgets`, `Dialogs`, `FilterDialog` | User interface |

## Limitations

- Editing is 8 bits per channel RGB. 16-bit and CMYK PSD files are converted to 8-bit RGB when opened.
- PSD export writes pixel layers and masks; adjustment layers are left out (keep them in `.pairpaint`),
  text layers are exported as pixels. Large-document PSB files are not supported.
- Layer groups, vector shapes and layer styles are not supported (PSD groups are flattened into a plain layer list).

## Tests

The test suite drives the real user interface offscreen (painting, selections, masks, adjustment layers,
text, transform, clone/heal, filters, undo, and PSD/project round-trips) and checks the resulting pixels:

```sh
cmake --build build -j && ctest --test-dir build --output-on-failure
```

GitHub Actions runs it on Linux, Windows and macOS for every push and pull request.

## License

PairPaint is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of
the License, or (at your option) any later version. See [LICENSE](LICENSE).

Copyright © 2026 Peter Gniewek and PairPaint contributors.
