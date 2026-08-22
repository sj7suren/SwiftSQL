// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableDesignView_Index.cpp — 索引 tab of the table design view, split out of
// TableDesignView.cpp per the ≤1000-line charter. The method bodies below are the
// exact originals; behavior is unchanged. IX_* column tables + IndexModelsEqual are
// kept file-local here (they are only used by 索引 code).
#include "ui/TableDesignView.h"
#include "ui/DesignGridCells.h"
#include "ui/I18n.h"

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <algorithm>
#include <memory>
#include <set>
#include <vector>

namespace ui {

namespace {

// 索引-form columns (序号 + name/fields/type/method/comment). Parallel to FormCol.
enum IdxCol {
    IX_Num = 0, IX_Name, IX_Cols, IX_Type, IX_Method, IX_Comment, IX_Count
};
const wchar_t* kIdxHeaderCols[IX_Count] = {
    L"序号", L"索引名", L"字段", L"类型", L"方式", L"注释"
};
const int kIdxColMin[IX_Count] = { 42, 160, 220, 110, 90, 200 };
inline bool IdxColLeft(int col) { return col == IX_Name || col == IX_Comment; }

// Two index models are equal iff every rendered attribute matches (columns are
// order-sensitive: a composite (a,b) ≠ (b,a)). Drives the load-time diff.
bool IndexModelsEqual(const db::IndexModel& a, const db::IndexModel& b)
{
    return a.name == b.name && a.type == b.type && a.method == b.method &&
           a.comment == b.comment && a.columns == b.columns;
}

} // namespace

// ---- 索引 tab implementation (M3b) ------------------------------------------

// One-time 索引 tab construction (invoked from the ctor): the scroll host + flexgrid +
// grid-line paint + seed widths + header row. Kept here so IX_Count / kIdxColMin stay
// local to this TU.
void TableDesignView::BuildIndexGrid()
{
    indexScroll_ = new wxScrolledWindow(book_, wxID_ANY);
    indexScroll_->SetBackgroundColour(theme::kWhite);
    indexScroll_->SetScrollRate(12, 12);
    indexSizer_ = new wxFlexGridSizer(IX_Count, /*vgap*/1, /*hgap*/1);
    indexScroll_->SetSizer(indexSizer_);
    indexScroll_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(indexScroll_);
        indexScroll_->DoPrepareDC(dc);
        const wxSize rows = indexSizer_->GetMinSize();
        dc.SetBrush(wxBrush(theme::kBorderGrid));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, rows.x + 1, rows.y + 1);
    });
    colWIdx_.assign(kIdxColMin, kIdxColMin + IX_Count);
    BuildIndexHeaderRow();
}

void TableDesignView::BuildIndexHeaderRow()
{
    idxHeaderCells_.assign(IX_Count, nullptr);
    for (int i = 0; i < IX_Count; ++i) {
        auto* h = new HeaderCell(indexScroll_, tr(kIdxHeaderCols[i]), /*centred*/!IdxColLeft(i),
            i, colWIdx_[i],
            [this](int col, int w) {
                if (col < 0 || col >= IX_Count) return;
                w = std::max(38, w);
                if (w == colWIdx_[col]) return;
                colWIdx_[col] = w;
                indexScroll_->Freeze();
                if (idxHeaderCells_[col]) idxHeaderCells_[col]->SetMinSize(wxSize(w, kHeaderH));
                for (auto& up : idxRows_) {
                    wxWindow* cells[IX_Count] =
                        { up->num, up->name, up->cols, up->type, up->method, up->comment };
                    if (cells[col]) cells[col]->SetMinSize(wxSize(w, kRowH));
                }
                indexSizer_->Layout(); indexScroll_->FitInside(); indexScroll_->Refresh();
                indexScroll_->Thaw();
            });
        idxHeaderCells_[i] = h;
        indexSizer_->Add(h, 0, wxEXPAND);
    }
}

// Live index-column candidates = the current (possibly just-edited) field names, so a
// freshly added column can be indexed in the same batch.
wxArrayString TableDesignView::CurrentColumnNames() const
{
    wxArrayString names;
    for (const auto& up : rows_) {
        wxString n = up->name ? up->name->GetValue() : wxString();
        n.Trim(true).Trim(false);
        if (!n.IsEmpty()) names.Add(n);
    }
    return names;
}

