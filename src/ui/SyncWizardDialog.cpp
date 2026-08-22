// SyncWizardDialog.cpp — see header. Four-page wxSimplebook (three numbered
// steps plus a transient progress page) driving the cross-database sync suite.
// The compare step runs SyncEngine::BuildPlan on a worker thread (cancellable);
// review + SQL preview are one screen (SyncComparePage); execution is delegated
// to SyncRunnerDialog.
#include "ui/SyncWizardDialog.h"
#include "core/CrashLog.h"
#include "db/ConnectionClone.h"
#include "db/MySqlRoutineRead.h"
#include "db/PgRoutineRead.h"
#include "db/RoutineCompare.h"

#include <wx/wx.h>
#include <wx/simplebook.h>
#include <wx/gauge.h>
#include <wx/statline.h>

#include "ui/ConnectionTree.h"    // ConnEntry (full type)
#include "ui/SyncComparePage.h"
#include "ui/SyncCrossEngineGate.h"
#include "ui/SyncRunnerDialog.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

// Page indices. The comparing page is not a numbered step — it is a transient
// progress screen the user never navigates to deliberately — which is why the
// labels below read "N / 3" over four book pages.
enum Page { kPageSelect = 0, kPageOptions, kPageComparing, kPageReview, kPageCount };

namespace {

// ---------------------------------------------------------------------------
// Functions & stored procedures — the DIALECT DISPATCH (ADR-013)
// ---------------------------------------------------------------------------
// This lives in the UI on purpose. db::sync::CompareRoutines is pure and knows
// nothing about connections; the catalog readers are per-engine free functions.
// Somebody has to choose between them, and doing it here keeps the compare
// layer free of an engine switch it would otherwise have to grow for every new
// driver.
//
// An engine with no reader is Unsupported with an EMPTY vector — never an empty
// "successful" read. CompareRoutines then returns a set whose Comparable() is
// false and which carries no pairs at all, so an engine we cannot introspect
// can never be reported as "has no functions".
db::sync::RoutineReadStatus ReadRoutinesFor(db::IConnection& conn, const wxString& database,
                                            std::vector<db::sync::RoutineDef>& out,
                                            wxString& detail)
{
    // PgRoutineRead reads whatever database the session is bound to (libpq
    // sessions are per-database), so the side must be pointed at the right one
    // first. BuildPlan's ListTables has already done that via the same public
    // API; doing it explicitly means this function does not depend on having
    // been called after it.
    wxString useErr;
    if (!database.IsEmpty() && !conn.UseDatabase(database, useErr)) {
        detail = useErr;
        return db::sync::RoutineReadStatus::Unsupported;
    }

    switch (conn.GetDialect()) {
    case db::Dialect::MySQL:
        return db::mysqlroutine::ReadRoutines(conn, database, out, detail);
    case db::Dialect::Postgres:
        // Empty filter == every non-system schema of the current database.
        return db::pgroutine::ReadRoutines(conn, wxString(), out, detail);
    case db::Dialect::Sqlite:
    case db::Dialect::SqlServer:
    case db::Dialect::Oracle:
        break;
    }
    detail = tr(L"该数据库类型暂不支持读取函数与存储过程定义。");
    return db::sync::RoutineReadStatus::Unsupported;
}

// Read both catalogs and compare. Returns a VALUE — one that is not part of
// SyncPlan and that SyncEngine::Execute never sees (ADR-013 Q1).
db::sync::RoutineDiffSet CompareRoutinesOn(db::IConnection& src, db::IConnection& tgt,
                                           const wxString& srcDb, const wxString& tgtDb)
{
    std::vector<db::sync::RoutineDef> sr, tr_;
    wxString sDetail, tDetail;
    const db::sync::RoutineReadStatus ss = ReadRoutinesFor(src, srcDb, sr, sDetail);
    const db::sync::RoutineReadStatus ts = ReadRoutinesFor(tgt, tgtDb, tr_, tDetail);

    db::sync::RoutineCompareOptions opt =
        db::sync::MakeRoutineCompareOptions(src.GetDialect(), tgt.GetDialect());
    opt.enabled = true;   // the caller decided; see optRoutines_

    db::sync::RoutineDiffSet out = db::sync::CompareRoutines(sr, tr_, ss, ts, opt);
    out.sourceStatusDetail = sDetail;
    out.targetStatusDetail = tDetail;
    return out;
}

} // namespace

// CrossEngineSupported() moved to SyncCrossEngineGate.h/.cpp (QA-scope
// exception, see that header's comment) so it can be unit-tested directly --
// this .cpp calls it via the #include above instead of defining it locally.

