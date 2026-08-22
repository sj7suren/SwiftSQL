// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SyncCompareGrid.cpp — see header. wxDataViewCtrl + a wxDataViewModel that
// reads and writes ui::SyncSelection directly (no local check state).
#include "ui/SyncCompareGrid.h"

#include <wx/dataview.h>
#include <wx/sizer.h>

#include "ui/I18n.h"
#include "ui/SyncCompareRow.h"
#include "ui/SyncDiffModel.h"
#include "ui/SyncRoutineCells.h"
#include "ui/SyncSelection.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

// The column indices, the ＋/≠/－ badge, the op colours, and the rendering of
// the informational 函数/存储过程 rows all live in SyncRoutineCells.h — see that
// header for why they are a separate TU (this file's line budget).
const wxString kCheckType = wxS("wxDataViewCheckIconText");

wxString KindLabel(DiffNodeKind k)
{
    switch (k) {
    case DiffNodeKind::Column:     return tr(L"列");
    case DiffNodeKind::Index:      return tr(L"索引");
    case DiffNodeKind::Constraint: return tr(L"外键");
    case DiffNodeKind::Row:        return tr(L"数据");
    case DiffNodeKind::ValueDiff:  return tr(L"值");
    case DiffNodeKind::Warning:    return tr(L"提示");
    default:                       return wxString();
    }
}

// "3 项" / empty. Structure children are the Column/Index/Constraint ones —
// counted from the node tree, never from the rendered DDL text.
wxString StructureCount(const DiffNode& table)
{
    int n = 0;
    for (const auto& c : table.children)
        if (c->kind == DiffNodeKind::Column || c->kind == DiffNodeKind::Index ||
            c->kind == DiffNodeKind::Constraint)
            ++n;
    return n > 0 ? wxString::Format(tr(L"%d 项"), n) : wxString();
}

wxString CountText(long long n)
{
    return n > 0 ? wxString::Format(L"%lld", n) : wxString();
}

// "12" normally; "10 −2" once the user has hand-excluded rows of that category.
//
// The EFFECTIVE number leads and the excluded part is called out beside it,
// rather than the detected total being shown unchanged: the counts in this grid
// are what the user reads to decide, so a column that still says 12 after two
// rows were unchecked is telling them something that will not happen. The
// subtraction is exact — exclusions exist only for a non-truncated diff, whose
// sample holds every counted row (see ui::CountExcludedRows).
wxString CountText(long long total, long long excluded)
{
    if (total <= 0) return wxString();
    if (excluded <= 0) return wxString::Format(L"%lld", total);
    const long long eff = total > excluded ? total - excluded : 0;
    return wxString::Format(L"%lld  −%lld", eff, excluded);
}

} // namespace

// ===========================================================================
// The model — the ONLY bridge between the widget and ui::SyncSelection
// ===========================================================================
//
// Every GetValue() is a fresh read of SyncSelection and every SetValue() is a
// write into it. The model holds no check bit of its own; the members below are
// navigation aids (which nodes are top level, who is whose parent) derived once
// from the borrowed DiffTree, never selection state.
class SyncCompareGridModel : public wxDataViewModel {
public:
    SyncCompareGridModel(const DiffTree* tree, SyncSelection* sel) { Adopt(tree, sel); }

    void Adopt(const DiffTree* tree, SyncSelection* sel)
    {
        tree_ = tree;
        sel_  = sel;
        roots_.clear();
        parent_.clear();
        if (!tree_) return;
        for (const auto& root : tree_->roots) {
            if (root->kind == DiffNodeKind::Category &&
                root->category == DiffCategory::Tables) {
                // The Tables category is flattened away: its table children
                // become the grid's top-level rows, so the grid reads as a
                // table list (Navicat) rather than a tree with one useless
                // root. The category node itself carries no id and nothing
                // executable, so nothing is lost.
                //
                // The ROUTINE categories are deliberately NOT flattened. Their
                // children are informational, and a heading is what tells the
                // user that 函数/存储过程 are a different kind of row from the
                // checkable tables above them — flattening would mix
                // non-actionable rows into a list whose every other row is
                // actionable.
                for (const auto& t : root->children) Index(t.get(), nullptr);
            } else {
                // Warning groups stay whole (Bug #2: a cross-engine finding
                // with no TableUnit must remain visible, never silently
                // dropped behind a table-only view).
                Index(root.get(), nullptr);
            }
        }
    }

