// SyncWizardHook.cpp — the wizard's SCRIPTED VERIFICATION HOOK, kept out of
// SyncWizardDialog.cpp because that TU is at 880 lines against the charter's
// 1000-line cap and this hook is a self-contained subject: it is the only code
// in the wizard that exists for verification rather than for the user.
//
// Same split rationale as ConnectionTree_Events.cpp / EditorPageFind.cpp — one
// TU, one answer to "what is this file for?".
//
// HONEST STATUS OF THIS HOOK. Its payload — ui::syncspec::RunSpecScript and
// FormatScriptReport — is covered headlessly by tests/SyncWizardSpecTests.cpp
// (73 assertions, each proven to fail under a deliberate mutation). The three
// lines BELOW that payload (the env read, RefreshFromSession, the file write)
// have never executed, because reaching OnCompareDone requires a human to pick
// a target connection and click 下一步 twice: this wizard has no scripted entry
// point. Firing it therefore needs either a hand-driven run or a further hook
// that opens the wizard and auto-advances pages 0 and 1. Do not cite this file
// as verified until one of those has happened.
#include "ui/SyncWizardDialog.h"

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include "ui/SyncComparePage.h"
#include "ui/SyncSpecScript.h"

namespace ui {

// ---------------------------------------------------------------------------
// Scripted verification hook (SWIFTSQL_SYNCSPEC) — the wizard's counterpart to
// SWIFTSQL_AUTOEDIT in MainFrame_Events.cpp.
// ---------------------------------------------------------------------------
//
// SWIFTSQL_SYNCSPEC="<directives>" replays a sequence of compare-screen
// gestures against the REAL page's model — the same ui::SyncCompareSession
// 执行 builds its two plans from, not a copy — and writes the resulting
// db::sync::DataExecPlan and filtered db::sync::SyncPlan to
// %TEMP%/swiftsql_syncspec.txt.
//
// OUTPUT CONTRACT: line 1 is a verdict token, then one G<n>| line per gesture,
// then the plan render. The vocabulary is ui::syncspec::ScriptToken /
// GestureToken; see SyncSpecScript.h for why each benign outcome has its own
// token instead of collapsing into one error-shaped line — that is the lesson
// SWIFTSQL_AUTOEDIT's old "empty diff reported as ERR:" taught, applied to a
// screen whose output authorizes writes.
//
// SCOPE, STATED SO THE FILE CANNOT BE OVER-READ: this asserts the BACKEND
// CONTRACT REACHABLE FROM THE GUI SEAT. It runs after a real compare against
// real servers and it drives the real dialog's model, so a plan it renders is a
// plan 执行 would hand SyncRunnerDialog. It observes NO widget: it cannot tell
// you whether a checkbox drew ticked, whether the 删除 column greyed out, or
// whether the summary label agrees. A green file is not evidence the screen
// renders.
void SyncWizardDialog::RunScriptedSpec()
{
    wxString script;
    if (!wxGetEnv(L"SWIFTSQL_SYNCSPEC", &script) || script.IsEmpty()) return;
    if (!comparePage_) return;

    const syncspec::ScriptResult r =
        syncspec::RunSpecScript(comparePage_->Session(), script);
    comparePage_->RefreshFromSession();

    wxFile out(wxFileName::GetTempDir() + L"/swiftsql_syncspec.txt", wxFile::write);
    if (out.IsOpened()) out.Write(syncspec::FormatScriptReport(r));
}

} // namespace ui