SyncWizardDialog::SyncWizardDialog(wxWindow* parent, ConnEntry* srcEntry,
                                   const wxString& srcDb,
                                   std::vector<ConnEntry*> conns, Mode preset)
    : CenteredDialog(parent, wxID_ANY, tr(L"数据同步"), wxDefaultPosition,
                     wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , srcEntry_(srcEntry)
    , srcDb_(srcDb)
    , conns_(std::move(conns))
    , preset_(preset)
{
    optStructure_ = (preset != Mode::Data);
    optData_      = (preset != Mode::Structure);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // --- pinned header: step + direction badge (visible on every page) ---
    auto* header = new wxPanel(this);
    header->SetBackgroundColour(theme::kToolbarBg);
    auto* hs = new wxBoxSizer(wxHORIZONTAL);
    stepLbl_ = new wxStaticText(header, wxID_ANY, wxEmptyString);
    stepLbl_->SetFont(Ui(9, /*bold*/ true));
    stepLbl_->SetForegroundColour(theme::kTextSecondary);
    hs->Add(stepLbl_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 16);
    hs->AddStretchSpacer(1);
    dirSrc_ = new wxStaticText(header, wxID_ANY, tr(L"源"));
    dirSrc_->SetFont(Ui(10, /*bold*/ true));
    dirSrc_->SetForegroundColour(theme::kPrimary);
    auto* arrow = new wxStaticText(header, wxID_ANY, L"  ──▶  ");
    arrow->SetFont(Ui(10, /*bold*/ true));
    arrow->SetForegroundColour(theme::kPrimary);
    dirTgt_ = new wxStaticText(header, wxID_ANY, tr(L"目标"));
    dirTgt_->SetFont(Ui(10, /*bold*/ true));
    dirTgt_->SetForegroundColour(theme::kTextMuted);
    hs->Add(dirSrc_, 0, wxALIGN_CENTER_VERTICAL);
    hs->Add(arrow, 0, wxALIGN_CENTER_VERTICAL);
    hs->Add(dirTgt_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    header->SetSizer(hs);
    hs->SetMinSize(wxSize(-1, FromDIP(40)));
    root->Add(header, 0, wxEXPAND);
    root->Add(new wxStaticLine(this), 0, wxEXPAND);

    book_ = new wxSimplebook(this, wxID_ANY);
    book_->AddPage(BuildSelectPage(),    L"select");
    book_->AddPage(BuildOptionPage(),    L"options");
    book_->AddPage(BuildComparingPage(), L"comparing");
    book_->AddPage(BuildReviewPage(),    L"review");
    root->Add(book_, 1, wxEXPAND);

    root->Add(new wxStaticLine(this), 0, wxEXPAND);
    BuildNavBar(root);

    SetSizerAndFit(root);
    // Wider/taller than the old wizard on purpose: the review step now shows an
    // 8-column grid AND the SQL/comparison panes on ONE screen, which is the
    // whole point of collapsing the two steps.
    SetSize(FromDIP(wxSize(1040, 720)));

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& ev) {
        if (comparing_.load()) { CancelCompare(); ev.Veto(); return; }
        JoinWorker();
        EndModal(didRun_ ? wxID_OK : wxID_CANCEL);
    });

    // Preselect the source and refresh once the controls exist.
    CallAfter([this]() {
        for (unsigned i = 0; i < srcConn_->GetCount(); ++i)
            if (static_cast<ConnEntry*>(srcConn_->GetClientData(i)) == srcEntry_) {
                srcConn_->SetSelection(i);
                break;
            }
        OnConnChanged(/*source*/ true);
        srcDbc_->SetStringSelection(srcDb_);   // default = invoked node, but selectable
        if (tgtConn_->GetCount() > 0) { tgtConn_->SetSelection(0); OnConnChanged(false); }
        RefreshInfo();
        GoTo(0);
    });
}

SyncWizardDialog::~SyncWizardDialog()
{
    JoinWorker();
}

