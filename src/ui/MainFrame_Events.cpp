#include "ui/MainFrame.h"
#include "ui/AboutDialog.h"
#include "ui/AiChatPanel.h"
#include "core/CrashLog.h"
#include "core/AppVersion.h"

#include <wx/wx.h>
#include <wx/aui/auibook.h>
#include <wx/aui/framemanager.h>
#include <wx/aui/dockart.h>
#include <wx/busyinfo.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/grid.h>
#include <wx/imaglist.h>
#include <wx/notebook.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/activityindicator.h>
#include <wx/srchctrl.h>
#include <wx/simplebook.h>
#include <wx/splitter.h>
#include <wx/statline.h>
#include <wx/bmpbuttn.h>
#include <wx/statbmp.h>
#include <wx/accel.h>

#ifdef __WXMSW__
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <uxtheme.h>   // MARGINS
#  include <dwmapi.h>    // DwmExtendFrameIntoClientArea
#  pragma comment(lib, "dwmapi.lib")
#  undef DrawText        // windows.h maps DrawText→DrawTextW; we call wxDC/gc DrawText
#endif
#include <wx/stopwatch.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <vector>
#include <wx/srchctrl.h>
#include <wx/stc/stc.h>
#include <wx/treectrl.h>
#include <wx/generic/treectlg.h>   // connTree_->widget() is a wxGenericTreeCtrl

#include "core/ConnectionStore.h"
#include "core/FavoriteStore.h"
#include "db/SqlScript.h"          // db::SplitSqlScript (dialect-aware statement splitter, R2)
#include "ui/CenteredDialog.h"
#include "ui/ConnectionDialog.h"
#include "ui/ConnectionTree.h"
#include "ui/EditorPage.h"
#include "ui/ObjectTemplates.h"
#include "ui/ExportDialog.h"
#include "ui/ImportDialog.h"
#include "ui/ResultGridPanel.h"
#include "ui/TableDataPage.h"
#include "ui/ErDiagramView.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/PreferencesDialog.h"
#include "ui/TableDesignView.h"
#include "ui/NewTableView.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"
#include "ui/MainFrameInternal.h"

