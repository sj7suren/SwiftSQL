// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// I18n.h — convenience wrapper so UI code writes tr(L"中文源串") and gets the
// current language's translation (via the language packs in core::Lang).
#pragma once

#include "core/Lang.h"

namespace ui {

inline wxString tr(const wchar_t* zh)
{
    return core::Lang::tr(zh);
}

} // namespace ui