// ===========================================================================
// Page 0 — source / target selection
// ===========================================================================
wxWindow* SyncWizardDialog::BuildSelectPage()
{
    auto* page = new wxPanel(book_);
    auto* s = new wxBoxSizer(wxVERTICAL);

    auto* cols = new wxBoxSizer(wxHORIZONTAL);

    // Build one side (source or target) as a titled column.
    auto buildSide = [&](const wxString& title, const wxColour& titleCol,
                         wxChoice*& connC, wxChoice*& dbC,
                         wxStaticText** info) -> wxSizer* {
        auto* col = new wxBoxSizer(wxVERTICAL);
        auto* t = new wxStaticText(page, wxID_ANY, title);
        t->SetFont(Ui(11, /*bold*/ true));
        t->SetForegroundColour(titleCol);
        col->Add(t, 0, wxBOTTOM, 8);

        auto* lc = new wxStaticText(page, wxID_ANY, tr(L"连接"));
        lc->SetForegroundColour(theme::kTextBody);
        col->Add(lc, 0, wxBOTTOM, 4);
        connC = new wxChoice(page, wxID_ANY);
        FillConnChoice(connC);
        col->Add(connC, 0, wxEXPAND | wxBOTTOM, 12);

        auto* ld = new wxStaticText(page, wxID_ANY, tr(L"数据库"));
        ld->SetForegroundColour(theme::kTextBody);
        col->Add(ld, 0, wxBOTTOM, 4);
        dbC = new wxChoice(page, wxID_ANY);
        col->Add(dbC, 0, wxEXPAND | wxBOTTOM, 16);

        auto* infoHdr = new wxStaticText(page, wxID_ANY, L"Information");
        infoHdr->SetFont(Ui(9, /*bold*/ true));
        infoHdr->SetForegroundColour(theme::kTextSecondary);
        col->Add(infoHdr, 0, wxBOTTOM, 6);

        auto* grid = new wxFlexGridSizer(2, wxSize(10, 4));
        grid->AddGrowableCol(1, 1);
        const wchar_t* keys[4] = { L"连接类型", L"主机", L"端口", L"服务器版本" };
        for (int i = 0; i < 4; ++i) {
            auto* k = new wxStaticText(page, wxID_ANY, tr(keys[i]));
            k->SetFont(Ui(9));
            k->SetForegroundColour(theme::kTextSecondary);
            info[i] = new wxStaticText(page, wxID_ANY, L"—");
            info[i]->SetFont(Ui(9));
            info[i]->SetForegroundColour(theme::kTextBody);
            grid->Add(k, 0);
            grid->Add(info[i], 1, wxEXPAND);
        }
        col->Add(grid, 0, wxEXPAND);
        return col;
    };

    cols->Add(buildSide(tr(L"源 Source"), theme::kPrimary, srcConn_, srcDbc_, srcInfo_),
              1, wxEXPAND | wxALL, 4);

    // centre column — swap button
    auto* mid = new wxBoxSizer(wxVERTICAL);
    mid->AddStretchSpacer(1);
    auto* swap = new wxButton(page, wxID_ANY, L"⇄", wxDefaultPosition, FromDIP(wxSize(40, 30)));
    swap->SetToolTip(tr(L"互换源与目标"));
    swap->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnSwap(); });
    mid->Add(swap, 0, wxALIGN_CENTER_HORIZONTAL);
    mid->AddStretchSpacer(1);
    cols->Add(mid, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);

    cols->Add(buildSide(tr(L"目标 Target"), theme::kTextBody, tgtConn_, tgtDbc_, tgtInfo_),
              1, wxEXPAND | wxALL, 4);

    s->Add(cols, 1, wxEXPAND | wxALL, 16);

    warn_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    warn_->SetFont(Ui(9));
    warn_->SetForegroundColour(theme::kDotAmber);
    s->Add(warn_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);

    // wire change events
    srcConn_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnConnChanged(true); RefreshInfo(); });
    tgtConn_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnConnChanged(false); RefreshInfo(); });
    srcDbc_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { RefreshInfo(); });
    tgtDbc_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { RefreshInfo(); });

    page->SetSizer(s);
    return page;
}

void SyncWizardDialog::FillConnChoice(wxChoice* c)
{
    c->Clear();
    for (ConnEntry* e : conns_) {
        if (!e || !e->IsConnected()) continue;
        c->Append(e->profile.name + L"  (" + db::InfoOf(e->profile.type).name + L")",
                  static_cast<void*>(e));
    }
}

void SyncWizardDialog::OnConnChanged(bool source)
{
    wxChoice* connC = source ? srcConn_ : tgtConn_;
    wxChoice* dbC   = source ? srcDbc_  : tgtDbc_;
    dbC->Clear();
    const int sel = connC->GetSelection();
    if (sel == wxNOT_FOUND) return;
    auto* e = static_cast<ConnEntry*>(connC->GetClientData(sel));
    if (!e || !e->IsConnected()) return;

    std::vector<wxString> dbs;
    wxString err;
    if (e->conn->ListDatabases(dbs, err))
        for (const auto& d : dbs) dbC->Append(d);
    if (dbC->GetCount() > 0) dbC->SetSelection(0);
}

SyncWizardDialog::Side SyncWizardDialog::ReadSide(bool source) const
{
    Side out;
    wxChoice* connC = source ? srcConn_ : tgtConn_;
    wxChoice* dbC   = source ? srcDbc_  : tgtDbc_;
    const int sel = connC->GetSelection();
    if (sel != wxNOT_FOUND)
        out.entry = static_cast<ConnEntry*>(connC->GetClientData(sel));
    out.db = dbC->GetStringSelection();
    return out;
}

void SyncWizardDialog::RefreshInfo()
{
    auto fill = [](wxStaticText** info, const Side& sd) {
        if (!sd.entry || !sd.entry->IsConnected()) {
            for (int i = 0; i < 4; ++i) info[i]->SetLabel(L"—");
            return;
        }
        const auto& p = sd.entry->profile;
        info[0]->SetLabel(db::InfoOf(p.type).name);
        info[1]->SetLabel(p.host.IsEmpty() ? wxString(L"—") : p.host);
        info[2]->SetLabel(p.port > 0 ? wxString::Format(L"%d", p.port) : wxString(L"—"));
        wxString ver = sd.entry->conn->ServerVersion();
        info[3]->SetLabel(ver.IsEmpty() ? wxString(L"—") : ver);
    };
    const Side s = ReadSide(true), t = ReadSide(false);
    fill(srcInfo_, s);
    fill(tgtInfo_, t);

    dirSrc_->SetLabel(s.entry ? (s.entry->profile.name + L" / " + s.db) : tr(L"源"));
    dirTgt_->SetLabel(t.entry ? (t.entry->profile.name + L" / " + t.db) : tr(L"目标"));
    dirTgt_->SetForegroundColour(t.entry ? theme::kPrimary : theme::kTextMuted);

    wxString reason;
    if (!SelectionValid(reason)) warn_->SetLabel(reason);
    else warn_->SetLabel(tr(L"就绪：可进入下一步选择同步内容。"));
    warn_->SetForegroundColour(SelectionValid(reason) ? theme::kGreen : theme::kDotAmber);

    UpdateCrossEngineDataOption();

    if (dirSrc_) dirSrc_->GetParent()->Layout();
    UpdateNextEnabled();
}

