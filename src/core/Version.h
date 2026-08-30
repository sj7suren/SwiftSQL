// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// Version.h — SINGLE SOURCE OF TRUTH for the product version.
//
// Included by BOTH the C++ display code (core/AppVersion.h → splash / About) AND the
// Windows resource script (src/win/SwiftSQL.rc → exe File/Product version). Editing
// the five macros below is the ONLY place the version needs to change; every other
// place reads from here, so the startup screen and the binary metadata can never
// drift apart again.
//
// Keep this file free of any C++ constructs (no namespaces / constexpr / includes):
// the .rc preprocessor must be able to parse it too. Classic include guard (not
// #pragma once) for the same reason.
#ifndef SWIFTSQL_VERSION_H
#define SWIFTSQL_VERSION_H

#define SWIFTSQL_VER_MAJOR 1
#define SWIFTSQL_VER_MINOR 1
#define SWIFTSQL_VER_PATCH 21
#define SWIFTSQL_VER_BUILD 0

// Dotted string form, e.g. "1.1.21". Kept alongside the numeric parts in this one
// file so both live in a single place.
#define SWIFTSQL_VER_STR "1.1.21"

#endif // SWIFTSQL_VERSION_H
