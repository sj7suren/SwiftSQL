// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// Lang.h — i18n engine using language packs. A "pack" is a catalog mapping the
// canonical (Chinese) source string to a target language, keyed by the source
// itself (gettext-style msgid). Adding a language = adding a pack (an in-code
// table or an external lang/<code>.ini file) — call sites never change.
//
//   tr(L"新建连接")  →  looks up the current pack; falls back to the source.
//
// This lives in core: a cross-cutting concern, persisted, no GUI beyond wxString.
#pragma once

#include <wx/string.h>
#include <vector>

namespace core {

struct LangInfo {
    wxString code;   // "zh" / "en" / …
    wxString name;   // display name, e.g. "中文" / "English"
};

class Lang {
public:
    static wxString Current();               // language code, loaded from settings
    static void     Set(const wxString& code);   // persists (applies on restart)

    // Translate a Chinese source string via the current pack (fallback = source).
    static wxString tr(const wxString& zh);

    // All registered languages (built-in + external packs) for the picker.
    static std::vector<LangInfo> Available();
};

} // namespace core