// Phase 1 is structure-only for cross-engine pairs (SyncEngine never
// auto-generates cross-engine data DML for a table it can't even guarantee
// exists on the target yet — see SyncEngine::BuildPlan's cross-dialect note).
// Grey the checkbox out rather than hide it, matching FilterPanel's
// "SetToolTip + Enable(false)" pattern for conditionally-disabled controls,
// so the user sees *why* it's unavailable instead of it just disappearing.
// Force-unchecks on entering the cross-engine state so a stale checked value
// from a same-engine selection can't silently ride through to OnNext().
void SyncWizardDialog::UpdateCrossEngineDataOption()
{
    if (!cbData_) return;
    const Side s = ReadSide(true), t = ReadSide(false);
    const bool crossEngine = s.entry && t.entry &&
        s.entry->conn->GetDialect() != t.entry->conn->GetDialect();
    if (crossEngine) {
        cbData_->SetValue(false);
        cbData_->Enable(false);
        cbData_->SetToolTip(tr(L"跨数据库引擎同步本阶段仅支持表结构，暂不支持数据同步。"));
    } else {
        cbData_->Enable(true);
        cbData_->SetToolTip(wxString());
    }
}

bool SyncWizardDialog::SelectionValid(wxString& reason) const
{
    const Side s = ReadSide(true), t = ReadSide(false);
    if (!s.entry || s.db.IsEmpty()) { reason = tr(L"请选择源数据库。"); return false; }
    if (!t.entry || t.db.IsEmpty()) { reason = tr(L"请选择目标连接与数据库。"); return false; }
    if (s.entry == t.entry && s.db == t.db) {
        reason = tr(L"源与目标不能是同一个数据库。");
        return false;
    }
    if (!CrossEngineSupported(s.entry->conn->GetDialect(), t.entry->conn->GetDialect())) {
        reason = tr(L"暂不支持跨数据库引擎同步（MVP 仅支持同引擎与 MySQL↔PostgreSQL）。");
        return false;
    }
    return true;
}

void SyncWizardDialog::OnSwap()
{
    // Locked source can still swap: temporarily unlock, exchange selections.
    const int si = srcConn_->GetSelection(), ti = tgtConn_->GetSelection();
    const wxString sdb = srcDbc_->GetStringSelection(), tdb = tgtDbc_->GetStringSelection();
    srcConn_->Enable(true); srcDbc_->Enable(true);
    if (ti != wxNOT_FOUND) srcConn_->SetSelection(ti);
    if (si != wxNOT_FOUND) tgtConn_->SetSelection(si);
    OnConnChanged(true); OnConnChanged(false);
    srcDbc_->SetStringSelection(tdb);
    tgtDbc_->SetStringSelection(sdb);
    RefreshInfo();
}