    const std::vector<const DiffNode*>& Roots() const { return roots_; }

    // Every table node currently in the grid, as stable keys. This is what
    // select-all operates on: SyncSelection's sweep APIs take the caller's
    // VISIBLE key set, so a filtered or re-sorted grid can never desynchronize
    // from the model (see SyncSelection.h).
    std::vector<TableKey> VisibleTableKeys() const
    {
        std::vector<TableKey> keys;
        for (const DiffNode* n : roots_)
            if (n->kind == DiffNodeKind::Table && !n->id.IsEmpty())
                keys.push_back(TableKey(n->id));
        return keys;
    }

    // Tri-state of the header's select-all affordance, computed from the
    // selection — not remembered from the last click.
    wxCheckBoxState HeaderState() const
    {
        int checked = 0, checkable = 0;
        for (const DiffNode* n : roots_) {
            if (n->kind != DiffNodeKind::Table || n->id.IsEmpty()) continue;
            const TableKey key(n->id);
            const wxCheckBoxState st = TableCheckState(key);
            if (st == wxCHK_UNDETERMINED) return wxCHK_UNDETERMINED;
            ++checkable;
            if (st == wxCHK_CHECKED) ++checked;
        }
        if (checkable == 0 || checked == 0) return wxCHK_UNCHECKED;
        return checked == checkable ? wxCHK_CHECKED : wxCHK_UNDETERMINED;
    }

    void ItemsRefreshed()
    {
        // Top-level rows only, deliberately. Every table's children can number
        // in the hundreds (a 500-row sample per table), and re-notifying all of
        // them on every check would turn one click into a six-figure sweep of
        // ItemChanged calls. Child rows are repainted individually by
        // NotifyRow() when they are the thing that actually changed.
        for (const DiffNode* n : roots_)
            ItemChanged(wxDataViewItem(const_cast<DiffNode*>(n)));
    }

    // Repaint ONE child row and the table above it. Toggling a row's checkbox
    // changes two rows at once — the row's own 状态 cell (已排除) and its
    // table's per-category counts — and the control only knows about the single
    // cell it wrote.
    void NotifyRow(const wxDataViewItem& item)
    {
        if (!item.IsOk()) return;
        const DiffNode* n = Node(item);
        if (!n || n->kind != DiffNodeKind::Row) return;
        ItemChanged(item);
        const wxString tableId = OwningTableId(*n);
        if (tableId.IsEmpty()) return;
        for (const DiffNode* r : roots_)
            if (r->kind == DiffNodeKind::Table && r->id == tableId) {
                ItemChanged(wxDataViewItem(const_cast<DiffNode*>(r)));
                return;
            }
    }

    // ---- wxDataViewModel ----

    unsigned int GetColumnCount() const override { return kColCount; }

    wxString GetColumnType(unsigned int col) const override
    {
        return (col == kColCheck || col == kColDelete) ? kCheckType : wxS("string");
    }

    // Table rows are containers (they expand into their field/row detail) AND
    // must still render every column. Without this override the generic
    // wxDataViewCtrl draws containers in column 0 only, which would erase the
    // per-table counts that are the whole point of this grid.
    bool HasContainerColumns(const wxDataViewItem&) const override { return true; }

    bool IsContainer(const wxDataViewItem& item) const override
    {
        const DiffNode* n = Node(item);
        return n ? !n->children.empty() : true;   // invalid item == the root
    }

    wxDataViewItem GetParent(const wxDataViewItem& item) const override
    {
        const DiffNode* n = Node(item);
        if (!n) return wxDataViewItem(nullptr);
        auto it = parent_.find(n);
        const DiffNode* p = (it == parent_.end()) ? nullptr : it->second;
        return wxDataViewItem(const_cast<DiffNode*>(p));
    }

