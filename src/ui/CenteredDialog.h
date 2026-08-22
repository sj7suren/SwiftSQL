// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// CenteredDialog.h — a wxDialog that always shows in the middle of the screen.
//
// Every modal dialog in SwiftSQL derives from this instead of wxDialog, so
// screen-centring is inherited uniformly (the frameless custom-chrome main
// window throws off the default parent-relative centring). Centring happens in
// ShowModal(), i.e. after the dialog is fully built and sized, so it lands
// dead-centre regardless of when the layout is computed.
#pragma once

#include <wx/dialog.h>
#include <wx/textdlg.h>

namespace ui {

class CenteredDialog : public wxDialog {
public:
    using wxDialog::wxDialog;   // inherit all wxDialog constructors

    int ShowModal() override
    {
        CentreOnScreen();
        return wxDialog::ShowModal();
    }
};

// Screen-centred single-line text prompt — a drop-in for wxGetTextFromUser that
// (unlike it) appears in the middle of the screen. Returns "" if cancelled.
inline wxString GetTextCentered(wxWindow* parent, const wxString& message,
                                const wxString& caption,
                                const wxString& value = wxEmptyString)
{
    wxTextEntryDialog dlg(parent, message, caption, value);
    dlg.CentreOnScreen();
    if (dlg.ShowModal() != wxID_OK) return wxEmptyString;
    return dlg.GetValue();
}

} // namespace ui
