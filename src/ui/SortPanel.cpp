// SortPanel.cpp — see header. A scrollable stack of sort rows (column · direction ·
// priority), an 应用/清除 bar, and a read-only ORDER BY preview — all flat controls
// (shared FlatControls). The per-row column field opens the searchable ColumnPicker
// so it scales to 50+ columns; direction is a flat ASC/DESC text toggle.
#include "ui/SortPanel.h"

#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>

#include <algorithm>

#include "ui/ColumnPicker.h"
#include "ui/FlatControls.h"   // InlineDropdown / FlatButton (shared flat controls)
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

SortPanel::SortPanel(wxWindow* parent,
                     std::function<void(const std::vector<std::pair<int, bool>>&)> onApply)
    : wxPanel(parent, wxID_ANY)
    , onApply_(std::move(onApply))
{
    SetBackgroundColour(theme::kMenuBg);
    picker_ = new ColumnPicker(this);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- top bar: + 添加排序列 · (stretch) · 应用 · 清除 (all flat) ----
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* addBtn = new FlatButton(this, tr(L"+ 添加排序列"), theme::kTextSecondary, [this] {
        Row* r = AddSortRow();
        OpenPicker(r);   // open the picker at once so the row isn't a blank field
    });
    bar->Add(addBtn, 0, wxALIGN_CENTER_VERTICAL);
    bar->AddStretchSpacer(1);

    auto* applyBtn = new FlatButton(this, icons::Glyph::Commit, theme::kGreen,
                                    [this] { if (onApply_) onApply_(BuildKeys()); });
    applyBtn->SetToolTip(tr(L"应用"));
    bar->Add(applyBtn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 8);

    auto* clearBtn = new FlatButton(this, icons::Glyph::Close, theme::kDotRed,
                                    [this] { SetKeys({}); if (onApply_) onApply_({}); });
    clearBtn->SetToolTip(tr(L"清除"));
    bar->Add(clearBtn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 4);
    root->Add(bar, 0, wxEXPAND | wxALL, 8);

    // ---- scrollable stack of sort rows ----
    rowsHost_ = new wxScrolledWindow(this, wxID_ANY);
    rowsHost_->SetBackgroundColour(theme::kMenuBg);
    rowsHost_->SetScrollRate(0, 12);
    rowsHost_->SetSizer(new wxBoxSizer(wxVERTICAL));
    // Floor height so the panel claims real vertical space when shown; otherwise
    // the scrolled window reports ~0 min height and rows collapse under the grid.
    rowsHost_->SetMinSize(wxSize(-1, 96));
    root->Add(rowsHost_, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

    // ---- read-only ORDER BY preview ----
    preview_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    preview_->SetFont(ui::Mono(9));
    preview_->SetForegroundColour(theme::kTextMuted);
    root->Add(preview_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, 8);

    SetSizer(root);
    AddSortRow();          // always start with one empty row
    UpdatePreview();
}

SortPanel::~SortPanel()
{
    for (Row* r : rows_) delete r;
}

void SortPanel::SetColumns(const std::vector<wxString>& cols, db::Dialect dialect)
{
    columns_ = cols;
    dialect_ = dialect;
    for (Row* r : rows_) {
        if (r->col >= static_cast<int>(columns_.size())) r->col = -1;   // stale → unset
        RefreshColButton(r);
    }
    UpdatePreview();
    Relayout();
}

SortPanel::Row* SortPanel::AddSortRow(int col, bool asc)
{
    Row* r = new Row();
    r->col = col;
    r->asc = asc;

    auto* host = new wxPanel(rowsHost_, wxID_ANY);
    host->SetBackgroundColour(theme::kMenuBg);
    r->host = host;
    auto* s = new wxBoxSizer(wxHORIZONTAL);

    r->badge = new wxStaticText(host, wxID_ANY, L"#1", wxDefaultPosition, wxSize(26, -1));
    r->badge->SetFont(ui::Mono(8));
    r->badge->SetForegroundColour(theme::kTextMuted);
    s->Add(r->badge, 0, wxALIGN_CENTER_VERTICAL);

    // Column: borderless "列名 ▾" → ColumnPicker (searchable). proportion 0 so it
    // fits its content (id narrow, occurred_at wide) — a compact, left-aligned row.
    r->colField = new InlineDropdown(host, [this, r] { OpenPicker(r); });
    s->Add(r->colField, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 6);

    // Direction: a single flat toggle showing the ASC / DESC text (not ▲▼).
    r->dirBtn = new FlatButton(host, r->asc ? L"ASC" : L"DESC", theme::kPrimary,
                               [this, r] { SetRowDir(r, !r->asc); });
    r->dirBtn->SetToolTip(tr(L"升序/降序 (点击切换)"));
    s->Add(r->dirBtn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 8);

    r->upBtn = new FlatButton(host, L"↑", theme::kTextSecondary, [this, r] { MoveRow(r, true); });
    r->upBtn->SetToolTip(tr(L"上移"));
    s->Add(r->upBtn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 6);

    r->downBtn = new FlatButton(host, L"↓", theme::kTextSecondary, [this, r] { MoveRow(r, false); });
    r->downBtn->SetToolTip(tr(L"下移"));
    s->Add(r->downBtn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 2);

    r->remove = new FlatButton(host, L"×", theme::kDotRed, [this, r] { RemoveSortRow(r); });
    r->remove->SetToolTip(tr(L"删除此排序列"));
    s->Add(r->remove, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 6);

    host->SetSizer(s);
    rows_.push_back(r);
    rowsHost_->GetSizer()->Add(host, 0, wxEXPAND | wxBOTTOM, 6);

    RefreshColButton(r);
    SetRowDir(r, asc);
    RenumberBadges();
    rowsHost_->FitInside();
    rowsHost_->Layout();
    Layout();
    return r;
}

void SortPanel::RemoveSortRow(Row* r)
{
    auto it = std::find(rows_.begin(), rows_.end(), r);
    if (it == rows_.end()) return;
    r->host->Destroy();
    rows_.erase(it);
    delete r;
    if (rows_.empty()) AddSortRow();     // never leave zero rows
    RenumberBadges();
    UpdatePreview();
    Relayout();
}

void SortPanel::MoveRow(Row* r, bool up)
{
    auto it = std::find(rows_.begin(), rows_.end(), r);
    if (it == rows_.end()) return;
    const size_t i = static_cast<size_t>(it - rows_.begin());
    if (up && i > 0)                 std::swap(rows_[i], rows_[i - 1]);
    else if (!up && i + 1 < rows_.size()) std::swap(rows_[i], rows_[i + 1]);
    else return;
    RenumberBadges();
    UpdatePreview();
    Relayout();
}

void SortPanel::OpenPicker(Row* r)
{
    if (columns_.empty()) return;
    std::vector<int> disabled;                       // sort disallows the same column twice
    for (Row* o : rows_)
        if (o != r && o->col >= 0) disabled.push_back(o->col);
    picker_->PopupAt(r->colField, columns_, disabled, [this, r](int col) {
        r->col = col;
        RefreshColButton(r);
        UpdatePreview();
    });
}

void SortPanel::SetRowDir(Row* r, bool asc)
{
    r->asc = asc;
    r->dirBtn->SetText(asc ? L"ASC" : L"DESC");   // ASC=true → the ORDER BY direction
    UpdatePreview();
}

void SortPanel::RefreshColButton(Row* r) const
{
    const bool ok = (r->col >= 0 && r->col < static_cast<int>(columns_.size()));
    r->colField->SetText(ok ? columns_[r->col] : tr(L"（选择列）"));   // adds " ▾" itself
}

void SortPanel::RenumberBadges()
{
    for (size_t i = 0; i < rows_.size(); ++i) {
        rows_[i]->badge->SetLabel(wxString::Format(L"#%zu", i + 1));
        rows_[i]->upBtn->SetEnabledLook(i > 0);
        rows_[i]->downBtn->SetEnabledLook(i + 1 < rows_.size());
    }
}

void SortPanel::Relayout()
{
    auto* sizer = rowsHost_->GetSizer();
    sizer->Clear(/*delete_windows*/ false);
    for (Row* r : rows_) sizer->Add(r->host, 0, wxEXPAND | wxBOTTOM, 6);
    rowsHost_->FitInside();
    rowsHost_->Layout();
    Layout();
}

std::vector<std::pair<int, bool>> SortPanel::BuildKeys() const
{
    std::vector<std::pair<int, bool>> keys;
    for (Row* r : rows_) {
        if (r->col < 0) continue;
        bool dup = false;
        for (const auto& k : keys) if (k.first == r->col) { dup = true; break; }
        if (dup) continue;                                   // belt-and-braces dedupe
        keys.emplace_back(r->col, r->asc);
    }
    return keys;
}

void SortPanel::UpdatePreview()
{
    if (!preview_) return;
    const auto keys = BuildKeys();
    if (keys.empty()) { preview_->SetLabel(L"ORDER BY —"); return; }
    wxString ob = L"ORDER BY ";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) ob += L", ";
        const int col = keys[i].first;
        ob += db::QuoteIdent(columns_[col], dialect_) + (keys[i].second ? L" ASC" : L" DESC");
    }
    preview_->SetLabel(ob);
}

void SortPanel::SetKeys(const std::vector<std::pair<int, bool>>& keys)
{
    for (Row* r : rows_) { r->host->Destroy(); delete r; }
    rows_.clear();
    for (const auto& k : keys) AddSortRow(k.first, k.second);
    if (rows_.empty()) AddSortRow();
    RenumberBadges();
    UpdatePreview();
    Relayout();
}

} // namespace ui