// ===========================================================================
// Page 1 — mode & options
// ===========================================================================
wxWindow* SyncWizardDialog::BuildOptionPage()
{
    auto* page = new wxPanel(book_);
    auto* s = new wxBoxSizer(wxVERTICAL);

    auto* t = new wxStaticText(page, wxID_ANY, tr(L"同步内容"));
    t->SetFont(Ui(11, /*bold*/ true));
    t->SetForegroundColour(theme::kPrimary);
    s->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, 20);

    cbStructure_ = new wxCheckBox(page, wxID_ANY, tr(L"表结构（列 / 主键 / 索引 / 外键）"));
    cbData_ = new wxCheckBox(page, wxID_ANY, tr(L"数据（按主键增量：INSERT / UPDATE，需目标含主键）"));
    cbStructure_->SetValue(optStructure_);
    cbData_->SetValue(optData_);
    s->Add(cbStructure_, 0, wxLEFT | wxRIGHT | wxTOP, 20);
    s->Add(cbData_,      0, wxLEFT | wxRIGHT | wxTOP, 10);

    // Compare-ONLY, and the label says so rather than leaving the user to
    // discover it at the review step. Sits here because this is where the user
    // decides what the compare should look at, and it costs two extra catalog
    // queries — but it is NOT a member of db::sync::SyncScope: it is copied
    // into RoutineCompareOptions::enabled, which plan construction never reads.
    cbRoutines_ = new wxCheckBox(page, wxID_ANY,
        tr(L"对比函数与存储过程（仅列出差异供人工查看，不参与同步执行）"));
    cbRoutines_->SetValue(optRoutines_);
    cbRoutines_->SetToolTip(
        tr(L"跨引擎的过程代码无法安全自动转换，因此本工具只并排展示两侧定义，"
           L"不会生成或执行任何例程 DDL。关闭可省去两次目录查询。"));
    s->Add(cbRoutines_,  0, wxLEFT | wxRIGHT | wxTOP, 10);

    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxALL, 20);

    auto* d = new wxStaticText(page, wxID_ANY, tr(L"危险动作策略"));
    d->SetFont(Ui(11, /*bold*/ true));
    d->SetForegroundColour(theme::kDiffDelFg);
    s->Add(d, 0, wxLEFT | wxRIGHT, 20);
    auto* dhint = new wxStaticText(page, wxID_ANY,
        tr(L"当目标存在源没有的表时："));
    dhint->SetFont(Ui(9));
    dhint->SetForegroundColour(theme::kTextSecondary);
    s->Add(dhint, 0, wxLEFT | wxRIGHT | wxTOP, 20);
    rbKeep_ = new wxRadioButton(page, wxID_ANY, tr(L"保留目标多余的表（安全，默认）"),
                                wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    rbDrop_ = new wxRadioButton(page, wxID_ANY, tr(L"删除目标多余的表（危险，需二次确认）"));
    rbKeep_->SetValue(true);
    s->Add(rbKeep_, 0, wxLEFT | wxRIGHT | wxTOP, 20);
    s->Add(rbDrop_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    s->Add(new wxStaticLine(page), 0, wxEXPAND | wxALL, 20);

    cbTxn_ = new wxCheckBox(page, wxID_ANY,
        tr(L"在事务中执行（可回滚方言：出错整体回滚；MySQL DDL 不受事务保护）"));
    cbTxn_->SetValue(true);
    cbDryRun_ = new wxCheckBox(page, wxID_ANY,
        tr(L"试运行（只比对、只预览 SQL，不落库）"));
    s->Add(cbTxn_,    0, wxLEFT | wxRIGHT, 20);
    s->Add(cbDryRun_, 0, wxLEFT | wxRIGHT | wxTOP, 10);

    s->AddStretchSpacer(1);
    page->SetSizer(s);
    return page;
}

// ===========================================================================
// Page 2 — comparing (transient progress, not a numbered step)
// ===========================================================================
wxWindow* SyncWizardDialog::BuildComparingPage()
{
    auto* page = new wxPanel(book_);
    auto* s = new wxBoxSizer(wxVERTICAL);
    s->AddStretchSpacer(1);

    auto* t = new wxStaticText(page, wxID_ANY, tr(L"正在比对源与目标…"));
    t->SetFont(Ui(13, /*bold*/ true));
    t->SetForegroundColour(theme::kTextStrong);
    s->Add(t, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 12);

    cmpGauge_ = new wxGauge(page, wxID_ANY, 100, wxDefaultPosition,
                            FromDIP(wxSize(360, 10)), wxGA_HORIZONTAL | wxGA_SMOOTH);
    s->Add(cmpGauge_, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 12);

    cmpStatus_ = new wxStaticText(page, wxID_ANY, wxEmptyString);
    cmpStatus_->SetFont(Ui(9));
    cmpStatus_->SetForegroundColour(theme::kTextSecondary);
    s->Add(cmpStatus_, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 8);

    s->AddStretchSpacer(1);
    page->SetSizer(s);
    return page;
}

// ===========================================================================
// Page 3 — 差异审阅与 SQL (the single-screen compare page; T8)
//
// This one page is what used to be steps 3 (diff tree) and 4 (SQL preview).
// Merging them is not a cosmetic tidy-up: the requirement is that ticking a
// box and seeing the resulting SQL happen on the SAME screen
// (「选中后对应下面应该显示 SQL 语句是什么」), which two sequential steps
// structurally cannot deliver.
// ===========================================================================
wxWindow* SyncWizardDialog::BuildReviewPage()
{
    comparePage_ = new SyncComparePage(book_);
    comparePage_->OnSelectionChanged = [this]() { UpdateNextEnabled(); };
    return comparePage_;
}

// ===========================================================================
// Nav bar
// ===========================================================================
void SyncWizardDialog::BuildNavBar(wxBoxSizer* root)
{
    auto* bar = new wxBoxSizer(wxHORIZONTAL);

    auto* save = new wxButton(this, wxID_ANY, tr(L"保存 Profile"));
    auto* load = new wxButton(this, wxID_ANY, tr(L"载入 Profile"));
    auto placeholder = [this](wxCommandEvent&) {
        wxMessageBox(tr(L"同步方案（Profile）保存 / 载入即将上线。"),
                     tr(L"数据同步"), wxOK | wxICON_INFORMATION, this);
    };
    save->Bind(wxEVT_BUTTON, placeholder);
    load->Bind(wxEVT_BUTTON, placeholder);
    bar->Add(save, 0, wxRIGHT, 8);
    bar->Add(load, 0);
    bar->AddStretchSpacer(1);

    btnCancel_ = new wxButton(this, wxID_ANY, tr(L"取消"));
    btnCancel_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (comparing_.load()) { CancelCompare(); return; }
        Close();
    });
    btnPrev_ = new wxButton(this, wxID_ANY, tr(L"上一步"));
    btnPrev_->Bind(wxEVT_BUTTON, &SyncWizardDialog::OnPrev, this);
    btnNext_ = new wxButton(this, wxID_ANY, tr(L"下一步"));
    btnNext_->SetBackgroundColour(theme::kPrimary);
    btnNext_->SetForegroundColour(theme::kWhite);
    btnNext_->Bind(wxEVT_BUTTON, &SyncWizardDialog::OnNext, this);
    bar->Add(btnCancel_, 0, wxRIGHT, 10);
    bar->Add(btnPrev_, 0, wxRIGHT, 8);
    bar->Add(btnNext_, 0);

    root->Add(bar, 0, wxEXPAND | wxALL, 14);
}

