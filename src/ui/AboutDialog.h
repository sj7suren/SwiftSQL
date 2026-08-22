// AboutDialog.h — self-drawn "About SwiftSQL" dialog (replaces the old
// wxMessageBox). Shows the app logo, version, tagline, copyright and the
// Apache-2.0 licence line. It also offers a "Donate" button that opens a
// secondary DonateDialog with the embedded Alipay QR code.
#pragma once

#include <wx/dialog.h>

namespace ui {

class AboutDialog : public wxDialog {
public:
    explicit AboutDialog(wxWindow* parent);
};

} // namespace ui