namespace ui {

// ---------------------------------------------------------------------------
// Scriptable startup connect: SWIFTSQL_AUTOCONNECT="type|host|port|user|pass|db"
// (type: mysql|postgresql|oceanbase). Optional SWIFTSQL_AUTORUN="<sql>" runs a
// query once connected. Enables CI / headless verification without UI driving.
void MainFrame::MaybeAutoConnect()
{
    wxString spec;
    if (!wxGetEnv(L"SWIFTSQL_AUTOCONNECT", &spec) || spec.IsEmpty()) return;

    wxArrayString f = wxSplit(spec, '|');
    if (f.size() < 5) return;

    core::ConnectionProfile p;
    const wxString t = f[0].Lower();
    p.type = (t == L"postgresql" || t == L"postgres" || t == L"pg")
                 ? db::DbType::PostgreSQL
             : (t == L"oceanbase" || t == L"ob")
                 ? db::DbType::OceanBase
             : (t == L"kingbase" || t == L"kes")
                 ? db::DbType::KingBase
             : (t == L"dm")
                 ? db::DbType::DM
                 : db::DbType::MySQL;
    p.name = L"AutoConnect";
    p.host = f[1];
    long port = 0; f[2].ToLong(&port); p.port = static_cast<int>(port);
    p.user = f[3];
    p.password = f[4];
    p.database = (f.size() > 5) ? f[5] : wxString();

    // Scriptable SSH tunnel: SWIFTSQL_AUTOSSH="host|port|user|keyfile" (pubkey auth).
    wxString sshEnv;
    if (wxGetEnv(L"SWIFTSQL_AUTOSSH", &sshEnv) && !sshEnv.IsEmpty()) {
        wxArrayString s = wxSplit(sshEnv, '|');
        if (s.size() >= 4) {
            p.sshEnabled = true;
            p.sshHost = s[0];
            long sp = 22; s[1].ToLong(&sp); p.sshPort = static_cast<int>(sp);
            p.sshUser = s[2];
            p.sshAuth = core::SshAuth::PublicKey;
            p.sshKeyFile = s[3];
        }
    }

    ConnEntry* e = connTree_->AutoConnect(p);   // create + connect + activate
    if (!e || !e->IsConnected()) return;

    if (wxGetEnv(L"SWIFTSQL_AUTOSAVE", nullptr))
        core::ConnectionStore::Save(p);

    // Scriptable: select the connection node (verify selected-state icon).
    if (wxGetEnv(L"SWIFTSQL_AUTOSELECT", nullptr))
        connTree_->widget()->SelectItem(e->node);

    // Scriptable: pop the connection node's context menu (verification hook).
    if (wxGetEnv(L"SWIFTSQL_AUTOCTX", nullptr))
        CallAfter([this, e] { connTree_->PopupNodeMenu(e); });

    // Scriptable autocomplete verification: SWIFTSQL_AUTOCOMPLETE="<context>"
    // → write the computed candidate list to a temp file.
    wxString acx;
    if (wxGetEnv(L"SWIFTSQL_AUTOCOMPLETE", &acx) && !acx.IsEmpty()) {
        if (EditorPage* pg = EnsurePage()) {
            const wxString cands = pg->ScriptedComplete(acx);
            wxFile out(wxFileName::GetTempDir() + L"/swiftsql_complete.txt", wxFile::write);
            if (out.IsOpened()) out.Write(cands);
        }
    }
    // Scriptable: set editor text (for highlight screenshots).
    wxString atext;
    if (wxGetEnv(L"SWIFTSQL_AUTOTEXT", &atext) && !atext.IsEmpty())
        if (EditorPage* pg = EnsurePage()) pg->InsertQuery(atext);

    // Scriptable favourites round-trip: "name|sql" → save + dump list to temp.
    wxString afav;
    if (wxGetEnv(L"SWIFTSQL_AUTOFAV", &afav) && !afav.IsEmpty()) {
        wxArrayString ff = wxSplit(afav, '|');
        if (ff.size() >= 2) core::FavoriteStore::Save({ ff[0], ff[1] });
        wxString dump;
        for (const core::Favorite& f : core::FavoriteStore::LoadAll())
            dump += f.name + L" => " + f.sql + L"\n";
        wxFile out(wxFileName::GetTempDir() + L"/swiftsql_fav.txt", wxFile::write);
        if (out.IsOpened()) out.Write(dump);
    }
    // Scriptable transaction test: BEGIN → UPDATE → ROLLBACK (should not persist).
    if (wxGetEnv(L"SWIFTSQL_AUTOTXN", nullptr)) {
        db::IConnection* c = e->conn.get(); db::QueryResult r; wxString er;
        c->Execute(L"BEGIN", r, er);
        c->Execute(L"UPDATE users SET name='TXN_ROLLED_BACK' WHERE id=1", r, er);
        c->Execute(L"ROLLBACK", r, er);
    }
    // Scriptable stop test: run SELECT SLEEP(5) then Cancel after ~1.2s.
    if (wxGetEnv(L"SWIFTSQL_AUTOSTOP", nullptr)) {
        db::IConnection* c = e->conn.get();
        const wxString sleepSql = (c->GetDialect() == db::Dialect::Postgres)
                                      ? L"SELECT pg_sleep(5)" : L"SELECT SLEEP(5)";
        // Join (don't detach): a detached thread capturing the bare IConnection* could
        // outlive the connection and use-after-free it. This is a scripted test path,
        // so blocking here until the cancelled query returns (~1.2s) is fine.
        std::thread runner = core::CrashLog::GuardedThread(L"AUTOSTOP 运行", [c, sleepSql] {
            wxStopWatch sw; db::QueryResult r; wxString er;
            const bool ok = c->Execute(sleepSql, r, er);
            wxFile f(wxFileName::GetTempDir() + L"/swiftsql_stop.txt", wxFile::write);
            if (f.IsOpened()) f.Write(wxString::Format(L"ok=%d ms=%ld err=%s",
                                                       ok ? 1 : 0, sw.Time(), er));
        });
        std::thread canceller = core::CrashLog::GuardedThread(L"AUTOSTOP 取消", [c] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            c->Cancel();
        });
        runner.join();
        canceller.join();
    }
    // Scriptable format verification.
    wxString afmt;
    if (wxGetEnv(L"SWIFTSQL_AUTOFORMAT", &afmt) && !afmt.IsEmpty()) {
        if (EditorPage* pg = EnsurePage()) {
            wxFile out(wxFileName::GetTempDir() + L"/swiftsql_format.txt", wxFile::write);
            if (out.IsOpened()) out.Write(pg->ScriptedFormat(afmt));
        }
    }