// ===========================================================================
// Navigation
// ===========================================================================
void SyncWizardDialog::GoTo(int step)
{
    step_ = step;
    book_->SetSelection(step);

    const wchar_t* names[kPageCount] = {
        L"步骤 1 / 3 · 源与目标",
        L"步骤 2 / 3 · 同步内容",
        L"正在比对…",
        L"步骤 3 / 3 · 差异审阅与 SQL"
    };
    stepLbl_->SetLabel(tr(names[step]));

    btnPrev_->Show(step == kPageOptions || step == kPageReview);
    btnNext_->Show(step != kPageComparing);
    if (step == kPageReview) btnNext_->SetLabel(optDryRun_ ? tr(L"试运行") : tr(L"执行"));
    else                     btnNext_->SetLabel(tr(L"下一步 ▸"));

    UpdateNextEnabled();
    if (step == kPageComparing) StartCompare();
    Layout();
}

void SyncWizardDialog::UpdateNextEnabled()
{
    if (!btnNext_) return;
    bool en = true;
    wxString reason;
    if (step_ == kPageSelect) en = SelectionValid(reason);
    else if (step_ == kPageOptions) en = cbStructure_->IsChecked() || cbData_->IsChecked();
    // Gated on the EXECUTION SPEC, not on a widget's idea of "something is
    // ticked": HasAnythingSelected() asks SyncSelection::Build(), so the button
    // is enabled exactly when a run would actually do something. A table whose
    // only tick is a delete while the master switch is off correctly reads as
    // "nothing selected", because that is what would run.
    else if (step_ == kPageReview) en = comparePage_->HasAnythingSelected();
    btnNext_->Enable(en);
}

void SyncWizardDialog::OnNext(wxCommandEvent&)
{
    switch (step_) {
    case kPageSelect: {
        wxString r;
        if (!SelectionValid(r)) return;
        GoTo(kPageOptions);
        break;
    }
    case kPageOptions:
        optStructure_ = cbStructure_->IsChecked();
        optData_      = cbData_->IsChecked();
        optDrop_      = rbDrop_->GetValue();
        optTxn_       = cbTxn_->IsChecked();
        optDryRun_    = cbDryRun_->IsChecked();
        optRoutines_  = cbRoutines_->IsChecked();
        if (!optStructure_ && !optData_) return;
        GoTo(kPageComparing);
        break;
    case kPageReview:
        if (optDryRun_) {
            wxMessageBox(tr(L"试运行完成：右侧「SQL 预览」即为将执行的语句，本次未落库。"),
                         tr(L"数据同步"), wxOK | wxICON_INFORMATION, this);
        } else {
            Execute();
        }
        break;
    default: break;
    }
}

void SyncWizardDialog::OnPrev(wxCommandEvent&)
{
    if (step_ == kPageOptions)     GoTo(kPageSelect);
    else if (step_ == kPageReview) GoTo(kPageOptions);
}

