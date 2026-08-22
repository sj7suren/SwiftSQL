// TableDesignView_Fk.cpp — 外键 tab of the table design view, split out of
// TableDesignView.cpp per the ≤1000-line charter. The method bodies below are the
// exact originals; behavior is unchanged. FK_* column tables are kept file-local here
// (only used by 外键 code).
#include "ui/TableDesignView.h"
#include "ui/DesignGridCells.h"
#include "ui/I18n.h"

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <algorithm>
#include <memory>
#include <vector>

namespace ui {

namespace {

// 外键-form columns.
enum FkCol {
    FK_Num = 0, FK_Name, FK_Cols, FK_RefDb, FK_RefTable, FK_RefCols, FK_OnDel, FK_OnUpd, FK_Count
};
const wchar_t* kFkHeaderCols[FK_Count] = {
    L"序号", L"名称", L"字段", L"引用库", L"引用表", L"引用字段", L"删除时", L"更新时"
};
const int kFkColMin[FK_Count] = { 42, 140, 150, 110, 140, 150, 100, 100 };
inline bool FkColLeft(int col) { return col == FK_Name || col == FK_RefDb || col == FK_RefTable; }

} // namespace

// ---- 外键 tab implementation (M3c) -----------------------------------------

// One-time 外键 tab construction (invoked from the ctor): same machinery as the 索引
// grid. Kept here so FK_Count / kFkColMin stay local to this TU.
void TableDesignView::BuildFkGrid()
{
    fkScroll_ = new wxScrolledWindow(book_, wxID_ANY);
    fkScroll_->SetBackgroundColour(theme::kWhite);
    fkScroll_->SetScrollRate(12, 12);
    fkSizer_ = new wxFlexGridSizer(FK_Count, /*vgap*/1, /*hgap*/1);
    fkScroll_->SetSizer(fkSizer_);
    fkScroll_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(fkScroll_);
        fkScroll_->DoPrepareDC(dc);
        const wxSize rows = fkSizer_->GetMinSize();
        dc.SetBrush(wxBrush(theme::kBorderGrid));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, rows.x + 1, rows.y + 1);
    });
    colWFk_.assign(kFkColMin, kFkColMin + FK_Count);
    BuildFkHeaderRow();
}

void TableDesignView::BuildFkHeaderRow()
{
    fkHeaderCells_.assign(FK_Count, nullptr);
    for (int i = 0; i < FK_Count; ++i) {
        auto* h = new HeaderCell(fkScroll_, tr(kFkHeaderCols[i]), /*centred*/!FkColLeft(i),
            i, colWFk_[i],
            [this](int col, int w) {
                if (col < 0 || col >= FK_Count) return;
                w = std::max(38, w);
                if (w == colWFk_[col]) return;
                colWFk_[col] = w;
                fkScroll_->Freeze();
                if (fkHeaderCells_[col]) fkHeaderCells_[col]->SetMinSize(wxSize(w, kHeaderH));
                for (auto& up : fkRows_) {
                    wxWindow* cells[FK_Count] = { up->num, up->name, up->cols, up->refDb,
                        up->refTable, up->refCols, up->onDelete, up->onUpdate };
                    if (cells[col]) cells[col]->SetMinSize(wxSize(w, kRowH));
                }
                fkSizer_->Layout(); fkScroll_->FitInside(); fkScroll_->Refresh();
                fkScroll_->Thaw();
            });
        fkHeaderCells_[i] = h;
        fkSizer_->Add(h, 0, wxEXPAND);
    }
}

