// TableDesignView_Trigger.cpp — 触发器 tab of the table design view, split out per the
// ≤1000-line charter. A master/detail editor: the top is the same always-live grid the
// 索引/外键 tabs use (序号/名称/时机/事件, timing/event via PopupListCell), the bottom is
// a multiline body editor bound to the current row. Add-only: db::IConnection exposes no
// trigger introspection, so existing triggers are NOT loaded — the tab only ADDs (each
// row → TriggerEdit::Add), and the 上移/下移 buttons just reorder the pending list.
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

// 触发器-list columns (the body is the detail editor below, not a column).
enum TrigCol { TRG_Num = 0, TRG_Name, TRG_Timing, TRG_Event, TRG_Count };
const wchar_t* kTrigHeaderCols[TRG_Count] = { L"序号", L"名称", L"时机", L"事件" };
const int kTrigColMin[TRG_Count] = { 42, 220, 130, 130 };
inline bool TrigColLeft(int col) { return col == TRG_Name; }

} // namespace

// ---- 触发器 tab implementation (M3d) ---------------------------------------

// One-time 触发器 tab construction (invoked from the ctor): a vertical split — the
// always-live list (same flexgrid + grid-line paint as 索引/外键) over a multiline body
// editor. Kept here so TRG_Count / kTrigColMin stay local to this TU.
void TableDesignView::BuildTriggerGrid()
{
    triggersPage_ = new wxPanel(book_);
    triggersPage_->SetBackgroundColour(theme::kWhite);
    auto* col = new wxBoxSizer(wxVERTICAL);

    triggerScroll_ = new wxScrolledWindow(triggersPage_, wxID_ANY);
    triggerScroll_->SetBackgroundColour(theme::kWhite);
    triggerScroll_->SetScrollRate(12, 12);
    triggerSizer_ = new wxFlexGridSizer(TRG_Count, /*vgap*/1, /*hgap*/1);
    triggerScroll_->SetSizer(triggerSizer_);
    triggerScroll_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(triggerScroll_);
        triggerScroll_->DoPrepareDC(dc);
        const wxSize rows = triggerSizer_->GetMinSize();
        dc.SetBrush(wxBrush(theme::kBorderGrid));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, rows.x + 1, rows.y + 1);
    });
    colWTrig_.assign(kTrigColMin, kTrigColMin + TRG_Count);
    BuildTriggerHeaderRow();
    col->Add(triggerScroll_, 3, wxEXPAND | wxALL, 6);

    auto* bodyLbl = new wxStaticText(triggersPage_, wxID_ANY, tr(L"触发器主体"));
    bodyLbl->SetFont(Ui(9));
    bodyLbl->SetForegroundColour(theme::kTextSecondary);
    col->Add(bodyLbl, 0, wxLEFT | wxTOP, 10);

    triggerBody_ = new wxTextCtrl(triggersPage_, wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
    triggerBody_->SetFont(Mono(9.5));
    triggerBody_->SetForegroundColour(theme::kTextBody);
    triggerBody_->SetHint(L"BEGIN ... END");
    triggerBody_->Enable(false);   // no current row yet
    col->Add(triggerBody_, 2, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    triggersPage_->SetSizer(col);
}

void TableDesignView::BuildTriggerHeaderRow()
{
    trigHeaderCells_.assign(TRG_Count, nullptr);
    for (int i = 0; i < TRG_Count; ++i) {
        auto* h = new HeaderCell(triggerScroll_, tr(kTrigHeaderCols[i]), /*centred*/!TrigColLeft(i),
            i, colWTrig_[i],
            [this](int c, int w) {
                if (c < 0 || c >= TRG_Count) return;
                w = std::max(38, w);
                if (w == colWTrig_[c]) return;
                colWTrig_[c] = w;
                triggerScroll_->Freeze();
                if (trigHeaderCells_[c]) trigHeaderCells_[c]->SetMinSize(wxSize(w, kHeaderH));
                for (auto& up : triggerRows_) {
                    wxWindow* cells[TRG_Count] = { up->num, up->name, up->timing, up->event };
                    if (cells[c]) cells[c]->SetMinSize(wxSize(w, kRowH));
                }
                triggerSizer_->Layout(); triggerScroll_->FitInside(); triggerScroll_->Refresh();
                triggerScroll_->Thaw();
            });
        trigHeaderCells_[i] = h;
        triggerSizer_->Add(h, 0, wxEXPAND);
    }
}

TableDesignView::TriggerRow* TableDesignView::AddTriggerRowControls()
{
    auto up = std::make_unique<TriggerRow>();
    TriggerRow* p = up.get();
    wxWindow* host = triggerScroll_;

    auto* numCell = new NumCell(host,
        [this, p](wxMouseEvent&) { const int r = TriggerRowIndexOf(p); if (r >= 0) SetCurrentTriggerRow(r); },
        [this, p](wxMouseEvent&) {
            const int r = TriggerRowIndexOf(p); if (r < 0) return;
            SetCurrentTriggerRow(r);
            wxMenu m; m.Append(1, tr(L"删除触发器"));
            m.Bind(wxEVT_MENU, [this](wxCommandEvent&) { OnDeleteTrigger(); }, 1);
            PopupMenu(&m);
        });
    numCell->SetMinSize(wxSize(colWTrig_[TRG_Num], kRowH));
    numCell->SetNumber(static_cast<int>(triggerRows_.size()) + 1);
    p->num = numCell; triggerSizer_->Add(p->num, 0, wxEXPAND);

    auto* name = new wxTextCtrl(host, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                wxDefaultSize, wxBORDER_NONE | wxTE_LEFT);
    name->SetMinSize(wxSize(colWTrig_[TRG_Name], kRowH));
    name->SetBackgroundColour(theme::kWhite);
    name->SetForegroundColour(theme::kTextBody);
    name->SetFont(Mono(9.5));
    p->name = name; triggerSizer_->Add(p->name, 0, wxEXPAND);

    // 时机 / 事件: closed enums (required — no "" placeholder); seed the first candidate.
    auto makeEnum = [&](int cc, const std::vector<wxString>& cands) {
        auto* cell = new PopupListCell(host, [] {});
        wxArrayString disp; for (const auto& s : cands) disp.Add(s);
        cell->SetChoices(disp, disp);
        if (!disp.IsEmpty()) cell->SetStored(disp[0]);
        cell->SetMinSize(wxSize(colWTrig_[cc], kRowH));
        return cell;
    };
    p->timing = makeEnum(TRG_Timing, profile_ ? profile_->TriggerTimings() : std::vector<wxString>{});
    triggerSizer_->Add(p->timing, 0, wxEXPAND);
    p->event = makeEnum(TRG_Event, profile_ ? profile_->TriggerEvents() : std::vector<wxString>{});
    triggerSizer_->Add(p->event, 0, wxEXPAND);

    triggerRows_.push_back(std::move(up));
    return p;
}

int TableDesignView::TriggerRowIndexOf(const TriggerRow* tr) const
{
    for (int i = 0; i < static_cast<int>(triggerRows_.size()); ++i)
        if (triggerRows_[i].get() == tr) return i;
    return -1;
}

// Switch the detail editor's binding: flush the outgoing row's body into its model, then
// load the incoming row's body (and (dis)enable the editor when no row is current).
void TableDesignView::SetCurrentTriggerRow(int row)
{
    if (curTrigRow_ >= 0 && curTrigRow_ < static_cast<int>(triggerRows_.size()) && triggerBody_)
        triggerRows_[curTrigRow_]->body = triggerBody_->GetValue();
    curTrigRow_ = row;
    for (int i = 0; i < static_cast<int>(triggerRows_.size()); ++i)
        static_cast<NumCell*>(triggerRows_[i]->num)->SetState(i == row, false);
    if (triggerBody_) {
        const bool ok = row >= 0 && row < static_cast<int>(triggerRows_.size());
        triggerBody_->Enable(ok);
        triggerBody_->ChangeValue(ok ? triggerRows_[row]->body : wxString());
    }
}

void TableDesignView::RenumberTriggerRows()
{
    for (int i = 0; i < static_cast<int>(triggerRows_.size()); ++i)
        static_cast<NumCell*>(triggerRows_[i]->num)->SetNumber(i + 1);
}

db::TriggerModel TableDesignView::TriggerRowToModel(int row) const
{
    db::TriggerModel m;
    const TriggerRow* p = triggerRows_[row].get();
    m.name = p->name->GetValue(); m.name.Trim(true).Trim(false);
    m.timing = static_cast<PopupListCell*>(p->timing)->GetStored();
    m.event  = static_cast<PopupListCell*>(p->event)->GetStored();
    // The current row's live body lives in the editor; others in their stored string.
    m.body = (row == curTrigRow_ && triggerBody_) ? triggerBody_->GetValue() : p->body;
    m.body.Trim(true).Trim(false);
    m.origName = p->origName;   // always "" (add-only)
    return m;
}

bool TableDesignView::BuildTriggerEdits(db::TableEdit& edit, wxString& err) const
{
    edit.db = db_; edit.table = table_;
    edit.triggers.clear();
    for (int r = 0; r < static_cast<int>(triggerRows_.size()); ++r) {
        db::TriggerModel m = TriggerRowToModel(r);
        if (m.name.IsEmpty() && m.body.IsEmpty()) continue;   // blank row
        if (m.name.IsEmpty() || m.timing.IsEmpty() || m.event.IsEmpty() || m.body.IsEmpty()) {
            err = wxString::Format(tr(L"第 %d 行:触发器名 / 时机 / 事件 / 主体不能为空"), r + 1);
            return false;
        }
        db::TriggerEdit te; te.op = db::TriggerEdit::Add; te.model = m;
        edit.triggers.push_back(te);
    }
    return true;
}

void TableDesignView::SaveTriggerChanges()
{
    if (!conn_ || !profile_ || table_.IsEmpty()) return;
    // Fold the editor's pending text into the current row before diffing.
    if (curTrigRow_ >= 0 && curTrigRow_ < static_cast<int>(triggerRows_.size()) && triggerBody_)
        triggerRows_[curTrigRow_]->body = triggerBody_->GetValue();

    wxString err;
    db::TableEdit edit;
    if (!BuildTriggerEdits(edit, err)) {
        wxMessageBox(err, tr(L"保存触发器"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (edit.triggers.empty()) {
        wxMessageBox(tr(L"没有检测到可新增的触发器"), tr(L"保存触发器"),
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    ExecuteEdit(edit);
}

// Append the pending triggers to a whole-table snapshot (⑦TABLE DDL). BuildTableModel
// helper — kept here so the trigger collection stays in this TU.
void TableDesignView::AppendTriggerModels(db::TableModel& model) const
{
    for (int r = 0; r < static_cast<int>(triggerRows_.size()); ++r) {
        db::TriggerModel m = TriggerRowToModel(r);
        if (m.name.IsEmpty() || m.timing.IsEmpty() || m.event.IsEmpty() || m.body.IsEmpty())
            continue;
        model.triggers.push_back(std::move(m));
    }
}

// Clear the trigger list + detail editor (table switch / empty). Add-only means Load
// never repopulates, so we simply start each table with a clean, empty tab.
void TableDesignView::ResetTriggerGrid()
{
    if (!triggerSizer_) return;
    triggerScroll_->Freeze();
    triggerSizer_->Clear(/*delete_windows=*/true);
    triggerRows_.clear();
    curTrigRow_ = -1;
    BuildTriggerHeaderRow();
    triggerScroll_->FitInside();
    triggerScroll_->Thaw();
    if (triggerBody_) { triggerBody_->ChangeValue(wxString()); triggerBody_->Enable(false); }
}

void TableDesignView::OnAddTrigger()
{
    if (!triggerScroll_) return;
    triggerScroll_->Freeze();
    AddTriggerRowControls();
    triggerSizer_->Layout();
    triggerScroll_->FitInside();
    triggerScroll_->Thaw();
    const int last = static_cast<int>(triggerRows_.size()) - 1;
    SetCurrentTriggerRow(last);
    if (last >= 0 && triggerRows_[last]->name) triggerRows_[last]->name->SetFocus();
}

void TableDesignView::OnDeleteTrigger()
{
    if (curTrigRow_ < 0 || curTrigRow_ >= static_cast<int>(triggerRows_.size())) return;
    const int gone = curTrigRow_;
    TriggerRow* p = triggerRows_[gone].get();
    wxWindow* cells[TRG_Count] = { p->num, p->name, p->timing, p->event };
    for (wxWindow* c : cells) { if (c) { triggerSizer_->Detach(c); c->Destroy(); } }
    triggerRows_.erase(triggerRows_.begin() + gone);
    curTrigRow_ = -1;                                   // deleted row is gone → no stale flush
    RenumberTriggerRows();
    triggerSizer_->Layout();
    triggerScroll_->FitInside();
    triggerScroll_->Refresh();
    const int n = static_cast<int>(triggerRows_.size());
    SetCurrentTriggerRow(n == 0 ? -1 : std::min(gone, n - 1));
}

// Reorder the current trigger row (上移 delta=-1 / 下移 delta=+1): swap it with its
// neighbour in both the sizer and the backing vector, keeping each row's controls
// (and their live body strings) intact — a pure list-order concern.
void TableDesignView::OnMoveTrigger(int delta)
{
    const int n = static_cast<int>(triggerRows_.size());
    if (curTrigRow_ < 0 || curTrigRow_ >= n) return;
    const int a = (delta < 0) ? curTrigRow_ - 1 : curTrigRow_;   // lower index of the pair
    if (a < 0 || a + 1 >= n) return;

    // Flush the live editor into the current row so the swap carries its edited body.
    if (triggerBody_) triggerRows_[curTrigRow_]->body = triggerBody_->GetValue();

    triggerScroll_->Freeze();
    triggerSizer_->Clear(/*delete_windows=*/false);   // detach all items, keep the windows
    for (wxWindow* h : trigHeaderCells_)              // re-add the EXISTING header cells
        if (h) triggerSizer_->Add(h, 0, wxEXPAND);
    std::swap(triggerRows_[a], triggerRows_[a + 1]);
    for (auto& up : triggerRows_) {                    // re-add the rows in the new order
        wxWindow* cells[TRG_Count] = { up->num, up->name, up->timing, up->event };
        for (wxWindow* c : cells) triggerSizer_->Add(c, 0, wxEXPAND);
    }
    RenumberTriggerRows();
    triggerSizer_->Layout();
    triggerScroll_->FitInside();
    triggerScroll_->Refresh();
    triggerScroll_->Thaw();
    SetCurrentTriggerRow(a + (delta < 0 ? 0 : 1));
}

} // namespace ui
