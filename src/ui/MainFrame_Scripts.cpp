// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MainFrame_Scripts.cpp — the saved-SQL script library wiring: the reusable
// library tab (a ScriptLibraryPanel in editors_), the Ctrl+S save flow for plain
// SQL editors (SaveEditorScript), and opening a stored script back into a fresh
// editor tab. Kept in its own TU per the 1000-line file charter.
#include "ui/MainFrame.h"

#include <wx/wx.h>
#include <wx/aui/auibook.h>

#include "core/ScriptStore.h"
#include "ui/CenteredDialog.h"
#include "ui/ConnectionTree.h"
#include "ui/EditorPage.h"
#include "ui/ScriptLibraryPanel.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/MainFrameInternal.h"

namespace ui {

// Lazily create the single, reusable script-library tab (dedupe-focus it if it is
// already open). Closable — cleared to null in the editors_ PAGE_CLOSE handler, so
// a stale pointer is never reused.
wxWindow* MainFrame::EnsureScriptLibraryTab()
{
    if (scriptLibTab_ && editors_->GetPageIndex(scriptLibTab_) != wxNOT_FOUND)
        return scriptLibTab_;

    auto* panel = new ScriptLibraryPanel(editors_);
    panel->SetOnOpen([this](const wxString& name) { OpenScriptInEditor(name); });
    panel->SetOnDelete([this](const wxString& name) { core::ScriptStore::Remove(name); });
    scriptLib_    = panel;
    scriptLibTab_ = panel;

    editors_->AddPage(panel, tr(L"SQL脚本"), /*select*/ true,
                      icons::Stroke(icons::Glyph::Save, 14, theme::kPrimary));
    UpdateQueryView();       // there is a page now → show editors_, not the placeholder
    return scriptLibTab_;
}

// Top-bar entry: open the library tab (creating it once), refresh it, bring it to
// front, and make sure the query view (which hosts editors_) is showing.
void MainFrame::OpenScriptLibrary()
{
    EnsureScriptLibraryTab();
    if (scriptLib_) scriptLib_->Reload();
    const int idx = editors_->GetPageIndex(scriptLibTab_);
    if (idx != wxNOT_FOUND) editors_->SetSelection(idx);
    SwitchView(View::Query);
}

// Reload the library grid if the tab is currently open (called after a save/delete).
void MainFrame::RefreshScriptLibrary()
{
    if (scriptLib_ && scriptLibTab_ &&
        editors_->GetPageIndex(scriptLibTab_) != wxNOT_FOUND)
        scriptLib_->Reload();
}

// Read a stored script and open it in a brand-new plain SQL editor tab named after
// the script; the tab remembers its saved name so the next Ctrl+S overwrites.
void MainFrame::OpenScriptInEditor(const wxString& name)
{
    wxString sql, err;
    if (!core::ScriptStore::Load(name, sql, err)) {
        wxMessageBox(err, tr(L"打开脚本"), wxOK | wxICON_WARNING, this);
        return;
    }
    auto* page = new EditorPage(editors_, sql);
    ConfigurePage(page);
    editors_->AddPage(page, name, /*select*/ true,
                      icons::Stroke(icons::Glyph::Code, 14, theme::kPrimary));
    UpdateQueryView();
    SwitchView(View::Query);
    // Prime the editor's saved-name so Ctrl+S overwrites instead of re-prompting.
    // (Re-fire the hook path isn't needed; the editor learns the name on first save,
    // but opening from the library means it is already named — see below.)
    page->PrimeSavedScriptName(name);
}

// Ctrl+S handler for a plain SQL editor (wired in ConfigurePage). Prompts for a
// name on the first save, then writes the buffer via ScriptStore with the active
// connection user as both owner and execution user, refreshes the library, and
// renames the tab. Returns the name saved under ("" if the user cancelled).
wxString MainFrame::SaveEditorScript(EditorPage* page, const wxString& fullSql,
                                     const wxString& curName)
{
    wxString name = curName;
    if (name.IsEmpty()) {
        // Suggest the first non-empty line (trimmed) as the default name.
        wxString suggested = fullSql.BeforeFirst('\n').Left(40).Strip(wxString::both);
        name = GetTextCentered(this, tr(L"脚本名称:"), tr(L"保存 SQL 脚本"), suggested);
        if (name.IsEmpty()) return wxEmptyString;   // cancelled
        // Overwrite confirm only when saving a *new* script over an existing name.
        if (core::ScriptStore::Exists(name) &&
            wxMessageBox(wxString::Format(tr(L"脚本「%s」已存在，覆盖它吗?"), name),
                         tr(L"保存 SQL 脚本"), wxYES_NO | wxICON_QUESTION, this) != wxYES)
            return wxEmptyString;
    }

    // Owner + execution user = the active connection's user (empty if none).
    wxString user;
    if (ConnEntry* a = connTree_ ? connTree_->active() : nullptr)
        user = a->profile.user;

    wxString err;
    if (!core::ScriptStore::Save(name, fullSql, user, user, err)) {
        wxMessageBox(err, tr(L"保存 SQL 脚本"), wxOK | wxICON_ERROR, this);
        return wxEmptyString;
    }

    // Rename the tab to the script name and refresh the library grid if it's open.
    if (page) {
        const int idx = editors_->GetPageIndex(page);
        if (idx != wxNOT_FOUND) editors_->SetPageText(idx, name);
    }
    RefreshScriptLibrary();
    SetStatusText(wxString::Format(tr(L"已保存脚本: %s"), name));
    return name;
}

} // namespace ui
