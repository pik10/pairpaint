// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Peter Gniewek and PairPaint contributors

#pragma once

#include <QIcon>

#include "Tools.h"

// Icons are drawn in code so the app has no external assets and looks the
// same on every platform.
QIcon toolIcon(Tool::Id id);
QIcon appIcon();