// ===========================================================================
// Compare (worker thread → SyncEngine::BuildPlan)
// ===========================================================================
void SyncWizardDialog::StartCompare()
{
    JoinWorker();
    src_ = ReadSide(true);
    tgt_ = ReadSide(false);
    if (!src_.entry || !tgt_.entry) { GoTo(0); return; }

    cmpGauge_->SetValue(0);
    cmpStatus_->SetLabel(tr(L"准备中…"));
    btnCancel_->SetLabel(tr(L"停止"));
    comparing_ = true;
    stopFlag_ = false;

    db::sync::SyncScope scope;
    scope.structure = optStructure_;
    scope.data = optData_;
    scope.dropMissingTables = optDrop_;

    // Snapshot the UI's live connections BY POINTER — read-only sources for
    // CloneConnection(); the worker never calls anything else on them.
    db::IConnection* srcRaw = src_.entry->conn.get();
    db::IConnection* tgtRaw = tgt_.entry->conn.get();
    const bool wantRoutines = optRoutines_;

    worker_ = core::CrashLog::GuardedThread(L"同步向导",
                                            [this, scope, srcRaw, tgtRaw, wantRoutines]() {
        bool ok = false;
        wxString err;
        db::sync::SyncPlan plan;
        db::sync::RoutineDiffSet routines;
        try {
            // Dedicated connections for the compare — never src_.entry->conn /
            // tgt_.entry->conn: db::IConnection is not thread-safe, and those
            // belong to the UI thread (see docs re: the AiKnowledgeBase heap-
            // corruption incident this mirrors the fix for). Connect latency
            // (incl. re-dialing through an SSH tunnel) happens here, off the UI
            // thread, on purpose.
            std::unique_ptr<db::IConnection> srcClone = db::CloneConnection(*srcRaw, err);
            std::unique_ptr<db::IConnection> tgtClone;
            if (srcClone) tgtClone = db::CloneConnection(*tgtRaw, err);

            if (srcClone && tgtClone) {
                // Publish the clones as CancelCompare()'s live Cancel() targets.
                // clearObservers is declared AFTER the clones, so it is
                // destroyed BEFORE them (reverse declaration order) on every
                // exit path — CancelCompare() can therefore never observe a
                // clone that's mid-destruction.
                {
                    std::lock_guard<std::mutex> lk(cmpConnMx_);
                    cmpSrcConn_ = srcClone.get();
                    cmpTgtConn_ = tgtClone.get();
                }
                struct ObserverGuard {
                    SyncWizardDialog* self;
                    ~ObserverGuard()
                    {
                        std::lock_guard<std::mutex> lk(self->cmpConnMx_);
                        self->cmpSrcConn_ = nullptr;
                        self->cmpTgtConn_ = nullptr;
                    }
                } clearObservers{ this };

                db::sync::SyncEngine eng(*srcClone, *tgtClone, src_.db, tgt_.db);
                auto progress = [this](const wxString& phase, int done, int total) {
                    const wxString ph = phase;
                    CallAfter([this, ph, done, total]() { OnCompareProgress(ph, done, total); });
                };
                ok = eng.BuildPlan(scope, plan, err, stopFlag_, progress);

                // Functions & stored procedures — ANOTHER CATALOG READ on the
                // SAME two cloned connections, in the same worker, under the
                // same stopFlag_ and the same Cancel()-able observers published
                // above. No new connection pattern: a routine read is
                // information_schema/pg_catalog SELECTs, exactly like the
                // schema read the plan just did.
                //
                // Runs only after a SUCCESSFUL plan and only if not cancelled:
                // a user who hit 停止 should not then wait out two more
                // queries, and a failed compare has nothing to attach these to.
                if (ok && wantRoutines && !stopFlag_.load())
                    routines = CompareRoutinesOn(*srcClone, *tgtClone, src_.db, tgt_.db);
            }
            // clearObservers (if constructed) then srcClone/tgtClone unwind here —
            // torn down on THIS (worker) thread, strictly before the CallAfter
            // below hands anything to the UI thread.
        } catch (...) {
            // Guarantee the completion CallAfter still fires even if the above
            // throws (CrashLog::Guard only logs/notifies — it has no notion of
            // "this worker's completion callback", so without this the dialog's
            // Veto()-until-joined close handler could hang forever waiting for
            // comparing_ to go false). Clones already unwound via stack
            // unwinding before this handler runs.
            CallAfter([this]() {
                plan_     = db::sync::SyncPlan{};
                routines_ = db::sync::RoutineDiffSet{};
                OnCompareDone(false, tr(L"比对线程发生未预期异常"));
            });
            throw;   // rethrow so CrashLog::Guard still logs/notifies (defense
                     // in depth) — the CallAfter above already unblocks the
                     // dialog either way.
        }
        // BY VALUE, both of them: the worker's stack is gone by the time the UI
        // thread runs this. RoutineDiffSet holds its pairs by value, so the
        // copy is self-contained — the non-owning pointers ByKind() hands out
        // are derived from the COPY the UI thread owns, never from the worker's.
        CallAfter([this, ok, plan, err, routines]() {
            plan_     = plan;
            routines_ = routines;
            OnCompareDone(ok, err);
        });
    });
}

void SyncWizardDialog::CancelCompare()
{
    if (!comparing_.load()) return;
    stopFlag_ = true;
    {
        std::lock_guard<std::mutex> lk(cmpConnMx_);
        if (cmpSrcConn_) cmpSrcConn_->Cancel();
        if (cmpTgtConn_) cmpTgtConn_->Cancel();
    }
    btnCancel_->Enable(false);
    cmpStatus_->SetLabel(tr(L"正在停止…"));
}

void SyncWizardDialog::OnCompareProgress(const wxString& phase, int done, int total)
{
    if (total > 0) {
        cmpGauge_->SetRange(total);
        cmpGauge_->SetValue(done);
    } else {
        cmpGauge_->Pulse();
    }
    cmpStatus_->SetLabel(total > 0
        ? wxString::Format(L"%s  %d / %d", phase, done, total)
        : phase);
}

