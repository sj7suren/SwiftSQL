// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// EditorPageObject.cpp — second translation unit for ui::EditorPage: everything
// that turns a plain SQL tab into an OBJECT EDITOR (新建/修改 视图·函数·存储过程·包).
//
// Three things live here, and they are one subsystem because they share the same
// lazily-built state and the same question — "which connection and database does
// this tab act on?":
//
//   * the top 连接/数据库 selector bar, built on demand and populated entirely
//     from MainFrame-supplied callbacks so EditorPage never learns what a
//     connection tree is (handles stay opaque void*, never dereferenced here);
//   * CommitRunTarget, which pushes that selection to the run target and is the
//     ONE place a dead/stale connection is caught before a statement is sent;
//   * the bottom result strip (green 保存成功 / red error), which is what an
//     object editor shows INSTEAD of the results grid — its DDL returns no rows,
//     so the editor keeps the whole tab.
//
// Plus RunCurrentRoutine (F10), which is just those three used together.
//
// This is a WIDGET-LEVEL seam, the same kind as ResultGridPanelChrome.cpp: none
// of it is headlessly testable, which is precisely why it is isolated — keeping
// untestable construction and wx event wiring out of the TU that holds the
// editor's behaviour. The logic seams for this file's siblings are
// EditorSqlFormat.{h,cpp} and EditorCompletion.{h,cpp}, which are widget-free
// and unit-tested.
#include "ui/EditorPage.h"

#include <wx/wx.h>
#include <wx/statbmp.h>
#include <wx/stc/stc.h>
#include <algorithm>

#include "ui/I18n.h"           // tr()
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

