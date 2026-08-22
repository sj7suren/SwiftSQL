// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// NewDatabaseDialog.h — engine-adaptive "new database" editor. The field set is
// built at runtime from the driver's DbCreateCaps (charset/collation for MySQL;
// owner/encoding/locale/template/tablespace/conn-limit for PostgreSQL; bare name
// for engines that report no options), and a live SQL preview shows exactly the
// CREATE DATABASE statement that will run. All SQL assembly lives in the driver.
#pragma once

#include <wx/dialog.h>
#include <vector>
#include "db/DbDriver.h"
#include "ui/CenteredDialog.h"

class wxTextCtrl;

namespace ui {

class NewDatabaseDialog : public CenteredDialog {
public:
    NewDatabaseDialog(wxWindow* parent, db::IConnection* conn,
                      const wxString& engineName);

    wxString DatabaseName() const;
    wxString Sql() const { return sql_; }   // valid after ShowModal()==wxID_OK

private:
    void BuildUi(const wxString& engineName);
    db::DbCreateRequest CurrentRequest() const;
    void Refilter();        // rebuild grouped choices (collation ← charset)
    void UpdatePreview();
    void OnOk(wxCommandEvent&);

    db::IConnection* conn_ = nullptr;
    db::DbCreateCaps caps_;

    wxTextCtrl* nameCtrl_ = nullptr;
    wxTextCtrl* preview_  = nullptr;

    struct Field { db::DbCreateOption opt; wxWindow* ctrl = nullptr; };
    std::vector<Field> fields_;

    wxString sql_;
};

} // namespace ui
