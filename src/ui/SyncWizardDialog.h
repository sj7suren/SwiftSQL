// SyncWizardDialog.h — the cross-database synchronization wizard (milestone D of
// docs/design/cross-db-sync.md §2). A wxSimplebook walks the user through:
//   0  source / target selection   (source prefilled + locked from the entry the
//                                    user invoked 同步到 on; target chosen from the
//                                    live connection list + a database dropdown)
//   1  mode & options              (structure / data, drop-missing policy,
//                                    transaction wrap, dry-run)
//   2  comparing                   (worker thread → SyncEngine::BuildPlan, cancellable)
//   3  差异审阅与 SQL              (SyncComparePage: ONE screen — the Navicat-style
//                                    compare grid on top, with per-table checkboxes
//                                    and 类型 / 结构差异 / 插入 / 更新 / 删除 / 状态
//                                    columns, and the SQL 预览 / 结构对比 / 数据对比
//                                    tabs directly below it. This replaces what used
//                                    to be two separate steps, so the SQL for the
//                                    current selection is visible AT THE SAME TIME as
//                                    the checkboxes instead of one screen later.)
// On 执行 it asks SyncComparePage for the filtered SyncPlan — built from
// ui::SyncSelection::Build()'s ExecutionSpec, so unchecked categories and (unless the
// delete master switch is on) every DELETE are structurally absent — and hands it to
// SyncRunnerDialog. dry-run stops at the preview without touching the target.
//
// The direction 源 ──▶ 目标 badge is pinned to the top of every page so the
// direction can never be read backwards. Same-connection-same-db selections and
// unsupported cross-engine pairs gate 下一步 with an honest reason (MVP is
// same-engine plus MySQL<->Postgres; see ui::CrossEngineSupported() in SyncCrossEngineGate.h).
// For a supported cross-engine pair, page 1's "同步数据" checkbox is greyed out
// (structure-only in this phase — see UpdateCrossEngineDataOption()).
//
// The compare grid populates three categories: "表" (Tables), which is the
// executable one, plus "函数" and "存储过程", which are COMPARE-ONLY (ADR-013).
// Routine rows have no checkbox, no id and no path into the execution spec —
// cross-engine procedural code cannot be safely auto-translated and same-engine
// routine sync is deferred, so a human rewrites. Page 1's 对比函数与存储过程
// switch turns the extra catalog reads off; it is NOT a db::sync::SyncScope
// field, because SyncScope is what plan BUILDING consumes and routines never
// build a plan.
//
// MVP simplifications (honestly gated — see the return handoff): the target list
// is the set of already-connected connections; Save/Load Profile is a placeholder.
#pragma once

#include <wx/string.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include "db/DbDriver.h"
#include "db/RoutineDiff.h"
#include "db/SyncEngine.h"
#include "ui/CenteredDialog.h"

class wxSimplebook;
class wxChoice;
class wxStaticText;
class wxButton;
class wxCheckBox;
class wxRadioButton;
class wxGauge;
class wxPanel;

namespace ui {

struct ConnEntry;       // ConnectionTree.h — used by pointer only
class SyncComparePage;  // SyncComparePage.h — used by pointer only

class SyncWizardDialog : public CenteredDialog {
public:
    enum class Mode { Structure = 0, Data = 1, Both = 2 };

    // srcEntry / srcDb are the locked source (the invoked database node). conns is
    // the candidate connection list (all currently-connected entries, source
    // included). preset seeds the mode checkboxes on page 1.
    SyncWizardDialog(wxWindow* parent, ConnEntry* srcEntry, const wxString& srcDb,
                     std::vector<ConnEntry*> conns, Mode preset);
    ~SyncWizardDialog() override;

    bool DidRun() const { return didRun_; }

private:
    struct Side { ConnEntry* entry = nullptr; wxString db; };

    // --- build ---
    wxWindow* BuildSelectPage();
    wxWindow* BuildOptionPage();
    wxWindow* BuildComparingPage();
    wxWindow* BuildReviewPage();
    void      BuildNavBar(wxBoxSizer* root);

    // --- navigation ---
    void GoTo(int step);
    void OnNext(wxCommandEvent&);
    void OnPrev(wxCommandEvent&);
    void UpdateNextEnabled();

    // --- page 0 helpers ---
    void FillConnChoice(wxChoice* c);
    void OnConnChanged(bool source);          // repopulate that side's db list
    void RefreshInfo();                       // both info panels + warning + gate
    void OnSwap();
    Side ReadSide(bool source) const;
    bool SelectionValid(wxString& reason) const;
    void UpdateCrossEngineDataOption();       // grey out cbData_ when src/tgt dialects differ (structure-only phase)