    unsigned int GetChildren(const wxDataViewItem& item,
                             wxDataViewItemArray& out) const override
    {
        const DiffNode* n = Node(item);
        if (!n) {
            for (const DiffNode* r : roots_)
                out.Add(wxDataViewItem(const_cast<DiffNode*>(r)));
            return static_cast<unsigned>(roots_.size());
        }
        for (const auto& c : n->children)
            out.Add(wxDataViewItem(const_cast<DiffNode*>(c.get())));
        return static_cast<unsigned>(n->children.size());
    }

    void GetValue(wxVariant& v, const wxDataViewItem& item, unsigned int col) const override
    {
        const DiffNode* n = Node(item);
        if (!n) { v = wxString(); return; }
        switch (n->kind) {
        case DiffNodeKind::Table:    GetTableValue(v, *n, col);       break;
        case DiffNodeKind::Category: RoutineCategoryCell(v, *n, col); break;
        case DiffNodeKind::Routine:  RoutineCell(v, *n, col);         break;
        default:                     GetChildValue(v, *n, col);       break;
        }
    }

    // The write half of the round trip. Nothing is stored here — the bit goes
    // straight into SyncSelection, and the next repaint reads it back out.
    bool SetValue(const wxVariant& v, const wxDataViewItem& item, unsigned int col) override
    {
        const DiffNode* n = Node(item);
        if (!sel_ || !n) return false;
        if (col != kColCheck && col != kColDelete) return false;

        wxDataViewCheckIconText value;
        value << v;
        const bool on = (value.GetCheckedState() == wxCHK_CHECKED);

        // A CONCRETE data row. The checkbox reads as "include this row", so an
        // UNCHECK is an exclusion — the inverse the selection model stores.
        //
        // The key handed over is DiffNode::rowKey, which is
        // db::sync::RowChange::RowKey() copied verbatim. Nothing here builds a
        // key out of the row's LABEL ("id = 42" is display text) or out of its
        // position in the sample (ui::StableId deletes its integral
        // constructors, so that would not compile anyway).
        //
        // SetRowExcluded itself refuses a truncated table, so this is gated
        // twice: IsEnabled() below never offers the checkbox there, and the
        // model would refuse the write even if it did.
        if (col == kColCheck && IsSelectableRow(*n)) {
            const wxString tableId = OwningTableId(*n);
            if (tableId.IsEmpty()) return false;
            return sel_->SetRowExcluded(TableKey(tableId), RowKey(n->rowKey), !on);
        }

        if (n->kind != DiffNodeKind::Table || n->id.IsEmpty()) return false;
        const TableKey key(n->id);

        if (col == kColCheck) {
            // Deliberately SetTableChecked, not a loop over all four
            // categories: it sweeps structure/inserts/updates and leaves
            // Deletes alone, so a select-all can never silently arm a
            // destructive delete (SyncSelection.h's documented rule).
            sel_->SetTableChecked(key, on);
        } else {
            sel_->SetChecked(key, ChangeCategory::Deletes, on);
        }
        return true;
    }

    bool IsEnabled(const wxDataViewItem& item, unsigned int col) const override
    {
        // Only the two CHECKBOX columns are ever gated. Everything else stays
        // enabled, including on child rows: wxDataViewCtrl greys a disabled
        // cell, so returning false for a column leaf's 表名/类型 cells would
        // render the entire drill-in as if it were unavailable.
        if (col != kColCheck && col != kColDelete) return true;

        const DiffNode* n = Node(item);
        if (!sel_ || !n) return false;

        // A concrete data row is activatable in the ☑ column exactly when the
        // selection model would accept the write — RowSelectionAvailable() is
        // asked rather than TableDiff::truncated re-read, so the widget and the
        // model cannot come to different conclusions about the same table.
        if (IsSelectableRow(*n)) return col == kColCheck;

        if (n->kind != DiffNodeKind::Table || n->id.IsEmpty()) return false;
        const TableDiff* d = sel_->Find(TableKey(n->id));
        if (!d) return false;
        if (col == kColDelete) {
            // The master switch is part of the gate, exactly as it is in
            // SyncSelection::IsActive — the cell cannot be armed while the
            // switch is off, and even if it somehow were, Build() could not
            // produce a delete without a DeleteGate.
            return sel_->AllowDeletes() && d->Executable(ChangeCategory::Deletes);
        }
        return d->Executable(ChangeCategory::Structure) ||
               d->Executable(ChangeCategory::Inserts) ||
               d->Executable(ChangeCategory::Updates);
    }

