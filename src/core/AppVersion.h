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
// resource. SwiftSQL is open source under the Apache License 2.0, so the holder
// is the project itself rather than a company — see LICENSE at the repo root.
// src/win/SwiftSQL.rc must be kept in sync by hand: the resource compiler cannot
// read these C++ constants, so it carries its own copy of the same strings.
inline constexpr const wchar_t* kCopyrightText =
    L"Copyright (c) 2026 SwiftSQL Contributors";
inline constexpr const wchar_t* kLicenseText = L"Apache License 2.0";

} // namespace core
