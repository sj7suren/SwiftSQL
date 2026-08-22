// SqlErrorDialog.h — a resizable, read-only error viewer.
//
// Used when a database operation returns a long/multi-line error the user needs
// to read in full and copy elsewhere (e.g. a routine/package COMPILE that fails
// with a list of PLS-xxxxx diagnostics). The message sits in a read-only
// multi-line text control; a 复制 button puts the whole text on the clipboard.
// Kept generic (title + body) so any long-error path can reuse it.
#pragma once

#include <wx/string.h>
#include "ui/CenteredDialog.h"

namespace ui {

class SqlErrorDialog : public CenteredDialog {
public:
    SqlErrorDialog(wxWindow* parent, const wxString& title, const wxString& message);
};

// Convenience: build + show a modal SqlErrorDialog.
void ShowSqlError(wxWindow* parent, const wxString& title, const wxString& message);

} // namespace ui