    bool GetAttr(const wxDataViewItem& item, unsigned int col,
                 wxDataViewItemAttr& attr) const override
    {
        const DiffNode* n = Node(item);
        if (!n) return false;

        if (n->kind == DiffNodeKind::Table) {
            if (col == kColDelete && (n->change == DiffChangeKind::Drop || HasDeletes(*n))) {
                attr.SetColour(theme::kDiffDelFg);   // destructive: always red
                return true;
            }
            if (col == kColName) { attr.SetBold(true); return true; }
            if (col == kColStatus && sel_) {
                const TableDiff* d = sel_->Find(TableKey(n->id));
                if (d && CompareRowStatusIsWarning(ClassifyCompareStatus(*d))) {
                    attr.SetColour(theme::kDotAmber);
                    return true;
                }
            }
            if (col == kColType && n->change == DiffChangeKind::Drop) {
                attr.SetColour(theme::kDiffDelFg);
                return true;
            }
            return false;
        }

        if (n->kind == DiffNodeKind::Category || n->kind == DiffNodeKind::Routine)
            return RoutineAttr(*n, col, attr);

        if (n->kind == DiffNodeKind::Warning) { attr.SetColour(theme::kDotAmber); return true; }

        // An excluded row is greyed WHOLE, not just in the checkbox column: the
        // op badge and the value summary are describing something that will no
        // longer happen, so leaving them in the op's own colour would keep them
        // reading as part of the plan. Muting is the same visual language a
        // placeholder side gets in the compare grids.
        if (IsExcludedRow(*n)) { attr.SetColour(theme::kTextMuted); return true; }

        if (col == kColName || col == kColType) { attr.SetColour(ColourForOp(n->op)); return true; }
        return false;
    }

private:
    static const DiffNode* Node(const wxDataViewItem& item)
    {
        return static_cast<const DiffNode*>(item.GetID());
    }

    void Index(const DiffNode* node, const DiffNode* parent)
    {
        if (parent) parent_[node] = parent;
        else        roots_.push_back(node);
        for (const auto& c : node->children) Index(c.get(), node);
    }

    // The stable id of the Table this node belongs to. Walks the parent chain,
    // which is the ONLY way a child resolves to an identity — Table nodes are
    // the only ones carrying an id, and there is no positional fallback to get
    // wrong (same contract SyncCompareGrid::FocusedTableId holds to).
    wxString OwningTableId(const DiffNode& node) const
    {
        const DiffNode* n = &node;
        while (n) {
            if (n->kind == DiffNodeKind::Table) return n->id;
            auto it = parent_.find(n);
            n = (it == parent_.end()) ? nullptr : it->second;
        }
        return wxString();
    }

    // Is this node a CONCRETE data row whose table still permits hand-picking?
    //
    // Three conditions, and each rules out a different lie:
    //   * kind == Row + a non-empty rowKey — a SUMMARY row (「修改 5 行」) is a
    //     counter with no identity, so a checkbox on it would claim a
    //     granularity that does not exist;
    //   * the row resolves to a table;
    //   * SyncSelection::RowSelectionAvailable — false for a TRUNCATED diff,
    //     where the rows on screen are a sample of a larger set and per-row
    //     checkboxes would let the user believe they picked from all of them.
    bool IsSelectableRow(const DiffNode& node) const
    {
        if (!sel_ || node.kind != DiffNodeKind::Row || node.rowKey.IsEmpty()) return false;
        const wxString tableId = OwningTableId(node);
        if (tableId.IsEmpty()) return false;
        return sel_->RowSelectionAvailable(TableKey(tableId));
    }

