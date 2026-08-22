// CrashDialog.h — the friendly face of the crash safety net.
//
// Shown on the MAIN THREAD only (it builds real wx controls). Two moods:
//   fatal=false → a recoverable error was caught (worker/main-loop). Reassures the
//                 user their data is safe and lets them keep working.
//   fatal=true  → startup failed / the app must close. Explains calmly and points
//                 at the saved diagnostics.
//
// Hard faults (access violations) never reach here — the process is dying, so the
// SEH filter shows a minimal native MessageBox instead (see core/CrashLog.cpp).
#pragma once

#include <wx/string.h>

namespace ui {

void ShowCrashDialog(bool fatal, const wxString& context, const wxString& details);

}  // namespace ui
