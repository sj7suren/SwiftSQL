// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableInfoPanel.h — the right-hand Navicat-style table-info panel: a fixed list
// of 14 metadata fields (行数 / 引擎 / 数据长度 / …) populated from a
// db::TableDetail. Extracted from EditorPage so ResultGridPanel stays under the
// 1000-line file charter (docs/CHARTER.md).
#pragma once

#include <wx/panel.h>
#include <vector>
#include "db/DbDriver.h"   // db::TableDetail

class wxStaticText;

namespace ui {

class TableInfoPanel : public wxPanel {
public:
    explicit TableInfoPanel(wxWindow* parent);

    void SetTitle(const wxString& title);
    void Reset();                              // all values → "—"
    void Populate(const db::TableDetail& d);   // fill the 14 fields

private:
    wxStaticText*              title_ = nullptr;
    std::vector<wxStaticText*> vals_;           // value labels (fixed field order)
};

} // namespace ui