TableDesignView::IndexRow* TableDesignView::AddIndexRowControls()
{
    auto up = std::make_unique<IndexRow>();
    IndexRow* p = up.get();
    wxWindow* host = indexScroll_;

    auto* numCell = new NumCell(host,
        [this, p](wxMouseEvent&) { const int r = IndexRowIndexOf(p); if (r >= 0) SetCurrentIndexRow(r); },
        [this, p](wxMouseEvent&) {
            const int r = IndexRowIndexOf(p); if (r < 0) return;
            SetCurrentIndexRow(r);
            wxMenu m; m.Append(1, tr(L"删除索引"));
            m.Bind(wxEVT_MENU, [this](wxCommandEvent&) { OnDeleteIndex(); }, 1);
            PopupMenu(&m);
        });
    numCell->SetMinSize(wxSize(colWIdx_[IX_Num], kRowH));
    numCell->SetNumber(static_cast<int>(idxRows_.size()) + 1);
    p->num = numCell; indexSizer_->Add(p->num, 0, wxEXPAND);

    auto makeText = [&](int col, long align) {
        auto* tc = new wxTextCtrl(host, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxBORDER_NONE | align);
        tc->SetMinSize(wxSize(colWIdx_[col], kRowH));
        tc->SetBackgroundColour(theme::kWhite);
        tc->SetForegroundColour(theme::kTextBody);
        tc->SetFont(Mono(9.5));
        return tc;
    };

    p->name = makeText(IX_Name, wxTE_LEFT);
    indexSizer_->Add(p->name, 0, wxEXPAND);

    auto* cols = new MultiPickCell(host, [] {});
    cols->SetCandidateProvider([this] { return CurrentColumnNames(); });
    cols->SetMinSize(wxSize(colWIdx_[IX_Cols], kRowH));
    p->cols = cols; indexSizer_->Add(p->cols, 0, wxEXPAND);

    auto* type = new PopupListCell(host, [] {});
    { wxArrayString disp, vals; disp.Add(tr(L"普通")); vals.Add(wxString());
      if (profile_) for (const auto& t : profile_->IndexTypes()) { disp.Add(t); vals.Add(t); }
      type->SetChoices(disp, vals); }
    type->SetMinSize(wxSize(colWIdx_[IX_Type], kRowH));
    p->type = type; indexSizer_->Add(p->type, 0, wxEXPAND);

    auto* method = new PopupListCell(host, [] {});
    { wxArrayString disp, vals; disp.Add(tr(L"默认")); vals.Add(wxString());
      if (profile_) for (const auto& mm : profile_->IndexMethods()) { disp.Add(mm); vals.Add(mm); }
      method->SetChoices(disp, vals); }
    method->SetMinSize(wxSize(colWIdx_[IX_Method], kRowH));
    p->method = method; indexSizer_->Add(p->method, 0, wxEXPAND);

    p->comment = makeText(IX_Comment, wxTE_LEFT);
    indexSizer_->Add(p->comment, 0, wxEXPAND);

    idxRows_.push_back(std::move(up));
    return p;
}

void TableDesignView::PopulateIndexes(const std::vector<db::IndexInfo>& idx)
{
    if (!indexScroll_) return;
    indexScroll_->Freeze();
    indexSizer_->Clear(/*delete_windows=*/true);
    idxRows_.clear();
    origIndexNames_.clear();
    curIdxRow_ = -1;
    BuildIndexHeaderRow();

    for (const auto& ix : idx) {
        if (ix.name.IsSameAs(L"PRIMARY", /*caseSensitive=*/false)) continue;  // PK = field tab
        IndexRow* p = AddIndexRowControls();
        p->name->ChangeValue(ix.name);

        // Split the comma-joined introspection column list (no extra includes).
        std::vector<wxString> cols; wxString cur;
        for (size_t i = 0; i <= ix.columns.size(); ++i) {
            if (i == ix.columns.size() || ix.columns[i] == L',') {
                wxString t = cur; t.Trim(true).Trim(false);
                if (!t.IsEmpty()) cols.push_back(t);
                cur.clear();
            } else cur += ix.columns[i];
        }
        static_cast<MultiPickCell*>(p->cols)->SetSelected(cols);
        static_cast<PopupListCell*>(p->type)->SetStored(ix.unique ? L"UNIQUE" : wxString());
        static_cast<PopupListCell*>(p->method)->SetStored(wxString());

        p->origName = ix.name;
        origIndexNames_.push_back(ix.name);
        p->baseline = IndexRowToModel(static_cast<int>(idxRows_.size()) - 1);
        p->hasBaseline = true;
    }

    indexSizer_->Layout();
    indexScroll_->FitInside();
    indexScroll_->Thaw();
}

int TableDesignView::IndexRowIndexOf(const IndexRow* ir) const
{
    for (int i = 0; i < static_cast<int>(idxRows_.size()); ++i)
        if (idxRows_[i].get() == ir) return i;
    return -1;
}

