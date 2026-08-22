// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AutomationScheduleDialog.h — configure the in-app schedule of one automation
// job: either periodic ("every N minutes/hours") or a daily fixed time ("每天
// HH:MM"). The dialog edits only the schedule fields of the AutomationJob it is
// handed (schedule / intervalMinutes / dailyHour / dailyMinute); everything else
// is left untouched. OS-level scheduled tasks (running with the app closed) are a
// documented phase-2 item and are NOT offered here.
#pragma once

#include "core/AutomationStore.h"
#include "ui/CenteredDialog.h"

class wxRadioButton;
class wxSpinCtrl;
class wxChoice;

namespace ui {

class AutomationScheduleDialog : public CenteredDialog {
public:
    AutomationScheduleDialog(wxWindow* parent, const core::AutomationJob& job);

    // The job with its schedule fields updated (valid after ShowModal()==wxID_OK).
    const core::AutomationJob& Job() const { return job_; }

private:
    void BuildUi();
    void SyncEnabledState();
    void OnOk(wxCommandEvent&);

    core::AutomationJob job_;

    wxRadioButton* rbEvery_ = nullptr;
    wxRadioButton* rbDaily_ = nullptr;
    wxSpinCtrl*    everyVal_ = nullptr;
    wxChoice*      everyUnit_ = nullptr;   // 0=分钟 1=小时
    wxSpinCtrl*    hourCtrl_ = nullptr;
    wxSpinCtrl*    minCtrl_  = nullptr;
};

} // namespace ui
