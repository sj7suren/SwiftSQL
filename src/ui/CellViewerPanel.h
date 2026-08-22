// CellViewerPanel.h — bottom-of-grid single-cell content viewer.
//
// Shows one database cell's full value in a read-only, syntax-coloured
// wxStyledTextCtrl, with a render-type selector (文本 / HEX / JSON / XML /
// WEB / IMAGE) above it. Pretty-prints JSON/XML, hex-dumps raw bytes, and
// colours WEB (HTML) source. IMAGE is a best-effort text note in v1.
#pragma once

#include <wx/panel.h>
#include <wx/string.h>

class wxStyledTextCtrl;
class wxChoice;

namespace ui {

class CellViewerPanel : public wxPanel {
public:
    // showSelector=false hides the built-in 文本/HEX/… dropdown so the render
    // type can be driven externally (the data browser puts it on the top toolbar).
    explicit CellViewerPanel(wxWindow* parent, bool showSelector = true);

    void ShowValue(const wxString& value);   // set the raw cell text; re-render in the current type
    void Clear();                            // empty the viewer

    // Render-type index: 0=文本 1=HEX 2=JSON 3=XML 4=WEB 5=IMAGE. SetType re-renders
    // the current value in the new type (drives the cell-menu 显示▶ switch).
    void SetType(int typeIndex);
    int  GetType() const;

private:
    void Render();                           // (re)render value_ per the selected type

    wxString          value_;
    wxChoice*         type_ = nullptr;       // 文本 / HEX / JSON / XML / WEB / IMAGE
    wxStyledTextCtrl* view_ = nullptr;       // read-only, syntax-colored display
};

} // namespace ui