void SyncWizardDialog::OnCompareDone(bool ok, const wxString& err)
{
    comparing_ = false;
    btnCancel_->SetLabel(tr(L"取消"));
    btnCancel_->Enable(true);

    if (stopFlag_.load()) { GoTo(kPageOptions); return; }
    if (!ok) {
        wxMessageBox(tr(L"比对失败：") + L"\n\n" + err, tr(L"数据同步"),
                     wxOK | wxICON_ERROR, this);
        GoTo(kPageOptions);
        return;
    }
    // SyncPlan::Empty() only looks at units/preamble/postamble — a cross-engine
    // "table missing, create it manually" Finding can leave units empty (there's
    // no DDL/DML to actually emit) while still carrying something the user needs
    // to see (plan_.warnings). Route through the diff page whenever there's
    // EITHER an actionable unit OR a warning, so a Finding is never silently
    // dropped behind a false "already consistent" message.
    //
    // Routine findings count as "something to show" too, otherwise a run whose
    // ONLY difference is a changed stored procedure would be dismissed with
    // 「目标已与源一致」 — the exact false all-clear this feature exists to stop.
    // An unreadable routine catalog (Comparable() false) also routes through,
    // so 不可见（权限不足） is seen rather than swallowed.
    const bool routineFindings =
        optRoutines_ && (routines_.stat.Differences() > 0 || !routines_.Comparable());

    if (plan_.Empty() && plan_.warnings.empty() && !routineFindings) {
        wxMessageBox(tr(L"目标已与源一致，没有需要同步的差异。"),
                     tr(L"数据同步"), wxOK | wxICON_INFORMATION, this);
        GoTo(kPageOptions);
        return;
    }
    // plan_ is a member and outlives the page, which borrows it — see
    // SyncComparePage.h's ownership note. routines_ is copied INTO the tree by
    // BuildDiffTree, so it is passed by pointer for this call only.
    comparePage_->SetPlan(&plan_, optRoutines_ ? &routines_ : nullptr);
    GoTo(kPageReview);
    // Verification hook; no-op unless SWIFTSQL_SYNCSPEC is set.
    // Defined in SyncWizardHook.cpp.
    RunScriptedSpec();
}

// ===========================================================================
// Execute → SyncRunnerDialog
// ===========================================================================
void SyncWizardDialog::Execute()
{
    // The filtered plan comes from ui::SyncSelection::Build()'s ExecutionSpec,
    // via SyncComparePage. Three properties follow structurally rather than by
    // this function's care:
    //   * tables are resolved by STABLE ID (TableExecSpec::Table()), never by a
    //     row or tree position — the Round 1 bug has no foothold, and
    //     ui::StableId's deleted integral constructors mean it cannot be
    //     reintroduced without a compile error;
    //   * categories the user unchecked are absent, including DELETEs, which
    //     additionally require an unforgeable ui::DeleteGate that
    //     SyncSelection refuses to mint while the master switch is off — and
    //     which is converted, in ui::BuildDataExecPlan and nowhere else, into
    //     the db layer's own db::sync::DeleteAuthorization token. A DELETE the
    //     user did not check therefore has no way to reach the merge: with the
    //     category off, the data layer never even DETECTS the row as a change,
    //     so no RowChange is built and there is no statement to filter later;
    //   * the SQL PREVIEW PANE'S TEXT IS NOT INVOLVED AT ALL. It has no getter
    //     and is capped at 200 statements; execution re-derives from the
    //     structured plan. Reading SQL and running SQL are different paths on
    //     purpose.
    //
    // Both halves come from ONE SyncSelection::Build() per call inside the page,
    // so the DDL plan and the data authorization always describe the same
    // selection.
    db::sync::SyncPlan     fp = comparePage_->BuildExecutionPlan();
    db::sync::DataExecPlan dp = comparePage_->BuildDataExecPlan();

    // Read BEFORE the plans are moved from, and read from the page rather than
    // recomputed from `dp`: DataExecPlan carries the per-table delete
    // AUTHORIZATION, not a row count, so the only place the post-exclusion
    // figure exists is next to the compare-time sample the page owns.
    const long long deleteRows = comparePage_->EffectiveDeleteRows();

    if (fp.units.empty() && dp.Empty()) {
        wxMessageBox(tr(L"请至少勾选一项变更后再执行。"), tr(L"数据同步"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }

    SyncRunnerDialog dlg(this, src_.entry->conn.get(), tgt_.entry->conn.get(),
                         src_.db, tgt_.db, src_.entry->profile.name,
                         tgt_.entry->profile.name, std::move(fp), std::move(dp),
                         optTxn_, deleteRows);
    dlg.ShowModal();
    if (dlg.Succeeded()) {
        didRun_ = true;
        Close();
    }
}

void SyncWizardDialog::JoinWorker()
{
    if (worker_.joinable()) {
        stopFlag_ = true;
        {
            std::lock_guard<std::mutex> lk(cmpConnMx_);
            if (cmpSrcConn_) cmpSrcConn_->Cancel();
            if (cmpTgtConn_) cmpTgtConn_->Cancel();
        }
        worker_.join();
    }
}

} // namespace ui