TableDesignView::FkRow* TableDesignView::AddFkRowControls()
{
    auto up = std::make_unique<FkRow>();
    FkRow* p = up.get();
    wxWindow* host = fkScroll_;

    auto* numCell = new NumCell(host,
        [this, p](wxMouseEvent&) { const int r = FkRowIndexOf(p); if (r >= 0) SetCurrentFkRow(r); },
        [this, p](wxMouseEvent&) {
            const int r = FkRowIndexOf(p); if (r < 0) return;
            SetCurrentFkRow(r);
            wxMenu m; m.Append(1, tr(L"删除外键"));
            m.Bind(wxEVT_MENU, [this](wxCommandEvent&) { OnDeleteForeignKey(); }, 1);
            PopupMenu(&m);
        });
    numCell->SetMinSize(wxSize(colWFk_[FK_Num], kRowH));
    numCell->SetNumber(static_cast<int>(fkRows_.size()) + 1);
    p->num = numCell; fkSizer_->Add(p->num, 0, wxEXPAND);

    auto makeText = [&](int col, long align) {
        auto* tc = new wxTextCtrl(host, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxBORDER_NONE | align);
        tc->SetMinSize(wxSize(colWFk_[col], kRowH));
        tc->SetBackgroundColour(theme::kWhite);
        tc->SetForegroundColour(theme::kTextBody);
        tc->SetFont(Mono(9.5));
        return tc;
    };

    p->name = makeText(FK_Name, wxTE_LEFT);
    fkSizer_->Add(p->name, 0, wxEXPAND);

    auto* cols = new MultiPickCell(host, [] {});
    cols->SetCandidateProvider([this] { return CurrentColumnNames(); });
    cols->SetMinSize(wxSize(colWFk_[FK_Cols], kRowH));
    p->cols = cols; fkSizer_->Add(p->cols, 0, wxEXPAND);

    p->refDb = makeText(FK_RefDb, wxTE_LEFT);
    fkSizer_->Add(p->refDb, 0, wxEXPAND);
    p->refTable = makeText(FK_RefTable, wxTE_LEFT);
    fkSizer_->Add(p->refTable, 0, wxEXPAND);

    // 引用字段: candidates fetched live from the typed referenced table on popup open.
    auto* refCols = new MultiPickCell(host, [] {});
    refCols->SetCandidateProvider([this, p]() -> wxArrayString {
        wxArrayString out;
        if (!conn_ || !p->refTable) return out;
        wxString rt = p->refTable->GetValue(); rt.Trim(true).Trim(false);
        if (rt.IsEmpty()) return out;
        wxString rdb = p->refDb ? p->refDb->GetValue() : wxString(); rdb.Trim(true).Trim(false);
        std::vector<db::ColumnInfo> cs; wxString e;
        if (conn_->GetColumns(rdb.IsEmpty() ? db_ : rdb, rt, cs, e))
            for (const auto& c : cs) out.Add(c.name);
        return out;
    });
    refCols->SetMinSize(wxSize(colWFk_[FK_RefCols], kRowH));
    p->refCols = refCols; fkSizer_->Add(p->refCols, 0, wxEXPAND);

    auto makeAction = [&](int col) {
        auto* cell = new PopupListCell(host, [] {});
        wxArrayString disp, vals; disp.Add(tr(L"默认")); vals.Add(wxString());
        if (profile_) for (const auto& a : profile_->FkActions()) { disp.Add(a); vals.Add(a); }
        cell->SetChoices(disp, vals);
        cell->SetMinSize(wxSize(colWFk_[col], kRowH));
        return cell;
    };
    p->onDelete = makeAction(FK_OnDel);
    fkSizer_->Add(p->onDelete, 0, wxEXPAND);
    p->onUpdate = makeAction(FK_OnUpd);
    fkSizer_->Add(p->onUpdate, 0, wxEXPAND);

    fkRows_.push_back(std::move(up));
    return p;
}

void TableDesignView::PopulateForeignKeys(const std::vector<db::ForeignKey>& fks)
{
    if (!fkScroll_) return;
    fkScroll_->Freeze();
    fkSizer_->Clear(/*delete_windows=*/true);
    fkRows_.clear();
    curFkRow_ = -1;
    BuildFkHeaderRow();

    // Introspection is one row per (fromColumn → toColumn) pair with no name; show each
    // as a view-only baseline row (single-column). Adding new FKs is the live path.
    for (const auto& f : fks) {
        FkRow* p = AddFkRowControls();
        static_cast<MultiPickCell*>(p->cols)->SetSelected({ f.fromColumn });
        p->refTable->ChangeValue(f.toTable);
        static_cast<MultiPickCell*>(p->refCols)->SetSelected({ f.toColumn });
        p->baseline = FkRowToModel(static_cast<int>(fkRows_.size()) - 1);
        p->hasBaseline = true;
    }

    fkSizer_->Layout();
    fkScroll_->FitInside();
    fkScroll_->Thaw();
}

int TableDesignView::FkRowIndexOf(const FkRow* fr) const
{
    for (int i = 0; i < static_cast<int>(fkRows_.size()); ++i)
        if (fkRows_[i].get() == fr) return i;
    return -1;
}

