// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// Settings.h — thin generic accessor to the app-wide settings.ini, the very
// same file (in the user-data dir) that already persists the UI language. It
// centralises the user-data-dir path derivation so call sites (Preferences,
// startup re-hydration) never re-implement it. Keys are wxFileConfig paths,
// e.g. L"/general/ociLibraryPath".
#pragma once

#include <wx/string.h>

namespace core {

// Canonical setting keys (wxFileConfig paths). Kept here so the write site
// (Preferences) and read site (startup re-hydration) can never drift apart.
namespace keys {
inline const wchar_t* const kOciLibraryPath = L"/general/ociLibraryPath";
} // namespace keys

class Settings {
public:
    // Absolute path to settings.ini (shared with core::Lang).
    static wxString FilePath();

    static wxString ReadString(const wxString& key,
                               const wxString& def = wxEmptyString);
    static void     WriteString(const wxString& key, const wxString& value);
};

} // namespace core