void TableDesignView::SetCurrentIndexRow(int row)
{
    curIdxRow_ = row;
    for (int i = 0; i < static_cast<int>(idxRows_.size()); ++i)
        static_cast<NumCell*>(idxRows_[i]->num)->SetState(i == row, false);
}

void TableDesignView::RenumberIndexRows()
{
    for (int i = 0; i < static_cast<int>(idxRows_.size()); ++i)
        static_cast<NumCell*>(idxRows_[i]->num)->SetNumber(i + 1);
}

db::IndexModel TableDesignView::IndexRowToModel(int row) const
{
    db::IndexModel m;
    const IndexRow* p = idxRows_[row].get();
    m.name = p->name->GetValue(); m.name.Trim(true).Trim(false);
    m.columns = static_cast<MultiPickCell*>(p->cols)->GetSelected();
    m.type   = static_cast<PopupListCell*>(p->type)->GetStored();
    m.method = static_cast<PopupListCell*>(p->method)->GetStored();
    m.comment = p->comment->GetValue(); m.comment.Trim(true).Trim(false);
    m.origName = p->origName;
    return m;
}

bool TableDesignView::BuildIndexEdits(db::TableEdit& edit, wxString& err) const
{
    edit.db = db_; edit.table = table_;
    edit.indexes.clear();

    std::set<wxString> seen;
    for (int r = 0; r < static_cast<int>(idxRows_.size()); ++r) {
        db::IndexModel m = IndexRowToModel(r);
        if (m.origName.IsEmpty() && m.name.IsEmpty() && m.columns.empty()) continue;  // blank
        if (m.name.IsEmpty() || m.columns.empty()) {
            err = wxString::Format(tr(L"第 %d 行:索引名和字段不能为空"), r + 1);
            return false;
        }
        if (m.origName.IsEmpty()) {                          // new → Add
            db::IndexEdit ie; ie.op = db::IndexEdit::Add; ie.model = m;
            edit.indexes.push_back(ie);
            continue;
        }
        seen.insert(m.origName);
        const IndexRow* p = idxRows_[r].get();
        if (!p->hasBaseline || !IndexModelsEqual(m, p->baseline)) {   // change = Drop + Add
            db::IndexEdit d; d.op = db::IndexEdit::Drop; d.model.name = m.origName;
            edit.indexes.push_back(d);
            db::IndexEdit a; a.op = db::IndexEdit::Add;  a.model = m;
            edit.indexes.push_back(a);
        }
    }
    for (const wxString& name : origIndexNames_)              // removed rows → Drop
        if (seen.find(name) == seen.end()) {
            db::IndexEdit d; d.op = db::IndexEdit::Drop; d.model.name = name;
            edit.indexes.push_back(d);
        }
    return true;
}

void TableDesignView::SaveIndexChanges()
{
    if (!conn_ || !profile_ || table_.IsEmpty()) return;
    wxString err;
    db::TableEdit edit;
    if (!BuildIndexEdits(edit, err)) {
        wxMessageBox(err, tr(L"保存索引"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (edit.indexes.empty()) {
        wxMessageBox(tr(L"没有检测到索引变更"), tr(L"保存索引"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    ExecuteEdit(edit);
}

void TableDesignView::OnAddIndex()
{
    if (!indexScroll_) return;
    indexScroll_->Freeze();
    AddIndexRowControls();
    indexSizer_->Layout();
    indexScroll_->FitInside();
    indexScroll_->Thaw();
    const int last = static_cast<int>(idxRows_.size()) - 1;
    SetCurrentIndexRow(last);
    if (last >= 0 && idxRows_[last]->name) idxRows_[last]->name->SetFocus();
}

void TableDesignView::OnDeleteIndex()
{
    if (curIdxRow_ < 0 || curIdxRow_ >= static_cast<int>(idxRows_.size())) return;
    IndexRow* p = idxRows_[curIdxRow_].get();
    wxWindow* cells[IX_Count] = { p->num, p->name, p->cols, p->type, p->method, p->comment };
    for (wxWindow* c : cells) { if (c) { indexSizer_->Detach(c); c->Destroy(); } }
    idxRows_.erase(idxRows_.begin() + curIdxRow_);
    RenumberIndexRows();
    curIdxRow_ = std::min(curIdxRow_, static_cast<int>(idxRows_.size()) - 1);
    if (curIdxRow_ >= 0) SetCurrentIndexRow(curIdxRow_);
    indexSizer_->Layout();
    indexScroll_->FitInside();
    indexScroll_->Refresh();
}

} // namespace ui