    // Scriptable data-edit verification: "table:col:value" — browse the table,
    // edit row 0's col, generate + execute the DML, write it to a temp file.
    //
    // OUTPUT CONTRACT (swiftsql_dml.txt): first line is a verdict token, the
    // rest is the generated SQL (empty unless the verdict is OK). The token
    // vocabulary is gridsql::ScriptedEditToken plus EXEC_ERR:<msg> for "we
    // built statements and the server rejected them". It used to print ERR:
    // for an empty diff, which merged "already equal", "edit refused" and
    // "nothing edited" into one error-shaped line — so a passing run and a
    // silently-refused write looked the same to whoever read the file.
    wxString editSpec;
    if (wxGetEnv(L"SWIFTSQL_AUTOEDIT", &editSpec) && !editSpec.IsEmpty()) {
        wxArrayString f2 = wxSplit(editSpec, ':');
        if (f2.size() >= 3) {
            const wxString sql = L"SELECT * FROM " + f2[0] + L" ORDER BY 1 LIMIT 5";
            db::QueryResult res; wxString err;
            if (e->conn->Execute(sql, res, err)) {
                ui::EditableSpec es = DeriveEditable(e->conn.get(), e->currentDb, sql);
                if (EditorPage* pg = EnsurePage()) {
                    pg->ShowResult(res, e->currentDb, es);
                    const ui::gridsql::ScriptedEditResult r =
                        pg->ScriptedEdit(f2[1], f2[2]);
                    wxString verdict = ui::gridsql::ScriptedEditToken(r);
                    // Only an Ok outcome carries statements, so this is the one
                    // branch where a server can have an opinion.
                    if (r.status == ui::gridsql::ScriptedEditStatus::Ok) {
                        wxString e2;
                        if (!ExecuteDml(r.sql, e2)) verdict = L"EXEC_ERR:" + e2;
                    }
                    wxFile out(wxFileName::GetTempDir() + L"/swiftsql_dml.txt", wxFile::write);
                    if (out.IsOpened()) out.Write(verdict + L"\n" + r.sql);
                }
            }
        }
    }

    // Optional: preselect a table and jump to a view (design/er) for
    // scriptable verification of the structure views.
    wxString table;
    if (wxGetEnv(L"SWIFTSQL_AUTOTABLE", &table) && !table.IsEmpty())
        selTable_ = table;

    wxString wantView;
    if (wxGetEnv(L"SWIFTSQL_AUTOVIEW", &wantView)) {
        wantView.MakeLower();
        ConnEntry* e = connTree_->active();
        if (wantView == L"design") {
            TableDesignView* dv = (e && e->IsConnected() && !selTable_.IsEmpty())
                                ? OpenDesignTab(e, e->currentDb, selTable_) : nullptr;
            // Scripted DDL verification: "name:type:length"
            wxString addcol;
            if (dv && wxGetEnv(L"SWIFTSQL_AUTOADDCOL", &addcol) && !addcol.IsEmpty()) {
                wxArrayString parts = wxSplit(addcol, ':');
                if (parts.size() >= 2) {
                    wxString ddl = dv->ScriptedAddColumn(
                        parts[0], parts[1], parts.size() > 2 ? parts[2] : wxString());
                    wxFile f(wxFileName::GetTempDir() + L"/swiftsql_ddl.txt", wxFile::write);
                    if (f.IsOpened()) f.Write(ddl);
                }
            }
            return;
        }
        if (wantView == L"er") {
            if (e && e->IsConnected() && !e->currentDb.IsEmpty())
                OpenErTab(e, e->currentDb);
            return;
        }
    }

    wxString sql;
    if (wxGetEnv(L"SWIFTSQL_AUTORUN", &sql) && !sql.IsEmpty()) {
        if (EditorPage* page = EnsurePage()) {
            page->InsertQuery(sql);
            wxCommandEvent dummy(wxEVT_BUTTON, ID_RUN_QUERY);
            OnRunQuery(dummy);
        }
    }
}

// Mirror the active data-browser tab's rows/db/SQL into the global bottom status
// bar; clear it (single field) for the editor / any other view. Field 0 stays the
// general message field driven by SetStatusText.
void MainFrame::RefreshBottomBar()
{
    wxStatusBar* sb = GetStatusBar();
    if (!sb) return;
    auto* tdp = (view_ == View::Query && editors_)
              ? dynamic_cast<TableDataPage*>(editors_->GetCurrentPage()) : nullptr;
    if (tdp && tdp->Grid()) {
        wxString rows, db, sql;
        tdp->Grid()->GetStatusInfo(rows, db, sql);
        if (!bottomBarBrowser_) {
            const int widths[4] = { -2, 110, 150, -3 };
            sb->SetFieldsCount(4, widths);
            bottomBarBrowser_ = true;
        }
        sb->SetStatusText(rows, 1);
        sb->SetStatusText(db.IsEmpty() ? wxString(L"—") : db, 2);
        sb->SetStatusText(sql, 3);
    } else if (bottomBarBrowser_) {
        const int one[1] = { -1 };
        sb->SetFieldsCount(1, one);   // back to a single general-message field
        bottomBarBrowser_ = false;
    }
}

