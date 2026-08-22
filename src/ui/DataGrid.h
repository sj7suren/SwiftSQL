// DataGrid.h — a wxGrid that paints the CURRENT row's left row-label (gutter)
// with an active tint + bold primary number, so the selected row is easy to
// track. wxGrid's SetLabelTextColour/Font are grid-global, so per-row styling
// needs an owner-drawn row label (override DrawRowLabel). Header-only.
#pragma once

#include <wx/grid.h>
#include <wx/dc.h>
#include <wx/pen.h>
#include <wx/brush.h>

#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

class DataGrid : public wxGrid {
public:
    DataGrid(wxWindow* parent, wxWindowID id) : wxGrid(parent, id) {}

    // Track the active (selected) row; repaint the gutter so old + new rows update.
    void SetActiveRow(int row)
    {
        if (row == activeRow_) return;
        activeRow_ = row;
        if (wxWindow* w = GetGridRowLabelWindow()) w->Refresh();
    }
    int GetActiveRow() const { return activeRow_; }

protected:
    void DrawRowLabel(wxDC& dc, int row) override
    {
        if (row != activeRow_) { wxGrid::DrawRowLabel(dc, row); return; }
        if (GetRowHeight(row) <= 0 || GetRowLabelSize() <= 0) return;

        const wxRect rect(0, GetRowTop(row), GetRowLabelSize(), GetRowHeight(row));
        dc.SetBrush(wxBrush(theme::kRowHeaderActiveBg));       // #E7EFFB active gutter
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(rect);
        dc.SetPen(wxPen(GetGridLineColour()));                 // keep the grid separators
        dc.DrawLine(rect.x, rect.GetBottom(), rect.GetRight(), rect.GetBottom());
        dc.DrawLine(rect.GetRight(), rect.y, rect.GetRight(), rect.GetBottom());

        dc.SetFont(Ui(9, /*bold*/ true));
        dc.SetTextForeground(theme::kPrimary);                 // bold blue row number
        const wxString label = GetRowLabelValue(row);
        wxCoord tw = 0, th = 0; dc.GetTextExtent(label, &tw, &th);
        dc.DrawText(label, rect.x + (rect.width - tw) / 2, rect.y + (rect.height - th) / 2);
    }

private:
    int activeRow_ = -1;
};

} // namespace ui