    // Is this node a routine, or anything beneath one? The whole subtree is
    // informational.
    bool UnderRoutine(const DiffNode& node) const
    {
        const DiffNode* n = &node;
        while (n) {
            if (n->kind == DiffNodeKind::Routine) return true;
            if (n->kind == DiffNodeKind::Table)   return false;
            auto it = parent_.find(n);
            n = (it == parent_.end()) ? nullptr : it->second;
        }
        return false;
    }

    bool IsExcludedRow(const DiffNode& node) const
    {
        if (!sel_ || node.kind != DiffNodeKind::Row || node.rowKey.IsEmpty()) return false;
        const wxString tableId = OwningTableId(node);
        if (tableId.IsEmpty()) return false;
        return sel_->IsRowExcluded(TableKey(tableId), RowKey(node.rowKey));
    }

    bool HasDeletes(const DiffNode& table) const
    {
        if (!sel_ || table.id.IsEmpty()) return false;
        const TableDiff* d = sel_->Find(TableKey(table.id));
        return d && d->Has(ChangeCategory::Deletes);
    }

    // Tri-state over the three non-destructive categories the whole-table
    // checkbox governs. Deletes are excluded on purpose — they have their own
    // column and their own master switch.
    wxCheckBoxState TableCheckState(const TableKey& key) const
    {
        if (!sel_) return wxCHK_UNCHECKED;
        const TableDiff* d = sel_->Find(key);
        if (!d) return wxCHK_UNCHECKED;
        int present = 0, on = 0;
        for (ChangeCategory cat : { ChangeCategory::Structure, ChangeCategory::Inserts,
                                    ChangeCategory::Updates }) {
            if (!d->Has(cat)) continue;
            ++present;
            if (sel_->IsChecked(key, cat)) ++on;
        }
        if (present == 0 || on == 0) return wxCHK_UNCHECKED;
        return on == present ? wxCHK_CHECKED : wxCHK_UNDETERMINED;
    }

    void GetTableValue(wxVariant& v, const DiffNode& n, unsigned int col) const
    {
        const TableKey  key(n.id);
        const TableDiff* d = sel_ ? sel_->Find(key) : nullptr;

        // Rows this table's user hand-unchecked, split by category. Cheap when
        // there are none, which is the state every table starts in.
        const ExcludedRowCounts ex =
            sel_ ? CountExcludedRows(n, *sel_) : ExcludedRowCounts{};

        switch (col) {
        case kColCheck:
            v << wxDataViewCheckIconText(wxString(), wxBitmapBundle(), TableCheckState(key));
            return;
        case kColName:
            v = n.label;
            return;
        case kColType:
            v = CompareRowTypeLabel(ClassifyCompareRow(n.change));
            return;
        case kColStruct:
            v = StructureCount(n);
            return;
        case kColInsert:
            v = d ? CountText(d->inserts, ex.inserts) : wxString();
            return;
        case kColUpdate:
            v = d ? CountText(d->updates, ex.updates) : wxString();
            return;
        case kColDelete: {
            const long long cnt = d ? d->deletes : 0;
            const bool on = sel_ && sel_->IsChecked(key, ChangeCategory::Deletes);
            v << wxDataViewCheckIconText(CountText(cnt, ex.deletes), wxBitmapBundle(),
                                         on ? wxCHK_CHECKED : wxCHK_UNCHECKED);
            return;
        }
        case kColStatus: {
            if (!d) { v = wxString(); return; }
            wxString s = CompareRowStatusLabel(ClassifyCompareStatus(*d));
            // The per-category columns already show the arithmetic; this is the
            // one place that says a hand-selection is in effect AT ALL, so a
            // user scanning the table list can see which tables they touched
            // without expanding each one.
            if (ex.Total() > 0)
                s += wxString::Format(tr(L" · 已排除 %lld 行"), ex.Total());
            v = s;
            return;
        }
        default:
            v = wxString();
            return;
        }
    }

