// OciConfig.h — public, vendor-free configuration surface for the runtime OCI
// loader. Declares only wxString-based entry points, so UI code (Preferences)
// and app-startup re-hydration can configure & probe the Oracle client WITHOUT
// pulling in <oci.h>. The Instant Client SDK header stays PRIVATE to
// swiftsql::db; OciLoader.h includes this header for the driver's own use.
#pragma once

#include <wx/string.h>

namespace db {

// Set the explicit oci.dll path. Accepts a full path to oci.dll *or* the
// directory that contains it. Empty => resolve only via exe dir / PATH. Setting
// a (different) path lets the next connection re-resolve the library.
void SetOciLibraryPath(const wxString& pathOrDir);

// The currently configured explicit path (may be empty).
wxString OciLibraryPath();

// Probe whether OCI can be loaded right now. On success sets `resolvedPath` to
// the loaded module's path and returns true; on failure returns false.
bool OciAvailable(wxString& resolvedPath);

} // namespace db
