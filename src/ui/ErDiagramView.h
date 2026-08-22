// ErDiagramView.h — entity-relationship diagram, hand-drawn on a scrolled
// canvas with wxGraphicsContext (per docs/UI's "ER 关系图" board):
// entity cards with PK/FK-marked columns + foreign-key connector lines.
#pragma once

#include <wx/panel.h>
#include <vector>
#include "db/DbDriver.h"

class wxStaticText;

namespace ui {

class ErCanvas;   // the scrolled drawing surface

class ErDiagramView : public wxPanel {
public:
    explicit ErDiagramView(wxWindow* parent);

    // Load all tables + their columns + foreign keys of the current database.
    void Load(db::IConnection* conn, const wxString& database);

private:
    void SetZoom(double z);

    double        zoom_ = 0.8;
    wxStaticText* subtitle_ = nullptr;
    wxStaticText* zoomLabel_ = nullptr;
    ErCanvas*     canvas_ = nullptr;
};

} // namespace ui