void TableDesignView::SetCurrentFkRow(int row)
{
    curFkRow_ = row;
    for (int i = 0; i < static_cast<int>(fkRows_.size()); ++i)
        static_cast<NumCell*>(fkRows_[i]->num)->SetState(i == row, false);
}

void TableDesignView::RenumberFkRows()
{
    for (int i = 0; i < static_cast<int>(fkRows_.size()); ++i)
        static_cast<NumCell*>(fkRows_[i]->num)->SetNumber(i + 1);
}

db::ForeignKeyModel TableDesignView::FkRowToModel(int row) const
{
    db::ForeignKeyModel m;
    const FkRow* p = fkRows_[row].get();
    m.name = p->name->GetValue(); m.name.Trim(true).Trim(false);
    m.columns = static_cast<MultiPickCell*>(p->cols)->GetSelected();
    m.refDb = p->refDb->GetValue(); m.refDb.Trim(true).Trim(false);
    m.refTable = p->refTable->GetValue(); m.refTable.Trim(true).Trim(false);
    m.refColumns = static_cast<MultiPickCell*>(p->refCols)->GetSelected();
    m.onDelete = static_cast<PopupListCell*>(p->onDelete)->GetStored();
    m.onUpdate = static_cast<PopupListCell*>(p->onUpdate)->GetStored();
    m.origName = p->origName;
    return m;
}

bool TableDesignView::BuildFkEdits(db::TableEdit& edit, wxString& err) const
{
    edit.db = db_; edit.table = table_;
    edit.fks.clear();
    for (int r = 0; r < static_cast<int>(fkRows_.size()); ++r) {
        if (fkRows_[r]->hasBaseline) continue;   // loaded FK — view-only (no name to DROP)
        db::ForeignKeyModel m = FkRowToModel(r);
        if (m.name.IsEmpty() && m.columns.empty() && m.refTable.IsEmpty()) continue;  // blank
        if (m.columns.empty() || m.refTable.IsEmpty() || m.refColumns.empty()) {
            err = wxString::Format(tr(L"第 %d 行:字段 / 引用表 / 引用字段不能为空"), r + 1);
            return false;
        }
        if (m.columns.size() != m.refColumns.size()) {
            err = wxString::Format(tr(L"第 %d 行:本表字段数与引用字段数需一致"), r + 1);
            return false;
        }
        db::ForeignKeyEdit fe; fe.op = db::ForeignKeyEdit::Add; fe.model = m;
        edit.fks.push_back(fe);
    }
    return true;
}

void TableDesignView::SaveFkChanges()
{
    if (!conn_ || !profile_ || table_.IsEmpty()) return;
    wxString err;
    db::TableEdit edit;
    if (!BuildFkEdits(edit, err)) {
        wxMessageBox(err, tr(L"保存外键"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (edit.fks.empty()) {
        wxMessageBox(tr(L"没有检测到可新增的外键（既有外键为只读）"), tr(L"保存外键"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    ExecuteEdit(edit);
}

void TableDesignView::OnAddForeignKey()
{
    if (!fkScroll_) return;
    fkScroll_->Freeze();
    AddFkRowControls();
    fkSizer_->Layout();
    fkScroll_->FitInside();
    fkScroll_->Thaw();
    const int last = static_cast<int>(fkRows_.size()) - 1;
    SetCurrentFkRow(last);
    if (last >= 0 && fkRows_[last]->name) fkRows_[last]->name->SetFocus();
}

void TableDesignView::OnDeleteForeignKey()
{
    if (curFkRow_ < 0 || curFkRow_ >= static_cast<int>(fkRows_.size())) return;
    FkRow* p = fkRows_[curFkRow_].get();
    wxWindow* cells[FK_Count] = { p->num, p->name, p->cols, p->refDb, p->refTable,
                                  p->refCols, p->onDelete, p->onUpdate };
    for (wxWindow* c : cells) { if (c) { fkSizer_->Detach(c); c->Destroy(); } }
    fkRows_.erase(fkRows_.begin() + curFkRow_);
    RenumberFkRows();
    curFkRow_ = std::min(curFkRow_, static_cast<int>(fkRows_.size()) - 1);
    if (curFkRow_ >= 0) SetCurrentFkRow(curFkRow_);
    fkSizer_->Layout();
    fkScroll_->FitInside();
    fkScroll_->Refresh();
}

} // namespace ui
