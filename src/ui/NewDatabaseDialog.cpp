// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// NewDatabaseDialog.cpp — see header. Field set + candidate values come from the
// driver's DbCreateCaps; the driver also assembles the (escaped) SQL, so this
// dialog is pure presentation + linkage.
#include "ui/NewDatabaseDialog.h"

#include <wx/wx.h>
#include <wx/combobox.h>
#include <wx/spinctrl.h>

#include "ui/I18n.h"

namespace ui {

namespace {
// Localized "(default)" sentinel shown as the first item of every optional
// Choice; mapped back to an empty value (option omitted) in CurrentRequest.
inline wxString DefaultItem() { return tr(L"(默认)"); }
} // namespace

NewDatabaseDialog::NewDatabaseDialog(wxWindow* parent, db::IConnection* conn,
                                     const wxString& engineName)
    : CenteredDialog(parent, wxID_ANY, tr(L"新建数据库"), wxDefaultPosition,
                     wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , conn_(conn)
{
    wxString err;
    if (conn_) {
        // The driver runs several metadata queries here (roles / encodings /
        // collations / templates / tablespaces on PostgreSQL) synchronously on the
        // GUI thread, so the dialog can take a moment to appear on a busy or remote
        // server — show the loading cursor while it prepares.
        wxBusyCursor busy;
        conn_->GetCreateDatabaseCaps(caps_, err);   // best-effort; empty on error
    }
    BuildUi(engineName);
    UpdatePreview();
}

wxString NewDatabaseDialog::DatabaseName() const
{
    return nameCtrl_ ? nameCtrl_->GetValue().Trim().Trim(false) : wxString();
}

void NewDatabaseDialog::BuildUi(const wxString& engineName)
{
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxStaticText(this, wxID_ANY,
        tr(L"新建数据库") + L" — " + engineName);
    wxFont hf = header->GetFont(); hf.MakeBold(); header->SetFont(hf);
    root->Add(header, 0, wxALL, 12);

    auto* grid = new wxFlexGridSizer(2, wxSize(10, 8));
    grid->AddGrowableCol(1, 1);

    auto addRow = [&](const wxString& label, wxWindow* ctrl) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0,
                  wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
    };

    // --- name (always) ---
    nameCtrl_ = new wxTextCtrl(this, wxID_ANY);
    nameCtrl_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdatePreview(); });
    addRow(tr(L"数据库名称"), nameCtrl_);

    // --- engine options (data-driven) ---
    for (const db::DbCreateOption& opt : caps_.options) {
        wxWindow* ctrl = nullptr;
        switch (opt.kind) {
        case db::DbCreateOption::Kind::Int: {
            auto* sp = new wxSpinCtrl(this, wxID_ANY);
            sp->SetRange(-1, 1000000);
            long v = 0; sp->SetValue(opt.defaultVal.ToLong(&v) ? static_cast<int>(v) : -1);
            sp->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { UpdatePreview(); });
            ctrl = sp;
            break;
        }
        case db::DbCreateOption::Kind::FreeText: {
            auto* cb = new wxComboBox(this, wxID_ANY, opt.defaultVal);
            for (const wxString& c : opt.choices) cb->Append(c);
            cb->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdatePreview(); });
            cb->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { UpdatePreview(); });
            ctrl = cb;
            break;
        }
        case db::DbCreateOption::Kind::Choice:
        default: {
            auto* ch = new wxChoice(this, wxID_ANY);
            ch->Append(DefaultItem());                 // index 0 = omit
            if (opt.groupSource.IsEmpty()) {           // ungrouped: all choices
                for (const wxString& c : opt.choices) ch->Append(c);
                int sel = opt.defaultVal.IsEmpty() ? wxNOT_FOUND
                                                   : ch->FindString(opt.defaultVal);
                ch->SetSelection(sel == wxNOT_FOUND ? 0 : sel);
            } else {
                ch->SetSelection(0);                   // filled by Refilter()
            }
            ch->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
                Refilter(); UpdatePreview();
            });
            ctrl = ch;
            break;
        }
        }
        fields_.push_back({opt, ctrl});
        addRow(tr(opt.label.wc_str()), ctrl);
    }

    root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    if (caps_.options.empty()) {
        root->Add(new wxStaticText(this, wxID_ANY,
            tr(L"该引擎暂不支持高级建库选项。")), 0, wxALL, 12);
    }

    // --- SQL preview ---
    root->Add(new wxStaticText(this, wxID_ANY, tr(L"SQL 预览")), 0,
              wxLEFT | wxRIGHT | wxTOP, 12);
    preview_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxSize(460, 72), wxTE_MULTILINE | wxTE_READONLY);
    root->Add(preview_, 0, wxEXPAND | wxALL, 12);

    // --- buttons ---
    if (wxSizer* btns = CreateButtonSizer(wxOK | wxCANCEL))
        root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    Bind(wxEVT_BUTTON, &NewDatabaseDialog::OnOk, this, wxID_OK);

    Refilter();   // seed grouped choices from the initial group selection
    SetSizerAndFit(root);
    if (nameCtrl_) nameCtrl_->SetFocus();
}

