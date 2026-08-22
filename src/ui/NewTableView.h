// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// NewTableView.h — the 新建表 (create-table) designer. Subclasses TableDesignView to
// reuse ALL of its tabs/grids/cells, overriding only the create-specific behaviour:
// no ⑦TABLE DDL tab, the ⑥DDL 预览 renders CREATE TABLE (not ALTER), and 保存 prompts
// for a name then runs CREATE. Kept a SEPARATE class from the edit flow so the two
// can't tangle (per request).
#pragma once

#include "ui/TableDesignView.h"

#include <functional>

namespace ui {

class NewTableView : public TableDesignView {
public:
    explicit NewTableView(wxWindow* parent) : TableDesignView(parent) {}

    // Start an empty CREATE-TABLE design in `database` (no table name yet).
    void BeginNew(db::IConnection* conn, const wxString& database, db::DbType type);
    // Fired with the new table name after a successful create — the host refreshes the
    // 表信息 list / sidebar so the table appears.
    void SetOnTableCreated(std::function<void(const wxString&)> cb) { onCreated_ = std::move(cb); }

protected:
    // A new table has no name yet, so editability must NOT require table_ — otherwise
    // every toolbar button (增加字段 / 保存 / …) and the attribute panel disable.
    bool IsEditable() const override { return conn_ && conn_->IsConnected(); }
    void SaveViaShortcut() override { OnSaveActiveTab(); }   // Ctrl+S → CREATE (prompt name)
    void OnSaveActiveTab() override;        // prompt for a name → CREATE
    void RefreshGeneratedTabs() override;   // ⑥DDL 预览 = CREATE preview (no ALTER, no ⑦)

private:
    std::function<void(const wxString&)> onCreated_;
};

} // namespace ui
