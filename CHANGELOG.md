# Changelog

## 0.3.1 — 2026-10-09

### Fixed
Opening damaged or malicious files is now safe. A fuzzing campaign (over 115,000 corrupted files,
run with AddressSanitizer and UndefinedBehaviorSanitizer) found and fixed:
- A PSD that could freeze PairPaint for minutes (a layer name claiming ~4 billion characters).
- PSD layer bounds that overflowed, which could cause huge memory use.
- Unknown or out-of-range values in project files (blend modes, opacity, effect and font sizes,
  adjustment settings) that caused undefined behavior.
- A corrupted project that made Qt try to reserve 48 GB and crash; running out of memory while
  reading a file now reports it as damaged.

### Other
- Images over 250 megapixels are rejected with a clear message.
- Every push is now also tested with sanitizers and a short fuzzing run.

## 0.3.0 — 2026-10-09

### New
- **Spot Healing Brush** (J): paint over spots and blemishes; a matching patch nearby is found
  automatically. The Healing Brush with a chosen source moves to Shift+J.
- **Polygonal Lasso** (Shift+L): click corner points for straight-edged selections.
- **Crop**: aspect ratio presets (1:1, 4:5, 3:2, 16:9, 9:16, original), move and resize the frame,
  and drag outside it to straighten (rotate) the image.
- **Blur** (Shift+R) and **Sharpen** brushes, and **Sponge** to saturate or desaturate.

## 0.2.0 — 2026-10-09

### New
- **Text on the canvas**: click and type directly on the image, click existing text to edit it in
  place, select, copy and paste; font, size, color and style apply live.
- **Layer groups**: folders in the Layers panel with their own opacity, blend mode and mask,
  including Photoshop's Pass Through. Group with Ctrl+G, ungroup with Ctrl+Shift+G.
- **Layer styles**: Drop Shadow, Outer Glow and Stroke, non-destructive (Layer > Layer Style or `fx`).
- **Clipping masks** (Ctrl+Alt+G) and **Fill opacity**.
- **All 27 Photoshop blend modes**, including Linear Burn, Vivid Light, Hard Mix, Hue and Luminosity.
- **Smudge, Dodge and Burn** tools.
- **Selections**: Feather, Expand, Contract, Border, Smooth, Color Range, Load Layer Transparency.

### Photoshop files
- Opening PSDs now matches Photoshop's own rendering for blend modes, clipping masks, Pass Through
  groups, shape layers and vector masks, solid color fills, Fill opacity, and Levels, Curves
  (per channel), Invert, Threshold, Posterize and legacy Brightness/Contrast adjustment layers;
  drop shadow, outer glow and stroke effects are imported too.
- PairPaint tells you when a file uses something it can't reproduce exactly.
- PSD export keeps groups, clipping, Fill and blend modes.

### Fixed
- A possible crash when a window closed with unsaved changes.
- PSDs without layers no longer open with washed-out semi-transparent areas.

## 0.1.0 — 2026-10-09

First release: layers with masks, adjustment layers and editable text; painting, selection, clone,
healing and transform tools; filters; pen pressure; PSD import and export; downloads for Linux,
Windows and macOS.
