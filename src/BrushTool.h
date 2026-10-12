// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include "Tools.h"

#include <memory>

// The brush tools: Brush, Color Replacement, Eraser, Background Eraser, Clone Stamp, Spot Healing Brush,
// Healing Brush, Smudge, Blur, Sharpen, Dodge, Burn and Sponge. Null for other tools.
std::unique_ptr<Tool> createBrushTool(Tool::Id id, ToolSettings *settings);
