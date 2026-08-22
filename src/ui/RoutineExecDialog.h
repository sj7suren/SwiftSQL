// RoutineExecDialog.h — parameter-entry dialog for 执行 (a stored procedure or
// function).
//
// Shows a small table (参数名 / 类型 / 值). The driver has no parameter-metadata
// accessor yet, so parameters are entered by hand: the user adds a row per
// argument, picks a type, and types a value. On OK the dialog assembles a runnable
// statement:
//   procedure → CALL <name>(<args>)
//   function  → SELECT <name>(<args>)      (MySQL / PostgreSQL)
// Values are quoted by type (文本 → 'escaped'; 数字/小数 → bare; empty → NULL).
// The 集合(SET/ARRAY) type is passed through verbatim (its literal syntax differs
// per engine — see CallStatement()).
#pragma once

#include <wx/string.h>
#include "db/DbDriver.h"          // db::Dialect
#include "ui/CenteredDialog.h"

class wxGrid;
class wxTextCtrl;

namespace ui {

class RoutineExecDialog : public CenteredDialog {
public:
    RoutineExecDialog(wxWindow* parent, const wxString& routineName,
                      bool isProcedure, db::Dialect dialect);

    // The runnable CALL / SELECT statement assembled from the entered parameters.
    wxString CallStatement() const { return statement_; }

private:
    void AddParamRow();
    void DeleteSelectedRow();
    wxString BuildStatement() const;   // from the current grid contents
    void UpdatePreview();
    void OnOk(wxCommandEvent&);

    wxString      routine_;
    bool          isProcedure_ = true;
    db::Dialect   dialect_;
    wxGrid*       grid_    = nullptr;
    wxTextCtrl*   preview_ = nullptr;
    wxString      statement_;
};

} // namespace ui