// Rebuild any grouped Choice (collation) from its source Choice's value (charset).
void NewDatabaseDialog::Refilter()
{
    for (Field& f : fields_) {
        if (f.opt.groupSource.IsEmpty() ||
            f.opt.kind != db::DbCreateOption::Kind::Choice)
            continue;

        // current value of the controlling option
        wxString group;
        for (const Field& src : fields_)
            if (src.opt.id == f.opt.groupSource &&
                src.opt.kind == db::DbCreateOption::Kind::Choice) {
                const wxString s = static_cast<wxChoice*>(src.ctrl)->GetStringSelection();
                if (s != DefaultItem()) group = s;
            }

        auto* ch = static_cast<wxChoice*>(f.ctrl);
        const wxString keep = ch->GetStringSelection();
        ch->Clear();
        ch->Append(DefaultItem());
        for (size_t i = 0; i < f.opt.choices.size(); ++i) {
            const bool match = group.IsEmpty() ||
                (i < f.opt.choiceGroup.size() && f.opt.choiceGroup[i] == group);
            if (match) ch->Append(f.opt.choices[i]);
        }
        const int sel = ch->FindString(keep);
        ch->SetSelection(sel == wxNOT_FOUND ? 0 : sel);
    }
}

db::DbCreateRequest NewDatabaseDialog::CurrentRequest() const
{
    db::DbCreateRequest req;
    req.name = DatabaseName();
    for (const Field& f : fields_) {
        wxString v;
        switch (f.opt.kind) {
        case db::DbCreateOption::Kind::Int:
            v = wxString::Format(L"%d", static_cast<wxSpinCtrl*>(f.ctrl)->GetValue());
            break;
        case db::DbCreateOption::Kind::FreeText:
            v = static_cast<wxComboBox*>(f.ctrl)->GetValue().Trim().Trim(false);
            break;
        case db::DbCreateOption::Kind::Choice:
        default: {
            const wxString s = static_cast<wxChoice*>(f.ctrl)->GetStringSelection();
            v = (s == DefaultItem()) ? wxString() : s;   // sentinel → omit
            break;
        }
        }
        req.values.emplace_back(f.opt.id, v);
    }
    return req;
}

void NewDatabaseDialog::UpdatePreview()
{
    const bool haveName = !DatabaseName().IsEmpty();
    if (wxWindow* ok = FindWindow(wxID_OK)) ok->Enable(haveName);
    if (!preview_) return;
    preview_->SetValue(haveName && conn_
                       ? conn_->BuildCreateDatabaseSql(CurrentRequest())
                       : wxString(tr(L"-- 请输入数据库名称")));
}

void NewDatabaseDialog::OnOk(wxCommandEvent& ev)
{
    if (DatabaseName().IsEmpty() || !conn_) return;   // OK is disabled anyway
    sql_ = conn_->BuildCreateDatabaseSql(CurrentRequest());
    ev.Skip();   // let the default handler close with wxID_OK
}

} // namespace ui