void MainFrame::OnNewConnection(wxCommandEvent&)
{
    connTree_->ShowNewConnectionMenu();
}

void MainFrame::OnNewQuery(wxCommandEvent&)
{
    auto* page = new EditorPage(editors_, L"");
    ConfigurePage(page);
    editors_->AddPage(page,
                      wxString::Format(L"query_%zu.sql", editors_->GetPageCount() + 1),
                      true, icons::Stroke(icons::Glyph::Code, 14, theme::kPrimary));
    UpdateQueryView();       // ensure the editor (not the placeholder) is shown
    SwitchView(View::Query); // editors_ 位于查询视图内 —— 不切回来新标签根本看不见
}

void MainFrame::OnAbout(wxCommandEvent&)
{
    ui::AboutDialog dlg(this);
    dlg.ShowModal();
}

void MainFrame::OnQuit(wxCommandEvent&)
{
    Close(true);
}

namespace {

// Push a just-saved AI configuration into every live chat panel. There is one panel
// per SQL editor (created lazily when its AI column is first expanded) plus the AI
// tab, so this walks the window tree instead of keeping a registry that every
// creation and teardown site would have to remember to update.
void SyncAiPanels(wxWindow* w)
{
    if (auto* panel = dynamic_cast<AiChatPanel*>(w)) {
        panel->SyncProviderFromSettings();
        return;                       // no chat panels nest inside another
    }
    for (wxWindow* child : w->GetChildren()) SyncAiPanels(child);
}

} // namespace

void MainFrame::OnPreferences(wxCommandEvent&)
{
    PreferencesDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK) return;
    // The provider may have changed. Every open chat panel caches an ai::AiClient
    // built from the OLD provider, so refresh them now rather than making the user
    // close and reopen the window (each panel also re-checks before its next turn).
    SyncAiPanels(this);
}

// ---------------------------------------------------------------------------
// Favourites
// ---------------------------------------------------------------------------
void MainFrame::OnFavorite(wxCommandEvent&)
{
    EditorPage* page = ActivePage();
    if (!page) return;
    const wxString sql = page->QueryText().Strip(wxString::both);
    if (sql.IsEmpty()) return;

    wxString suggested = sql.BeforeFirst('\n').Left(40).Strip(wxString::both);
    const wxString name = GetTextCentered(this, tr(L"收藏名称:"), tr(L"收藏 SQL"),
                                          suggested);
    if (name.IsEmpty()) return;
    core::FavoriteStore::Save({ name, sql });
    SetStatusText(tr(L"已收藏") + L" — " + name);
}

void MainFrame::OnMenuOpen(wxMenuEvent& ev)
{
    if (ev.GetMenu() == favMenu_) RebuildFavorites();
    if (ev.GetMenu() == toolsMenu_) {
        // Sync needs a live, open database as its source. Grey the items out
        // when nothing is open, rather than letting a dead click open a hint.
        ConnEntry* a = connTree_ ? connTree_->active() : nullptr;
        const bool canSync = a && a->IsConnected() && !a->currentDb.IsEmpty();
        toolsMenu_->Enable(ID_TOOL_SYNC_STRUCT, canSync);
        toolsMenu_->Enable(ID_TOOL_SYNC_DATA,   canSync);
        toolsMenu_->Enable(ID_TOOL_SYNC_BOTH,   canSync);
    }
    ev.Skip();
}

void MainFrame::RebuildFavorites()
{
    if (!favMenu_) return;
    // keep "收藏当前查询" + separator (first two items); rebuild the rest
    while (favMenu_->GetMenuItemCount() > 2)
        favMenu_->Destroy(favMenu_->FindItemByPosition(favMenu_->GetMenuItemCount() - 1));

    const std::vector<core::Favorite> favs = core::FavoriteStore::LoadAll();
    if (favs.empty()) {
        wxMenuItem* mi = favMenu_->Append(wxID_ANY, tr(L"(暂无收藏)"));
        mi->Enable(false);
        return;
    }
    for (const core::Favorite& f : favs) {
        wxMenuItem* mi = favMenu_->Append(wxID_ANY, f.name);
        const wxString sql = f.sql;
        favMenu_->Bind(wxEVT_MENU, [this, sql](wxCommandEvent&) {
            if (EditorPage* p = EnsurePage()) p->InsertQuery(sql);
        }, mi->GetId());
    }
}

} // namespace ui
