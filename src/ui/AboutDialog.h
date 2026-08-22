// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AboutDialog.h — self-drawn "About SwiftSQL" dialog (replaces the old
// wxMessageBox). Shows the app logo, version, tagline, copyright and the
// GPL-3.0-or-later licence line. It also offers a "Donate" button that opens a
// secondary DonateDialog with the embedded Alipay QR code.
#pragma once

#include <wx/dialog.h>

namespace ui {

class AboutDialog : public wxDialog {
public:
    explicit AboutDialog(wxWindow* parent);
};

} // namespace ui