    void GetChildValue(wxVariant& v, const DiffNode& n, unsigned int col) const
    {
        // A routine's 签名 / 语言 / 定义 leaves inherit their parent's rule: no
        // checkbox at all, not a disabled one. Asked of the parent chain rather
        // than of the node's own kind, so a future routine child of any kind is
        // covered without another edit here.
        if ((col == kColCheck || col == kColDelete) && UnderRoutine(n)) {
            v = NoCheckCell();
            return;
        }

        switch (col) {
        case kColCheck:
            // A CONCRETE data row gets a real checkbox — this is the widget the
            // 「让用户去勾选哪些可以同步」 requirement was missing. Checked ==
            // included, so the state is the INVERSE of the exclusion set.
            if (IsSelectableRow(n)) {
                v << wxDataViewCheckIconText(
                    wxString(), wxBitmapBundle(),
                    IsExcludedRow(n) ? wxCHK_UNCHECKED : wxCHK_CHECKED);
                return;
            }
            // Everything else — summary counters, structure leaves, warnings,
            // and every row of a TRUNCATED table — renders a blank,
            // non-activatable cell rather than a checkbox implying a
            // granularity the execution spec does not have. The 状态 column
            // below says which of those two reasons applies.
            v << wxDataViewCheckIconText(wxString(), wxBitmapBundle(), wxCHK_UNDETERMINED);
            return;
        case kColDelete:
            v << wxDataViewCheckIconText(wxString(), wxBitmapBundle(), wxCHK_UNDETERMINED);
            return;
        case kColName: {
            const wxString badge = (n.kind == DiffNodeKind::Warning) ? wxString() : OpBadge(n.op);
            v = badge.IsEmpty() ? n.label : (badge + L" " + n.label);
            return;
        }
        case kColType:
            v = KindLabel(n.kind);
            return;
        case kColStruct:
            v = n.summary;
            return;
        case kColStatus:
            // Per-row state, in the column that already means "state of this
            // row". A missing checkbox is otherwise indistinguishable from a
            // broken one, so a sample row of a truncated table says so on the
            // row itself — the table's amber Warning child carries the full
            // sentence, and the 状态 column carries the short form, exactly the
            // arrangement the per-table verdicts already use.
            if (n.kind == DiffNodeKind::Row && !n.rowKey.IsEmpty()) {
                if (IsExcludedRow(n))         v = tr(L"已排除");
                else if (!IsSelectableRow(n)) v = tr(L"样本行 · 不可单独勾选");
                else                          v = wxString();
                return;
            }
            v = wxString();
            return;
        default:
            v = wxString();
            return;
        }
    }

    const DiffTree* tree_ = nullptr;
    SyncSelection*  sel_  = nullptr;

    std::vector<const DiffNode*>                   roots_;
    std::map<const DiffNode*, const DiffNode*>     parent_;
};

// ===========================================================================
// The widget
// ===========================================================================

SyncCompareGrid::SyncCompareGrid(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* s = new wxBoxSizer(wxVERTICAL);
    view_ = new wxDataViewCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxDV_ROW_LINES | wxDV_VERT_RULES | wxDV_SINGLE);
    view_->SetFont(Ui(9));
    s->Add(view_, 1, wxEXPAND);
    SetSizer(s);

    BuildColumns();

    view_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,       &SyncCompareGrid::OnActivated, this);
    view_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,    &SyncCompareGrid::OnSelectionEvent, this);
    view_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_CLICK,  &SyncCompareGrid::OnHeaderClick, this);
    view_->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED,   &SyncCompareGrid::OnValueChanged, this);
}

SyncCompareGrid::~SyncCompareGrid() = default;

