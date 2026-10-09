# Changelog

## Unreleased

### New
- **Brush smoothing**: a Smoothing option for all brush tools steadies shaky strokes. The brush
  follows the pointer on a string (shown while painting) and catches up when you let go.
- **Rulers** (View > Rulers, Ctrl+R) along the top and left edge, showing where the pointer is.
- **Guides**: drag them out of the rulers, or add one at an exact position with View > New Guide.
  Move them with the Move tool, drop them back on a ruler to delete them, or Clear Guides. They
  follow crops, resizes, rotations and flips, can be undone, and are saved in projects and PSD files.
- **Snapping** (View > Snap): selections, shapes, crop, gradients and the Move tool snap to guides
  and to the canvas edges and center, so centering something is easy. The line snapped to is
  highlighted while you drag.

### Changed
- Project files are now version 8 (for guides).

## 0.6.0 — 2026-10-09

### New
- **Autosave and crash recovery**: every 2 minutes, images with unsaved changes are copied to a
  recovery folder, in the background so editing doesn't pause. If PairPaint crashes or the
  computer loses power, the next start offers to recover them (or discard them, or decide
  later). The copies are removed when you save, close the image or quit normally.

## 0.5.0 — 2026-10-09

### New
- **HEIC photos** (the default format of iPhone cameras) open on all systems, turned upright:
  - Linux: decoded with libheif, which the AppImage includes.
  - Windows: decoded by Windows itself, which needs the "HEIF Image Extensions" and
    "HEVC Video Extensions" from the Microsoft Store (often preinstalled). Without them,
    PairPaint says what to install.
  - macOS: decoded by macOS.

### Fixed
- Photos with a wide-gamut color profile (Display P3 from phones, Adobe RGB) looked dull: they
  are now converted to sRGB when opened.

## 0.4.0 — 2026-10-09

### New
- **Auto Tone**, **Auto Contrast** and **Auto Color** (Image > Adjustments): one-click fixes for
  dull or flat photos; Auto Color also removes color casts.
- **White Balance** adjustment: Temperature (cooler / warmer) and Tint (green / magenta).
- **Vibrance**, **Exposure** and **Color Balance** adjustments, also as adjustment layers. Photoshop
  files with these adjustment layers now open with them (Vibrance and Color Balance approximated).
- **JPEG and WebP quality** setting when exporting, remembered for next time.

### Changed
- Project files are now version 7 (for the new adjustment layers).

## 0.3.2 — 2026-10-09

### Fixed
A code review of 0.3.1 found attacks the fuzzer had missed. Files built on purpose could still
exhaust memory or crash PairPaint:
- PSD effects nested thousands of levels deep overflowed the stack (a crash).
- PSD and project files with many layers on a large canvas could use tens of gigabytes: every
  layer is a full-canvas image. Layers without pixels (groups, adjustments, empty layers) now share
  one image, and a file whose layers need more than 8× the image size limit in total is rejected.
- Small ZIP-compressed PSD channels or flattened images claiming huge sizes, and impossible channel
  counts, are rejected before memory is reserved.
- PNG, JPEG and other image files are checked for their size before they are decoded.
- Out-of-range Hue/Saturation color-range settings from files were not fully clamped.

### Changed
- Project files are now version 6: the text font is stored in a portable form instead of Qt's
  binary font format, which differs between Qt versions. Older projects still open; projects
  saved by this version need this version or newer.
- Fully transparent layers are stored without image data, so projects are smaller.

## 0.3.1 — 2026-10-09

### Fixed
Opening damaged files is much safer (some deliberately crafted files could still crash PairPaint
or use too much memory; fixed in 0.3.2). A fuzzing campaign (over 115,000 corrupted files,
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
