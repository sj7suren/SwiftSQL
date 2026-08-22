// ProcessMonitor.h — cross-dialect server-session listing + kill for the 服务器监控
// tool. These are free functions (not IConnection virtuals) so every engine's session
// SQL lives in one place and the ceiling-height driver TUs stay untouched — the same
// "externalize a concern into its own TU" split UserAdminOracle.cpp uses.
//
// Each function drives the connection through its public Execute()/GetDialect() surface
// only, so nothing here depends on a specific driver's internals.
#pragma once

#include "db/DbDriver.h"

#include <vector>

namespace db {

// Whether the engine has a server-session concept at all (everything but SQLite,
// which is an in-process file). Drives the menu's "not supported" notice.
bool SupportsProcessList(Dialect d);

// List every live server session, mapped onto ProcessInfo. Returns false + err on a
// query/permission failure (the dialog surfaces err verbatim).
bool ListServerProcesses(IConnection& conn, std::vector<ProcessInfo>& out, wxString& err);

// Terminate one session by its engine-native id token (ProcessInfo::id). The token
// format is self-describing enough to pick the right primitive per engine.
bool KillServerProcess(IConnection& conn, const wxString& id, wxString& err);

} // namespace db
