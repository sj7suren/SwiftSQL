// ColumnPicker.h — a small reusable searchable column selector.
//
// Opened from a sort row's column button (SortPanel). A wxSearchCtrl on top +
// a live-filtered list below, so it stays usable at 50+ columns where a flat
// menu / plain wxChoice does not. Keyboard-first: type to filter, ↑/↓ to move,
// Enter / double-click / 确定 to pick, Esc to cancel. Columns already used by
// another row are shown with a "（已用）" hint and are not pickable.
//
// Implemented as a small MODAL wxDialog (not wxPopupTransientWindow): the popup's
// mouse-capture + transient-dismiss made clicks on the inner wxListBox unreliable
// on MSW (the click-through dismissed the popup before the list registered a
// selection). A plain dialog makes mouse + keyboard selection work reliably.
#pragma once

#include <wx/dialog.h>
#include <wx/string.h>
#include <functional>
#include <vector>

class wxSearchCtrl;
class wxListBox;

namespace ui {

class ColumnPicker : public wxDialog {
public:
    explicit ColumnPicker(wxWindow* parent);

    // Show the picker just under `anchor` (modal). `cols` = every column name;
    // `disabled` = column indices already used by other rows (greyed, not
    // pickable); `onPick` fires with the chosen column index when confirmed.
    void PopupAt(wxWindow* anchor, const std::vector<wxString>& cols,
                 const std::vector<int>& disabled,
                 std::function<void(int)> onPick);

private:
    void Rebuild(const wxString& filter);   // (re)fill the list for the current filter
    void Confirm();                          // commit the highlighted row (if pickable)
    void MoveSelection(int delta);

    wxSearchCtrl* search_ = nullptr;
    wxListBox*    list_   = nullptr;

    std::vector<wxString> cols_;
    std::vector<bool>     disabled_;
    std::vector<int>      shown_;            // list row → column index (-1 = not pickable)
    int                   chosen_ = -1;      // result of the last modal run
    std::function<void(int)> onPick_;
};

} // namespace ui
