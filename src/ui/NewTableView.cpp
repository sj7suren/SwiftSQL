// NewTableView.cpp — see NewTableView.h. Overrides only the create-specific behaviour;
// everything else (the field/index/FK/trigger/option/comment tabs, grids, cells, dirty
// tracking, close guard) is inherited from TableDesignView unchanged.
#include "ui/NewTableView.h"

#include <wx/wx.h>
#include <wx/simplebook.h>
#include <wx/textdlg.h>

#include "ui/I18n.h"

namespace ui {

// Start an empty CREATE-TABLE design: seed one blank field row, hide the ⑦TABLE DDL
// tab (⑥DDL 预览 is the CREATE preview here), and reset every other tab to empty.
void NewTableView::BeginNew(db::IConnection* conn, const wxString& database, db::DbType type)
{
    conn_ = conn; db_ = database; table_ = wxString(); dbType_ = type;
    profile_ = &db::GetDialectProfile(type);

    tabs_[Tab_TableDdl]->Hide();                          // no full-DDL tab when creating
    tabs_[Tab_Triggers]->Show(profile_->SupportsTriggers());
    if (tabs_[Tab_Fields]->GetParent()) tabs_[Tab_Fields]->GetParent()->Layout();

    createCaps_ = db::DbCreateCaps{};
    wxString capsErr; conn->GetCreateDatabaseCaps(createCaps_, capsErr);

    title_->SetLabel(tr(L"新建表"));
    subtitle_->SetLabel(database + L" · " + tr(L"未命名"));

    origCols_.clear();
    PopulateFields();      // empty grid (no origCols_)
    OnAddField();          // seed one blank field row to start
    origComment_.clear();
    origOptions_ = db::TableOptions{};
    if (commentEdit_) commentEdit_->ChangeValue(wxString());
    PopulateIndexes({});
    PopulateForeignKeys({});
    ResetTriggerGrid();
    BuildOptionsForm();
    ApplyCodeKeywords();
    UpdateEditability();
    SelectTab(Tab_Fields);
    Layout();
}

// ⑥DDL 预览: the full CREATE TABLE for the current design (a placeholder name until the
// user saves). No ALTER, and no ⑦TABLE DDL pane.
void NewTableView::RefreshGeneratedTabs()
{
    if (!conn_ || !profile_ || !sqlPreview_) return;
    FlushAttrPanel();

    db::TableModel model;
    BuildTableModel(model);
    if (model.table.IsEmpty()) model.table = L"new_table";   // placeholder until named
    std::vector<wxString> stmts;
    wxString err, text;
    if (profile_->RenderCreate(model, stmts, err))
        for (const wxString& s : stmts) text += s + L";\n";
    SetCodePane(sqlPreview_, text.IsEmpty() ? tr(L"-- 尚无字段，先在“字段”页添加列") : text);
}

// 保存: validate there are columns, prompt for a table name, then CREATE (+ any
// follow-on index/comment/trigger statements from RenderCreate).
void NewTableView::OnSaveActiveTab()
{
    if (!conn_ || !profile_) return;
    FlushAttrPanel();

    db::TableModel model;
    BuildTableModel(model);
    if (model.columns.empty()) {
        wxMessageBox(tr(L"请先在“字段”页添加至少一个字段。"), tr(L"新建表"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }

    wxTextEntryDialog dlg(this, tr(L"请输入表名："), tr(L"新建表"), wxString());
    if (dlg.ShowModal() != wxID_OK) return;
    wxString name = dlg.GetValue(); name.Trim(true).Trim(false);
    if (name.IsEmpty()) {
        wxMessageBox(tr(L"表名不能为空。"), tr(L"新建表"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    model.table = name;

    std::vector<wxString> stmts;
    wxString err;
    if (!profile_->RenderCreate(model, stmts, err) || stmts.empty()) {
        wxMessageBox(err.IsEmpty() ? tr(L"没有可执行的语句") : err, tr(L"新建表"),
                     wxOK | wxICON_ERROR, this);
        return;
    }
    if (wxMessageBox(tr(L"确认创建表 ") + name + L" ？", tr(L"新建表"),
                     wxYES_NO | wxICON_WARNING, this) != wxYES)
        return;

    for (const wxString& s : stmts) {
        db::QueryResult res;
        if (!conn_->Execute(s, res, err)) {
            wxMessageBox(tr(L"创建失败：\n\n") + err + tr(L"\n\n-- 语句：\n") + s, tr(L"新建表"),
                         wxOK | wxICON_ERROR, this);
            return;
        }
    }
    wxMessageBox(tr(L"✓ 表 ") + name + tr(L" 已创建。"), tr(L"新建表"), wxOK | wxICON_INFORMATION, this);
    if (onCreated_) onCreated_(name);          // host refreshes the 表信息 list / tree
    if (onCloseRequest_) onCloseRequest_();    // close this 新建表 tab
}

} // namespace ui