void SyncCompareGrid::BuildColumns()
{
    auto addText = [this](const wxString& title, int width, int align) {
        auto* r = new wxDataViewTextRenderer(wxS("string"), wxDATAVIEW_CELL_INERT, align);
        view_->AppendColumn(new wxDataViewColumn(title, r, view_->GetColumnCount(),
                                                 FromDIP(width), static_cast<wxAlignment>(align),
                                                 wxDATAVIEW_COL_RESIZABLE));
    };

    // ☑ — the header doubles as select-all (see OnHeaderClick). The renderer is
    // the no-box variant so an informational row (a routine, a category header)
    // can render an EMPTY cell instead of a disabled-looking checkbox.
    auto* check = new NoBoxCheckRenderer();
    view_->AppendColumn(new wxDataViewColumn(L"☐", check, kColCheck, FromDIP(44),
                                             wxALIGN_CENTER, wxDATAVIEW_COL_RESIZABLE));

    addText(tr(L"表名"),     190, wxALIGN_LEFT);
    addText(tr(L"类型"),      92, wxALIGN_LEFT);
    addText(tr(L"结构差异"),  110, wxALIGN_LEFT);
    addText(tr(L"插入"),      62, wxALIGN_RIGHT);
    addText(tr(L"更新"),      62, wxALIGN_RIGHT);

    // 删除 — its own checkbox, red, and inert until the master switch is on.
    auto* del = new NoBoxCheckRenderer();
    view_->AppendColumn(new wxDataViewColumn(tr(L"删除"), del, kColDelete, FromDIP(76),
                                             wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE));

    addText(tr(L"状态"), 150, wxALIGN_LEFT);
}

void SyncCompareGrid::SetSource(const DiffTree* tree, SyncSelection* selection)
{
    tree_ = tree;
    sel_  = selection;

    // A fresh model per compare: wxDataViewCtrl ref-counts the model, so
    // AssociateModel takes its own reference and the DecRef below hands
    // ownership over — the previous model (if any) is released by the ctrl.
    model_ = new SyncCompareGridModel(tree_, sel_);
    view_->AssociateModel(model_);
    model_->DecRef();

    ExpandTopLevel();
    RefreshFromSelection();
}

void SyncCompareGrid::ExpandTopLevel()
{
    // Collapsed by default: a compare of 200 tables expanded to every field is
    // unreadable. The user expands the table they care about — which is the
    // drill-in gesture Round 1 established.
    //
    // The routine categories ARE expanded, unlike tables: they hold only
    // differences (identical routines never become children), they are bounded
    // by how many routines a schema has, and the user's request was that the
    // differing functions and procedures be SHOWN — a collapsed header saying
    // 「函数 Functions」 does not show them.
    if (!model_) return;
    for (const DiffNode* n : model_->Roots())
        if (n->kind == DiffNodeKind::Warning ||
            (n->kind == DiffNodeKind::Category && n->category != DiffCategory::Tables))
            view_->Expand(wxDataViewItem(const_cast<DiffNode*>(n)));
}

void SyncCompareGrid::RefreshFromSelection()
{
    if (!model_) return;
    // Guard the ItemChanged burst below: each one synchronously re-enters
    // OnValueChanged, which would call back here without this flag (see the
    // header's note on the compare→display recursion). Save/restore rather than
    // a plain assign so a nested call (OnValueChanged → here) leaves the flag
    // set for the rest of the outer scope.
    const bool prev = refreshing_;
    refreshing_ = true;
    model_->ItemsRefreshed();
    if (view_->GetColumnCount() > kColCheck) {
        const wxCheckBoxState st = model_->HeaderState();
        view_->GetColumn(kColCheck)->SetTitle(st == wxCHK_CHECKED     ? L"☑"
                                            : st == wxCHK_UNCHECKED   ? L"☐"
                                                                      : L"◪");
    }
    refreshing_ = prev;
}

void SyncCompareGrid::OnHeaderClick(wxDataViewEvent& ev)
{
    if (ev.GetColumn() != kColCheck || !model_ || !sel_) { ev.Skip(); return; }

    // Select-all over the VISIBLE keys, never over an index range. Note this
    // routes through SetAllTablesChecked, which by contract does not touch
    // Deletes — "全选" must never arm a destructive delete.
    const bool turnOn = (model_->HeaderState() != wxCHK_CHECKED);
    sel_->SetAllTablesChecked(model_->VisibleTableKeys(), turnOn);

    RefreshFromSelection();
    if (OnSelectionChanged) OnSelectionChanged();
}

