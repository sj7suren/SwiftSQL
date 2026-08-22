// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AutomationJobDialog.h — create / edit one automation job (name, type, source
// connection+database, target connection+database for sync jobs, and per-type
// options). Connection candidates are supplied as name→databases snapshots by
// MainFrame (gathered from the live connection tree at open time), so the dialog
// never touches a db::IConnection or a ConnEntry — it only edits an
// core::AutomationJob value that the caller persists via AutomationStore.
//
// Scheduling is NOT edited here (it has its own AutomationScheduleDialog reached
// from the panel's 设定自动计划 action); this dialog leaves the schedule fields of
// the job untouched.
#pragma once

#include <wx/string.h>
#include <vector>
#include "core/AutomationStore.h"
#include "ui/CenteredDialog.h"

class wxTextCtrl;
class wxChoice;
class wxCheckBox;
class wxStaticText;
class wxStaticBoxSizer;

namespace ui {

class AutomationJobDialog : public CenteredDialog {
public:
    // One connection candidate: its name plus the databases it currently exposes.
    struct ConnDbs {
        wxString              name;
        std::vector<wxString> databases;
    };

    // `conns` are the connection candidates. `job` seeds the fields (for edit); pass
    // a default-constructed job for 新建. `isNew` gates name-uniqueness prompting in
    // the owner. If the job references a connection not present in `conns` (e.g. it
    // is offline), that stored name is still added to the relevant choice so editing
    // never silently drops it.
    AutomationJobDialog(wxWindow* parent, std::vector<ConnDbs> conns,
                        const core::AutomationJob& job, bool isNew);

    // The edited job (valid only after ShowModal() == wxID_OK).
    const core::AutomationJob& Job() const { return job_; }

private:
    void BuildUi();
    void SyncEnabledState();               // enable/disable target + option rows by type
    void FillDbChoice(wxChoice* dbc, const wxString& connName, const wxString& keepDb);
    void OnConnChanged(bool source);
    void OnOk(wxCommandEvent&);
    bool IsSyncType() const;

    std::vector<ConnDbs>  conns_;
    core::AutomationJob   job_;
    bool                  isNew_ = true;

    wxTextCtrl*  nameCtrl_ = nullptr;
    wxChoice*    typeCtrl_ = nullptr;
    wxChoice*    srcConn_  = nullptr;
    wxChoice*    srcDb_    = nullptr;
    wxChoice*    tgtConn_  = nullptr;
    wxChoice*    tgtDb_    = nullptr;
    wxStaticText* tgtLabel_ = nullptr;
    wxCheckBox*  cbDrop_   = nullptr;
    wxCheckBox*  cbTxn_    = nullptr;
    wxCheckBox*  cbData_   = nullptr;
    wxStaticText* hint_    = nullptr;
};

} // namespace ui
