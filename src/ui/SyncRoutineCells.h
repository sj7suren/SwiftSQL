// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncRoutineCells.h — the compare grid's cell rendering for the rows that are
// INFORMATIONAL rather than executable: the 函数 / 存储过程 category headers and
// the individual routine rows beneath them (ADR-013).
//
// ---------------------------------------------------------------------------
// WHY THIS IS ITS OWN TRANSLATION UNIT
// ---------------------------------------------------------------------------
// SyncCompareGrid.cpp was at 723 lines against the charter's 1000-line ceiling
// before routines existed. Adding the renderer plus two more cell paths took it
// past 900, which is the point at which the charter says split rather than
// cram — the same call that produced SyncCompareRow.h and SyncCompareDetail.
// The seam is a real one and not an arbitrary cut: everything here is a PURE
// function of (DiffNode, column). None of it reads ui::SyncSelection, because
// none of these rows has any selection state to read — which is precisely the
// property that makes routines non-executable.
//
// ---------------------------------------------------------------------------
// THE NO-CHECKBOX RULE
// ---------------------------------------------------------------------------
// A routine row's ☑ cell must be EMPTY. Not a disabled checkbox, not an
// indeterminate one: both of those read as "a control that is currently off",
// which invites the click that the CEO ruling says must not have a target.
// Routine DDL is never generated, cross-engine or same-engine — a human
// rewrites — so there is nothing for a checkbox to mean here.
//
// wxDataViewCheckIconTextRenderer always paints a box, and renderers are
// per-column, so the "no box here" decision has to travel in the cell VALUE.
// NoCheckCell() produces that value and NoBoxCheckRenderer honours it.
#pragma once

#include <wx/dataview.h>
#include <wx/string.h>

namespace ui {

struct DiffNode;
enum class DiffOp;

// Column layout of the compare grid — the PRD's 8 columns, in order. Shared
// with SyncCompareGrid.cpp, which indexes the same columns.
enum GridCol {
    kColCheck = 0,   // ☑            (wxDataViewCheckIconText, tri-state)
    kColName,        // 表名 / 例程签名
    kColType,        // 类型
    kColStruct,      // 结构差异 / 说明
    kColInsert,      // 插入
    kColUpdate,      // 更新
    kColDelete,      // 删除          (wxDataViewCheckIconText, red, gated)
    kColStatus,      // 状态
    kColCount
};

// ＋ / ≠ / － and the diff colour tokens for an op. Here rather than in the
// grid because the routine cells below need them too and two copies of a
// visual vocabulary drift.
wxString OpBadge(DiffOp op);
wxColour ColourForOp(DiffOp op);

// The value that means "this row has no checkbox at all".
wxVariant NoCheckCell();

// wxDataViewCheckIconTextRenderer that draws NOTHING for a NoCheckCell() value
// and refuses to activate it. Used for BOTH checkbox columns.
class NoBoxCheckRenderer : public wxDataViewCheckIconTextRenderer {
public:
    NoBoxCheckRenderer();

    bool SetValue(const wxVariant& value) override;
    bool Render(wxRect cell, wxDC* dc, int state) override;
    bool ActivateCell(const wxRect& cell, wxDataViewModel* model,
                      const wxDataViewItem& item, unsigned int col,
                      const wxMouseEvent* mouseEvent) override;

private:
    bool suppress_ = false;
};

// One 函数 / 存储过程 category header row. When the category is degraded
// (a side's catalog could not be read) its 状态 cell carries the 不可见 label
// and it has no children to render — see ui::MakeRoutineCategoryNode.
void RoutineCategoryCell(wxVariant& v, const DiffNode& n, unsigned int col);

// One routine row. The ☑ and 删除 cells are empty; 状态 carries the verdict.
void RoutineCell(wxVariant& v, const DiffNode& n, unsigned int col);

// Foreground/weight for either of the above. Returns false when the cell wants
// the default attributes, matching wxDataViewModel::GetAttr's contract.
bool RoutineAttr(const DiffNode& n, unsigned int col, wxDataViewItemAttr& attr);

} // namespace ui