    // --- page 2 (compare) ---
    void StartCompare();
    void CancelCompare();
    void OnCompareProgress(const wxString& phase, int done, int total);
    void OnCompareDone(bool ok, const wxString& err);

    // --- scripted verification hook (SWIFTSQL_SYNCSPEC) ---
    // No-op unless the env var is set. See the definition for the output
    // contract and, above all, for what a green run does NOT establish.
    void RunScriptedSpec();

    // --- execute ---
    void Execute();
    void JoinWorker();

    // context
    ConnEntry*               srcEntry_ = nullptr;
    wxString                 srcDb_;
    std::vector<ConnEntry*>  conns_;
    Mode                     preset_;

    // captured selections (set when compare starts)
    Side src_, tgt_;

    // options
    bool optStructure_ = true, optData_ = false, optDrop_ = false;
    bool optTxn_ = true, optDryRun_ = false;

    // COMPARE-only, and deliberately not a db::sync::SyncScope field (ADR-013
    // Q5). SyncScope is the input to plan BUILDING — i.e. to what gets
    // EXECUTED — so a compare-only flag there would invite an
    // `if (scope.routines)` branch inside plan construction. This one is copied
    // into db::sync::RoutineCompareOptions::enabled, which plan construction
    // never reads.
    bool optRoutines_ = true;

    // plan — the compared result, owned here because SyncComparePage borrows
    // it. Check state does NOT live here and does not live in any widget:
    // SyncComparePage owns the one ui::SyncSelection for this compare, keyed by
    // table name, never by row position.
    db::sync::SyncPlan plan_;

    // The compare-only functions & stored procedures result, produced on the
    // same worker thread and on the same cloned connections as the plan. Kept
    // next to plan_ but structurally apart from it: RoutineDiffSet is not part
    // of SyncPlan (ADR-013 Q1) precisely so that nothing SyncEngine::Execute
    // consumes can reach it, and this member is passed to SyncComparePage for
    // display and to nothing else.
    db::sync::RoutineDiffSet routines_;

    // widgets
    wxSimplebook* book_ = nullptr;
    wxStaticText* stepLbl_ = nullptr;
    wxStaticText* dirSrc_ = nullptr;
    wxStaticText* dirTgt_ = nullptr;

    wxChoice*     srcConn_ = nullptr;
    wxChoice*     srcDbc_  = nullptr;
    wxChoice*     tgtConn_ = nullptr;
    wxChoice*     tgtDbc_  = nullptr;
    wxStaticText* srcInfo_[4] = { nullptr, nullptr, nullptr, nullptr };
    wxStaticText* tgtInfo_[4] = { nullptr, nullptr, nullptr, nullptr };
    wxStaticText* warn_ = nullptr;

    wxCheckBox*    cbStructure_ = nullptr;
    wxCheckBox*    cbData_ = nullptr;
    wxCheckBox*    cbRoutines_ = nullptr;
    wxRadioButton* rbKeep_ = nullptr;
    wxRadioButton* rbDrop_ = nullptr;
    wxCheckBox*    cbTxn_ = nullptr;
    wxCheckBox*    cbDryRun_ = nullptr;

    wxGauge*      cmpGauge_ = nullptr;
    wxStaticText* cmpStatus_ = nullptr;

    // The single-screen compare page (T8): grid on top, SQL preview +
    // side-by-side structure/data comparison below. Owns the DiffTree and the
    // SyncSelection for this compare.
    SyncComparePage* comparePage_ = nullptr;

    wxButton*     btnPrev_ = nullptr;
    wxButton*     btnNext_ = nullptr;
    wxButton*     btnCancel_ = nullptr;

    // worker (compare) — runs on its OWN cloned connections (db::CloneConnection),
    // never src_.entry->conn / tgt_.entry->conn directly: db::IConnection is not
    // thread-safe, and those are the UI thread's connections. cmpSrcConn_ /
    // cmpTgtConn_ are non-owning observers of the worker's clones (the clones
    // themselves live and die inside the worker lambda's scope), published only
    // for as long as they're alive so CancelCompare()/JoinWorker() (UI thread)
    // can still Cancel() whatever query is actually in flight. cmpConnMx_ makes
    // "read the pointer + Cancel() it" and "null it out before destroying the
    // clone" mutually exclusive, so a Cancel() call can never reach a
    // half-destroyed connection.
    std::thread       worker_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> comparing_{false};
    std::mutex        cmpConnMx_;
    db::IConnection*  cmpSrcConn_ = nullptr;
    db::IConnection*  cmpTgtConn_ = nullptr;

    int  step_ = 0;
    bool didRun_ = false;
};

} // namespace ui
