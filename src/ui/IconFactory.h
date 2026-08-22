// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// IconFactory.h — all icons are drawn in pure C++ with wxGraphicsContext.
// No external image files: keeps the binary self-contained (single-file dist)
// and icons crisp at any size.
#pragma once

#include <wx/bitmap.h>
#include <wx/colour.h>
#include "core/DbTypes.h"

namespace icons {

// Brand icon for a database engine. By default renders the vendor logo in full
// brand colour on a soft tint chip (for menus / dialog). Pass `mono` to render
// the vendor logo as a single-colour silhouette instead — used by the sidebar
// tree to signal connection status (green = connected, grey = disconnected).
wxBitmap DbBrand(db::DbType type, int size, const wxColour& mono = wxNullColour);

// SwiftSQL app logo: blue→cyan gradient rounded square + white lightning bolt
// piercing a cylinder ring (from the brand board in docs/UI).
wxBitmap AppLogo(int size);

// Stroke glyphs used across toolbar / sidebar / editor chrome.
// Drawn in a 24x24 design space, scaled to `size`.
enum class Glyph {
    Link,          // 新建连接
    Code,          // 新建SQL </>
    Table,         // 表
    Eye,           // 视图
    Function,      // 函数 fx
    Model,         // 模型
    Backup,        // 备份
    Import,        // 导入
    Export,        // 导出
    Timer,         // 自动运行
    Star,          // AI 生成 SQL
    Play,          // 运行 (filled)
    Stop,          // 停止 (filled square)
    TxBegin,       // 开始事务 (flag)
    Commit,        // 提交事务 (circle + check)
    Rollback,      // 回滚事务 (undo arrow)
    Cylinder,      // 数据库
    Search,        // 搜索
    Format,        // 格式化
    Beautify,      // SQL 美化 { } + 星火
    Explain,       // 解释
    Plus,          // 新建标签
    ChevronRight,  // 树折叠
    ChevronDown,   // 树展开
    ErDiagram,     // ER 图(两方块+连线)
    StarLine,      // AI 生成 SQL(描边五角星)
    ViewLayers,    // 数据库视图(层叠矩形 = 派生/虚拟表)
    // ---- data-browser toolbar + bottom bar (design §5 icon inventory) ----
    Filter,        // 筛选 (漏斗)
    Sort,          // 排序 (升序竖线 + 递减横线)
    Refresh,       // 刷新 (环形箭头)
    RowAdd,        // 增行 (表 + 加号)
    RowDelete,     // 删行 (表 + 减号)
    Save,          // 保存 (软盘)
    PageFirst,     // 首页 (‖◀◀)
    PagePrev,      // 上一页 (◀)
    PageNext,      // 下一页 (▶)
    PageLast,      // 末页 (▶▶‖)
    TxDatabase,    // 开始事务 (数据库圆柱 + ▶ begin badge)
    TxCommit,      // 提交事务 (数据库圆柱 + ✓ commit badge)
    TxRollback,    // 回滚事务 (数据库圆柱 + ↺ rollback badge)
    Close,         // 关闭 / 清除 (X)
    Minus,         // 删除行 (居中水平线 −)
    // ---- table-design field toolbar ----
    Key,           // 主键 (钥匙)
    ArrowUp,       // 上移 (↑)
    ArrowDown,     // 下移 (↓)
    RowInsert,     // 插入字段 (表 + 右上 ⊕ = 插到上方)
    Copy,          // 复制 (两叠矩形)
    Paste,         // 粘贴 (剪贴板)
    Users,         // 用户组 (两个人形) — user-management pill segment + tab
    Folder,        // 分组 (带页签的文件夹) — sidebar connection / object groups
};

wxBitmap Stroke(Glyph g, int size, const wxColour& color, double strokeWidth = 1.9);

} // namespace icons