// ---------------------------------------------------------------------------
// Object-editor bottom result strip.
// ---------------------------------------------------------------------------
void EditorPage::BuildObjStatusBar()
{
    if (objStatusBar_) return;
    auto* bar = new wxPanel(this);
    auto* h = new wxBoxSizer(wxHORIZONTAL);
    objStatusIcon_ = new wxStaticBitmap(bar, wxID_ANY, wxNullBitmap);
    h->Add(objStatusIcon_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    objStatusText_ = new wxStaticText(bar, wxID_ANY, wxEmptyString);
    objStatusText_->SetFont(Ui(10));
    h->Add(objStatusText_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
    bar->SetSizer(h);
    bar->SetMinSize(wxSize(-1, 30));
    objStatusBar_ = bar;
    // Below the editor/results split, at the very bottom of the tab.
    if (auto* s = GetSizer()) {
        s->Add(objStatusBar_, 0, wxEXPAND);
        objStatusBar_->Hide();
        s->Layout();
    }
}

void EditorPage::ShowObjectSaveStatus(bool ok, const wxString& msg)
{
    if (!objectEditor_) return;
    BuildObjStatusBar();
    const wxColour bg = ok ? theme::kDiffAddedBg : theme::kDiffDelBg;
    const wxColour fg = ok ? theme::kDiffAddedFg : theme::kDiffDelFg;
    objStatusBar_->SetBackgroundColour(bg);
    objStatusIcon_->SetBitmap(icons::Stroke(ok ? icons::Glyph::Commit : icons::Glyph::Close,
                                            16, fg, 1.9));
    objStatusText_->SetForegroundColour(fg);
    objStatusText_->SetLabel(ok ? tr(L"保存成功") : msg);
    objStatusText_->Wrap(std::max(80, GetClientSize().x - 60));
    objStatusBar_->Show();
    if (auto* s = GetSizer()) s->Layout();
}

// ---------------------------------------------------------------------------
// Object-editor top selector bar (连接 + 数据库).
// ---------------------------------------------------------------------------
void EditorPage::BuildObjToolbar()
{
    if (objToolbar_) return;
    auto* bar = new wxPanel(this);
    bar->SetBackgroundColour(theme::kChromeBg);
    auto* h = new wxBoxSizer(wxHORIZONTAL);

    auto* cl = new wxStaticText(bar, wxID_ANY, tr(L"连接:"));
    cl->SetForegroundColour(theme::kTextSecondary);
    h->Add(cl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
    connChoice_ = new ui::BoxDropdown(bar, wxSize(180, -1));
    h->Add(connChoice_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

    auto* dl = new wxStaticText(bar, wxID_ANY, tr(L"数据库:"));
    dl->SetForegroundColour(theme::kTextSecondary);
    h->Add(dl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 14);
    dbChoice_ = new ui::BoxDropdown(bar, wxSize(160, -1));
    h->Add(dbChoice_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

    h->AddStretchSpacer(1);
    bar->SetSizer(h);
    bar->SetMinSize(wxSize(-1, 34));
    objToolbar_ = bar;

    // Cascade: a new connection reloads its database list (default db no longer
    // applies on a manual switch → prefer nothing, i.e. the first db).
    connChoice_->SetOnChange([this] { PopulateDbChoice(wxEmptyString); });

    // Sits at the very top of the tab, above the editor/results split.
    if (auto* s = GetSizer()) { s->Insert(0, objToolbar_, 0, wxEXPAND); s->Layout(); }
}

void EditorPage::PopulateConnChoice()
{
    if (!connChoice_ || !objTarget_.connections) return;
    connChoice_->Clear();
    int sel = wxNOT_FOUND;
    for (const auto& c : objTarget_.connections()) {
        const int i = connChoice_->Append(c.first, c.second);   // (label, opaque handle)
        if (c.second == objDefConn_) sel = i;
    }
    if (sel == wxNOT_FOUND && connChoice_->GetCount() > 0) sel = 0;
    if (sel != wxNOT_FOUND) connChoice_->SetSelection(sel);
    PopulateDbChoice(objDefDb_);
}

void EditorPage::PopulateDbChoice(const wxString& prefer)
{
    if (!dbChoice_ || !objTarget_.databases) return;
    dbChoice_->Clear();
    const int ci = connChoice_ ? connChoice_->GetSelection() : wxNOT_FOUND;
    void* h = (ci != wxNOT_FOUND) ? connChoice_->GetClientData(ci) : nullptr;
    int sel = wxNOT_FOUND;
    if (h) {
        for (const wxString& d : objTarget_.databases(h)) {
            const int i = dbChoice_->Append(d);
            if (!prefer.IsEmpty() && d.IsSameAs(prefer, false)) sel = i;
        }
    }
    if (sel == wxNOT_FOUND && dbChoice_->GetCount() > 0) sel = 0;
    if (sel != wxNOT_FOUND) dbChoice_->SetSelection(sel);
}

void EditorPage::SetObjectTarget(ObjectTarget src, void* defConn, const wxString& defDb)
{
    // Every editor tab carries the 连接/数据库 selector now — plain SQL editors
    // (default = the connection/db active when the tab was created) as well as
    // object editors (default = the object's own connection/db, set a second time
    // right after ConfigurePage). Repeatable: re-sets the target + defaults and
    // re-selects; BuildObjToolbar is idempotent.
    objTarget_  = std::move(src);
    objDefConn_ = defConn;
    objDefDb_   = defDb;
    BuildObjToolbar();
    PopulateConnChoice();
}

bool EditorPage::CommitRunTarget(wxString& err)
{
    if (!connChoice_ || !objTarget_.setTarget) return true;   // no selector → no-op
    const int ci = connChoice_->GetSelection();
    if (ci == wxNOT_FOUND) { err = tr(L"请选择连接"); return false; }
    void* h = connChoice_->GetClientData(ci);
    wxString db;
    if (dbChoice_) {
        const int di = dbChoice_->GetSelection();
        if (di != wxNOT_FOUND) db = dbChoice_->GetString(di);
    }
    return objTarget_.setTarget(h, db, err);
}

// ---------------------------------------------------------------------------
// F10 — run the 函数/存储过程 currently in the buffer, with a parameter dialog.
// ---------------------------------------------------------------------------
// Determines the object from the *current buffer* (name via ParseObjectName —
// the user may type/rename it in a 新建 template) and its kind from what the
// editor was opened as, switches the run target to the top selector's
// connection+db (reusing CommitRunTarget's liveness check + USE), then hands off
// to the MainFrame-supplied hook which shows RoutineExecDialog and runs.
bool EditorPage::RunCurrentRoutine()
{
    if (!objectEditor_) return false;   // plain query tab → let F10 propagate
    // Only stored procedures / functions take parameters. Views have none and
    // packages have no execute entry today → consume the key with no action.
    if (objKind_ != ObjectKind::Procedure && objKind_ != ObjectKind::Function)
        return true;
    if (!runRoutine_) return true;

    const wxString name = ParseObjectName(stc_->GetText());
    if (name.IsEmpty()) return true;    // nothing identifiable in the buffer yet

    // Validate + switch the run target; a dead connection fills err (no crash),
    // and setTarget issues USE <db> so the CALL resolves in the chosen database.
    wxString err;
    if (!CommitRunTarget(err)) {
        wxMessageBox(err, tr(L"执行"), wxOK | wxICON_WARNING,
                     wxGetTopLevelParent(this));
        return true;
    }

    // The connection + database chosen in the top selector — never the tree active.
    void* conn = objDefConn_;
    wxString db = objDefDb_;
    if (connChoice_) {
        const int ci = connChoice_->GetSelection();
        if (ci != wxNOT_FOUND) conn = connChoice_->GetClientData(ci);
        if (dbChoice_) {
            const int di = dbChoice_->GetSelection();
            if (di != wxNOT_FOUND) db = dbChoice_->GetString(di);
        }
    }
    if (conn) runRoutine_(conn, db, objKind_, name);
    return true;
}

} // namespace ui