void SyncCompareGrid::OnValueChanged(wxDataViewEvent& ev)
{
    // Distinguish a REAL user edit from our OWN repaint notifications. Both
    // arrive as wxEVT_DATAVIEW_ITEM_VALUE_CHANGED: the control fires it once
    // after a checkbox SetValue, but wxDataViewModel::ItemChanged() (which
    // RefreshFromSelection/NotifyRow call to repaint) fires it too, and
    // synchronously. Responding to the latter re-enters RefreshFromSelection,
    // which fires more ItemChanged — the unbounded recursion that crashed the
    // compare screen on first paint. When refreshing_ is set we are the source
    // of the event, so there is nothing to react to.
    if (refreshing_) { ev.Skip(); return; }

    // A single cell click can change the header's tri-state and (via
    // SetTableChecked) several categories at once, so re-read the whole grid
    // rather than trusting the one cell the control told us about. Hold the
    // guard across BOTH emitters (RefreshFromSelection AND NotifyRow) so neither
    // one's ItemChanged burst bounces back into here.
    const bool prev = refreshing_;
    refreshing_ = true;
    RefreshFromSelection();
    // ...and, for a per-ROW check, the two rows RefreshFromSelection
    // deliberately does not sweep (see ItemsRefreshed).
    if (model_) model_->NotifyRow(ev.GetItem());
    refreshing_ = prev;
    if (OnSelectionChanged) OnSelectionChanged();
    ev.Skip();
}

void SyncCompareGrid::OnActivated(wxDataViewEvent& ev)
{
    // Double-click / Enter on a container toggles expansion, preserving the
    // Round 1 drill-in gesture.
    const wxDataViewItem item = ev.GetItem();
    if (item.IsOk() && model_ && model_->IsContainer(item)) {
        if (view_->IsExpanded(item)) view_->Collapse(item);
        else                         view_->Expand(item);
    }
    ev.Skip();
}

void SyncCompareGrid::OnSelectionEvent(wxDataViewEvent& ev)
{
    if (OnFocusChanged) OnFocusChanged();
    ev.Skip();
}

const DiffNode* SyncCompareGrid::FocusedNode() const
{
    if (!view_) return nullptr;
    const wxDataViewItem item = view_->GetSelection();
    if (!item.IsOk()) return nullptr;
    return static_cast<const DiffNode*>(item.GetID());
}

const DiffNode* SyncCompareGrid::FocusedRoutine() const
{
    if (!model_) return nullptr;
    const DiffNode* n = FocusedNode();
    while (n) {
        if (n->kind == DiffNodeKind::Routine) return n;
        // A Table ancestor means we are in the executable half of the tree;
        // stop rather than walk on into the roots.
        if (n->kind == DiffNodeKind::Table) return nullptr;
        const wxDataViewItem parent = model_->GetParent(wxDataViewItem(const_cast<DiffNode*>(n)));
        n = parent.IsOk() ? static_cast<const DiffNode*>(parent.GetID()) : nullptr;
    }
    return nullptr;
}

wxString SyncCompareGrid::FocusedTableId() const
{
    if (!model_) return wxString();
    const DiffNode* n = FocusedNode();
    // Walk up to the Table ancestor: a stable id exists only on Table nodes, so
    // this is also the ONLY way to turn a focused child into an identity —
    // there is no position-based fallback to get wrong.
    while (n) {
        if (n->kind == DiffNodeKind::Table) return n->id;
        const wxDataViewItem parent = model_->GetParent(wxDataViewItem(const_cast<DiffNode*>(n)));
        n = parent.IsOk() ? static_cast<const DiffNode*>(parent.GetID()) : nullptr;
    }
    return wxString();
}

} // namespace ui
