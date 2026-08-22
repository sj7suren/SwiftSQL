// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AutomationJobDialog.cpp — see header. Pure presentation over an AutomationJob
// value; the connection candidates are name→databases snapshots so the dialog is
// free of any live db handle.
#include "ui/AutomationJobDialog.h"

#include <wx/wx.h>
#include <wx/statline.h>

#include "ui/I18n.h"

namespace ui {

namespace {

// Type choice items, index-aligned with core::JobType (0..3).
const wchar_t* const kTypeItems[] = {
    L"同步表结构", L"同步数据", L"同步表结构 + 数据", L"导出",
};

// Add a stored connection name to a choice if it is not already present (so an
// offline connection referenced by the job never silently vanishes on edit).
void EnsureName(wxChoice* c, const wxString& name)
{
    if (name.IsEmpty()) return;
    if (c->FindString(name) == wxNOT_FOUND) c->Append(name);
}

} // namespace

AutomationJobDialog::AutomationJobDialog(wxWindow* parent, std::vector<ConnDbs> conns,
                                         const core::AutomationJob& job, bool isNew)
    : CenteredDialog(parent, wxID_ANY,
                     isNew ? tr(L"新建自动化作业") : tr(L"修改自动化作业"),
                     wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , conns_(std::move(conns))
    , job_(job)
    , isNew_(isNew)
{
    BuildUi();
}

bool AutomationJobDialog::IsSyncType() const
{
    const int t = typeCtrl_ ? typeCtrl_->GetSelection() : 0;
    return t == static_cast<int>(core::JobType::SyncStructure) ||
           t == static_cast<int>(core::JobType::SyncData) ||
           t == static_cast<int>(core::JobType::SyncBoth);
}

void AutomationJobDialog::FillDbChoice(wxChoice* dbc, const wxString& connName,
                                       const wxString& keepDb)
{
    dbc->Clear();
    for (const ConnDbs& c : conns_)
        if (c.name == connName)
            for (const wxString& db : c.databases) dbc->Append(db);
    // Keep the stored database even when its connection is offline / unlisted.
    if (!keepDb.IsEmpty() && dbc->FindString(keepDb) == wxNOT_FOUND)
        dbc->Append(keepDb);
    const int sel = keepDb.IsEmpty() ? (dbc->GetCount() ? 0 : wxNOT_FOUND)
                                     : dbc->FindString(keepDb);
    if (sel != wxNOT_FOUND) dbc->SetSelection(sel);
    else if (dbc->GetCount()) dbc->SetSelection(0);
}

void AutomationJobDialog::OnConnChanged(bool source)
{
    if (source) FillDbChoice(srcDb_, srcConn_->GetStringSelection(), wxEmptyString);
    else        FillDbChoice(tgtDb_, tgtConn_->GetStringSelection(), wxEmptyString);
}

void AutomationJobDialog::BuildUi()
{
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxStaticText(this, wxID_ANY,
        isNew_ ? tr(L"新建自动化作业") : tr(L"修改自动化作业"));
    wxFont hf = header->GetFont(); hf.MakeBold(); header->SetFont(hf);
    root->Add(header, 0, wxALL, 12);

    auto* grid = new wxFlexGridSizer(2, wxSize(10, 8));
    grid->AddGrowableCol(1, 1);
    auto addRow = [&](const wxString& label, wxWindow* ctrl) -> wxStaticText* {
        auto* lbl = new wxStaticText(this, wxID_ANY, label);
        grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
        return lbl;
    };

    // --- name ---
    nameCtrl_ = new wxTextCtrl(this, wxID_ANY, job_.name);
    addRow(tr(L"作业名称"), nameCtrl_);

    // --- type ---
    typeCtrl_ = new wxChoice(this, wxID_ANY);
    for (const wchar_t* it : kTypeItems) typeCtrl_->Append(tr(it));
    typeCtrl_->SetSelection(static_cast<int>(job_.type));
    typeCtrl_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { SyncEnabledState(); });
    addRow(tr(L"作业类型"), typeCtrl_);

    // --- source ---
    srcConn_ = new wxChoice(this, wxID_ANY);
    for (const ConnDbs& c : conns_) srcConn_->Append(c.name);
    EnsureName(srcConn_, job_.srcConn);
    { int s = srcConn_->FindString(job_.srcConn);
      if (s == wxNOT_FOUND && srcConn_->GetCount()) s = 0;
      if (s != wxNOT_FOUND) srcConn_->SetSelection(s); }
    srcConn_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnConnChanged(true); });
    addRow(tr(L"源连接"), srcConn_);

    srcDb_ = new wxChoice(this, wxID_ANY);
    FillDbChoice(srcDb_, srcConn_->GetStringSelection(), job_.srcDb);
    addRow(tr(L"源数据库"), srcDb_);

    root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    root->Add(new wxStaticLine(this), 0, wxEXPAND | wxALL, 10);

    // --- target (sync only) ---
    auto* tgrid = new wxFlexGridSizer(2, wxSize(10, 8));
    tgrid->AddGrowableCol(1, 1);
    auto addTRow = [&](const wxString& label, wxWindow* ctrl) -> wxStaticText* {
        auto* lbl = new wxStaticText(this, wxID_ANY, label);
        tgrid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        tgrid->Add(ctrl, 1, wxEXPAND);
        return lbl;
    };

    tgtConn_ = new wxChoice(this, wxID_ANY);
    for (const ConnDbs& c : conns_) tgtConn_->Append(c.name);
    EnsureName(tgtConn_, job_.tgtConn);
    { int s = tgtConn_->FindString(job_.tgtConn);
      if (s == wxNOT_FOUND && tgtConn_->GetCount()) s = 0;
      if (s != wxNOT_FOUND) tgtConn_->SetSelection(s); }
    tgtConn_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnConnChanged(false); });
    tgtLabel_ = addTRow(tr(L"目标连接"), tgtConn_);

    tgtDb_ = new wxChoice(this, wxID_ANY);
    FillDbChoice(tgtDb_, tgtConn_->GetStringSelection(), job_.tgtDb);
    addTRow(tr(L"目标数据库"), tgtDb_);

    root->Add(tgrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // --- options ---
    cbTxn_  = new wxCheckBox(this, wxID_ANY, tr(L"在事务中应用变更（失败回滚）"));
    cbTxn_->SetValue(job_.useTransaction);
    cbDrop_ = new wxCheckBox(this, wxID_ANY, tr(L"删除目标端多余的表"));
    cbDrop_->SetValue(job_.dropMissing);
    cbData_ = new wxCheckBox(this, wxID_ANY, tr(L"导出内容包含数据（否则仅结构）"));
    cbData_->SetValue(job_.withData);
    root->Add(cbTxn_,  0, wxLEFT | wxRIGHT | wxTOP, 12);
    root->Add(cbDrop_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    root->Add(cbData_, 0, wxLEFT | wxRIGHT | wxTOP, 6);

    hint_ = new wxStaticText(this, wxID_ANY,
        tr(L"计划（周期/定时）在列表中通过「设定自动计划」配置。"));
    root->Add(hint_, 0, wxALL, 12);

    if (wxSizer* btns = CreateButtonSizer(wxOK | wxCANCEL))
        root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    Bind(wxEVT_BUTTON, &AutomationJobDialog::OnOk, this, wxID_OK);

    SyncEnabledState();
    SetSizerAndFit(root);
    nameCtrl_->SetFocus();
}

void AutomationJobDialog::SyncEnabledState()
{
    const bool sync = IsSyncType();
    const bool isExport = !sync;
    tgtConn_->Enable(sync);
    tgtDb_->Enable(sync);
    if (tgtLabel_) tgtLabel_->Enable(sync);
    // 事务/删表 只对同步有意义;数据开关只对导出有意义。
    cbTxn_->Enable(sync);
    cbDrop_->Enable(sync);
    cbData_->Enable(isExport);
}

void AutomationJobDialog::OnOk(wxCommandEvent& ev)
{
    const wxString name = nameCtrl_->GetValue().Trim().Trim(false);
    if (name.IsEmpty()) {
        wxMessageBox(tr(L"请填写作业名称。"), tr(L"自动化作业"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    const auto type = static_cast<core::JobType>(typeCtrl_->GetSelection());
    const wxString srcConn = srcConn_->GetStringSelection();
    const wxString srcDb   = srcDb_->GetStringSelection();
    if (srcConn.IsEmpty() || srcDb.IsEmpty()) {
        wxMessageBox(tr(L"请选择源连接与源数据库。"), tr(L"自动化作业"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (IsSyncType()) {
        const wxString tgtConn = tgtConn_->GetStringSelection();
        const wxString tgtDb   = tgtDb_->GetStringSelection();
        if (tgtConn.IsEmpty() || tgtDb.IsEmpty()) {
            wxMessageBox(tr(L"同步作业需要选择目标连接与目标数据库。"),
                         tr(L"自动化作业"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (srcConn == tgtConn && srcDb == tgtDb) {
            wxMessageBox(tr(L"源与目标不能是同一个数据库。"), tr(L"自动化作业"),
                         wxOK | wxICON_WARNING, this);
            return;
        }
        job_.tgtConn = tgtConn;
        job_.tgtDb   = tgtDb;
    } else {
        job_.tgtConn.clear();
        job_.tgtDb.clear();
    }

    job_.name    = name;
    job_.type    = type;
    job_.srcConn = srcConn;
    job_.srcDb   = srcDb;
    job_.dropMissing    = cbDrop_->GetValue();
    job_.useTransaction = cbTxn_->GetValue();
    job_.withData       = cbData_->GetValue();
    ev.Skip();   // close with wxID_OK
}

} // namespace ui
