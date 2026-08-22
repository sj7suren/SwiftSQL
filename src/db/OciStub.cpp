// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// OciStub.cpp — link-time stand-in for the native Oracle OCI driver. Compiled
// ONLY when the build has no Oracle Instant Client SDK (see src/db/CMakeLists.txt).
//
// WHY THIS FILE EXISTS
// <oci.h> is a vendor header that Oracle's licence does not let us redistribute,
// so it cannot live in this repository. It is needed at COMPILE time only (for
// OCI types/structs/constants) -- no oci.lib is ever linked, because every OCI
// function is late-bound from oci.dll at runtime by OciLoader.cpp.
//
// Without this stub, that compile-time-only dependency made the whole project
// unbuildable for anyone who had not separately downloaded the Instant Client
// SDK -- including the large majority of users who never touch Oracle. Now the
// two OCI translation units (OciLoader.cpp, OracleOciDriver.cpp) are simply left
// out of the build when the SDK is absent, and this file takes their place.
//
// It defines exactly the symbols the rest of the application links against:
//   * db::CreateOracleConnection() -- dispatched to from DbDriver.cpp
//   * the three vendor-free entry points declared in OciConfig.h, which
//     main.cpp / ConnectionTree.cpp / PreferencesDialog.cpp call unconditionally
//
// BEHAVIOUR: Oracle reports itself as unavailable; every other engine is
// unaffected. To get the real driver back, put the SDK under third_party/ (it is
// auto-detected) or configure with -DSWIFTSQL_OCI_SDK=<path to the sdk dir>.
#include "db/DbDriver.h"
#include "db/OciConfig.h"

#include <memory>

namespace db {

// No OCI driver in this build. Returning nullptr makes CreateConnection() report
// the same "unsupported engine" path it already uses for engines without a
// driver, so no caller needs to know the difference.
std::unique_ptr<IConnection> CreateOracleConnection()
{
    return nullptr;
}

// Preferences may still show and store an OCI path; it simply has no effect
// until the app is rebuilt with the SDK. Hold the value so the settings screen
// reads back what the user typed instead of silently blanking it -- a control
// that discards its own input looks like a bug, not like a disabled feature.
namespace {
wxString g_ociPath;
}

void SetOciLibraryPath(const wxString& pathOrDir)
{
    g_ociPath = pathOrDir;
}

wxString OciLibraryPath()
{
    return g_ociPath;
}

bool OciAvailable(wxString& resolvedPath)
{
    resolvedPath.clear();
    return false;
}

} // namespace db
