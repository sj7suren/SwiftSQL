// AutomationScheduleDialog.cpp — see header.
#include "ui/AutomationScheduleDialog.h"

#include <wx/wx.h>
#include <wx/radiobut.h>
#include <wx/spinctrl.h>

#include "ui/I18n.h"

namespace ui {

AutomationScheduleDialog::AutomationScheduleDialog(wxWindow* parent,
                                                   const core::AutomationJob& job)
    : CenteredDialog(parent, wxID_ANY, tr(L"设定自动计划"), wxDefaultPosition,
                     wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , job_(job)
{
    BuildUi();
}

void AutomationScheduleDialog::BuildUi()
{
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxStaticText(this, wxID_ANY,
        tr(L"自动计划") + L" — " + job_.name);
    wxFont hf = header->GetFont(); hf.MakeBold(); header->SetFont(hf);
    root->Add(header, 0, wxALL, 12);

    root->Add(new wxStaticText(this, wxID_ANY,
        tr(L"应用运行时按此计划自动执行本作业（关闭应用则不运行）。")),
        0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    // ---- periodic ----
    rbEvery_ = new wxRadioButton(this, wxID_ANY, tr(L"周期性 — 每"),
                                 wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    auto* everyRow = new wxBoxSizer(wxHORIZONTAL);
    everyRow->Add(rbEvery_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    everyVal_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize(80, -1));
    everyVal_->SetRange(1, 10000);
    everyRow->Add(everyVal_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    everyUnit_ = new wxChoice(this, wxID_ANY);
    everyUnit_->Append(tr(L"分钟"));
    everyUnit_->Append(tr(L"小时"));
    everyUnit_->SetSelection(0);
    everyRow->Add(everyUnit_, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(everyRow, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- daily ----
    rbDaily_ = new wxRadioButton(this, wxID_ANY, tr(L"定时 — 每天"));
    auto* dailyRow = new wxBoxSizer(wxHORIZONTAL);
    dailyRow->Add(rbDaily_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    hourCtrl_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize(70, -1));
    hourCtrl_->SetRange(0, 23);
    dailyRow->Add(hourCtrl_, 0, wxALIGN_CENTER_VERTICAL);
    dailyRow->Add(new wxStaticText(this, wxID_ANY, L" : "), 0,
                  wxALIGN_CENTER_VERTICAL);
    minCtrl_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxSize(70, -1));
    minCtrl_->SetRange(0, 59);
    dailyRow->Add(minCtrl_, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(dailyRow, 0, wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- seed from the job ----
    if (job_.schedule == core::ScheduleKind::DailyAt) {
        rbDaily_->SetValue(true);
    } else {
        rbEvery_->SetValue(true);
    }
    // periodic value/unit: prefer whole hours when evenly divisible.
    int mins = job_.intervalMinutes > 0 ? job_.intervalMinutes : 60;
    if (mins % 60 == 0 && mins >= 60) { everyUnit_->SetSelection(1); everyVal_->SetValue(mins / 60); }
    else { everyUnit_->SetSelection(0); everyVal_->SetValue(mins); }
    hourCtrl_->SetValue(job_.dailyHour >= 0 && job_.dailyHour < 24 ? job_.dailyHour : 3);
    minCtrl_->SetValue(job_.dailyMinute >= 0 && job_.dailyMinute < 60 ? job_.dailyMinute : 0);

    rbEvery_->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { SyncEnabledState(); });
    rbDaily_->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { SyncEnabledState(); });

    if (wxSizer* btns = CreateButtonSizer(wxOK | wxCANCEL))
        root->Add(btns, 0, wxEXPAND | wxALL, 12);
    Bind(wxEVT_BUTTON, &AutomationScheduleDialog::OnOk, this, wxID_OK);

    SyncEnabledState();
    SetSizerAndFit(root);
}

void AutomationScheduleDialog::SyncEnabledState()
{
    const bool every = rbEvery_->GetValue();
    everyVal_->Enable(every);
    everyUnit_->Enable(every);
    hourCtrl_->Enable(!every);
    minCtrl_->Enable(!every);
}

void AutomationScheduleDialog::OnOk(wxCommandEvent& ev)
{
    if (rbDaily_->GetValue()) {
        job_.schedule    = core::ScheduleKind::DailyAt;
        job_.dailyHour   = hourCtrl_->GetValue();
        job_.dailyMinute = minCtrl_->GetValue();
    } else {
        job_.schedule = core::ScheduleKind::EveryMinutes;
        const int v = everyVal_->GetValue();
        job_.intervalMinutes = (everyUnit_->GetSelection() == 1) ? v * 60 : v;
        if (job_.intervalMinutes < 1) job_.intervalMinutes = 1;
    }
    ev.Skip();
}

} // namespace ui
