// NullCellEditor.h — a grid text editor that treats the driver's stringified
// NULL marker ("NULL", per db::QueryResult) as an EMPTY edit box.
//
// Rationale: the driver delivers a real SQL NULL as the literal text "NULL"
// (DbDriver.h). With the stock wxGridCellTextEditor, double-clicking a NULL cell
// pre-fills the editor with "NULL", forcing the user to delete it before typing.
// This editor instead opens empty on a NULL cell, and — the key rule — commits an
// EMPTY edit box back as the NULL marker. So:
//   • NULL cell → edit box empty; leave it empty → stays NULL; type a value → that value.
//   • any cell cleared to empty in the editor → becomes NULL.
// An explicit empty STRING is set via the右键「设为空字符串」path, not the editor.
//
// Ambiguity (documented, driver-contract limitation): a real string whose text is
// exactly "NULL" is indistinguishable from a real NULL here, because QueryResult
// already stringified both to "NULL". Disambiguating needs a per-cell null flag in
// the driver layer — out of scope for this UI-layer fix.
#pragma once

#include <wx/grid.h>
#include <wx/textctrl.h>

namespace ui {

inline const wxString kNullMarker = wxT("NULL");

class NullCellEditor : public wxGridCellTextEditor {
public:
    wxGridCellEditor* Clone() const override { return new NullCellEditor(); }

    // Open empty when the underlying value is the NULL marker (no "NULL" to erase).
    void BeginEdit(int row, int col, wxGrid* grid) override
    {
        own_ = grid->GetTable()->GetValue(row, col);
        DoBeginEdit(own_ == kNullMarker ? wxString() : own_);
    }

    // Commit an empty box back as NULL; a typed value replaces it. (Base m_value is
    // private, so we keep our own and write it ourselves in ApplyEdit.)
    bool EndEdit(int row, int col, const wxGrid* grid,
                 const wxString& oldval, wxString* newval) override
    {
        if (!Text()) return false;
        const wxString cur = Text()->GetValue();
        const wxString result = cur.empty() ? kNullMarker : cur;
        if (result == oldval) return false;    // unchanged (e.g. NULL left empty)
        own_ = result;
        if (newval) *newval = result;
        return true;
    }

    void ApplyEdit(int row, int col, wxGrid* grid) override
    {
        grid->GetTable()->SetValue(row, col, own_);
    }

private:
    wxString own_;
};

} // namespace ui
