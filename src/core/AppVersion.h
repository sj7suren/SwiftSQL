// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AppVersion.h - user-facing build/version labels.
//
// The version number itself lives in ONE place — core/Version.h — which the Windows
// resource script also includes, so the splash/About text and the exe's file version
// are always the same. Do not hardcode the version here; edit core/Version.h.
#pragma once

#include "core/Version.h"

namespace core {

// Widen the single-source dotted version to L"vMAJOR.MINOR.PATCH" (e.g. L"v1.1.20").
// SWIFTSQL_VER_STR is a single string literal, so the L## paste + adjacent-literal
// concatenation below is well-formed.
#define SW_WSTR2(x) L##x
#define SW_WSTR(x)  SW_WSTR2(x)
inline constexpr const wchar_t* kAppVersionText = L"v" SW_WSTR(SWIFTSQL_VER_STR);
#undef SW_WSTR
#undef SW_WSTR2
// Attribution shown in the About dialog, the splash footer and the exe version
// resource. SwiftSQL is free software under the GNU GPL v3.0 or later, so the
// holder is the project itself rather than a company — see LICENSE at the repo
// root. src/win/SwiftSQL.rc must be kept in sync by hand: the resource compiler
// cannot read these C++ constants, so it carries its own copy of the same
// strings.
inline constexpr const wchar_t* kCopyrightText =
    L"Copyright (C) 2026 SwiftSQL Contributors";
inline constexpr const wchar_t* kLicenseText = L"GNU GPL v3.0 or later";
// Where this binary's corresponding source lives. Under the GPL a distributed
// binary must be accompanied by its source or by an offer for it (GPLv3 s.6);
// surfacing the URL in the About dialog is how that offer reaches the person
// actually holding the program, rather than only the person who downloaded the
// installer and read its licence page.
inline constexpr const wchar_t* kSourceUrl = L"https://github.com/sj7suren/SwiftSQL";

} // namespace core
