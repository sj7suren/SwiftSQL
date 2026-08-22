// ScriptLibraryPanel.h — the "已保存 SQL 脚本库" tab: a report-style list of every
// script saved via Ctrl+S in a plain SQL editor. Columns: 名称 / 文件大小 /
// 创建时间 / 修改时间 / 归属用户 / 执行用户. Double-click or right-click ▸ 打开 opens
// the script in a new SQL editor; right-click ▸ 删除 removes it. The panel is a
// thin view over core::ScriptStore — it owns no state beyond the list control and
// delegates open/delete back to MainFrame via callbacks.
#pragma once

#include <wx/panel.h>
#include <functional>

class wxListCtrl;

namespace ui {

class ScriptLibraryPanel : public wxPanel {
public:
    explicit ScriptLibraryPanel(wxWindow* parent);

    // Re-query core::ScriptStore::List() and repopulate the grid.
    void Reload();

    // Open a script (by display name) in a new SQL editor tab.
    void SetOnOpen(std::function<void(const wxString&)> fn) { onOpen_ = std::move(fn); }
    // Delete a script (by display name); the panel reloads afterwards.
    void SetOnDelete(std::function<void(const wxString&)> fn) { onDelete_ = std::move(fn); }

private:
    wxString SelectedName() const;              // display name of the focused row
    void     StretchColumns();                  // last column fills the width
    void     OpenSelected();
    void     DeleteSelected();

    wxListCtrl* list_ = nullptr;
    std::function<void(const wxString&)> onOpen_;
    std::function<void(const wxString&)> onDelete_;
};

} // namespace ui
