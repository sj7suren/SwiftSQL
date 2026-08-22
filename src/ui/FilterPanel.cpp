// FilterPanel.cpp — see header. Navicat-style segmented condition rows with a
// per-row AND/OR connective; the value is the only inline field, column/operator
// open a ColumnPicker / wxMenu. BuildWhere groups consecutive AND rows in explicit
// parentheses and joins the groups with OR, so the live preview equals the SQL.
#include "ui/FilterPanel.h"

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/checkbox.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/menu.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/dcbuffer.h>

#include <algorithm>
#include <utility>

#include "ui/ColumnPicker.h"
#include "ui/FlatControls.h"   // InlineDropdown / UnderlineText (shared flat controls)
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

// Flat icon button (hover tint + tooltip) for the 应用 / 清除 actions.
wxButton* MakeIconButton(wxWindow* parent, icons::Glyph g, const wxColour& col,
                         const wxString& tip, std::function<void()> fn)
{
    auto* b = new wxButton(parent, wxID_ANY, wxEmptyString, wxDefaultPosition,
                           wxSize(30, 26), wxBORDER_NONE);
    b->SetBitmap(icons::Stroke(g, 18, col, 1.9));
    b->SetToolTip(tip);
    b->SetBackgroundColour(theme::kMenuBg);
    b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
    b->Bind(wxEVT_ENTER_WINDOW, [b](wxMouseEvent& e) {
        b->SetBackgroundColour(theme::kChromeBtnHover); b->Refresh(); e.Skip();
    });
    b->Bind(wxEVT_LEAVE_WINDOW, [b](wxMouseEvent& e) {
        b->SetBackgroundColour(theme::kMenuBg); b->Refresh(); e.Skip();
    });
    return b;
}

// Operator keys in menu order. The four convenience predicates expand to LIKE with
// auto-wrapped '%'. IS NULL / IS NOT NULL take no value; IN / NOT IN take a list.
const std::vector<wxString>& FilterOps()
{
    static const std::vector<wxString> ops = {
        L"=", L"<>", L">", L">=", L"<", L"<=",
        L"contains", L"not contains", L"starts with", L"ends with",
        L"LIKE", L"NOT LIKE", L"IN", L"NOT IN",
        L"IS NULL", L"IS NOT NULL",
    };
    return ops;
}

inline bool OpTakesNoValue(const wxString& op) { return op == L"IS NULL" || op == L"IS NOT NULL"; }
inline bool OpIsInList(const wxString& op)     { return op == L"IN" || op == L"NOT IN"; }
inline bool OpIsLikeRaw(const wxString& op)    { return op == L"LIKE" || op == L"NOT LIKE"; }
inline bool OpIsConvenience(const wxString& op)
{
    return op == L"contains" || op == L"not contains" ||
           op == L"starts with" || op == L"ends with";
}

wxString OpDisplay(const wxString& op)
{
    if (op == L"contains")     return tr(L"包含");
    if (op == L"not contains") return tr(L"不包含");
    if (op == L"starts with")  return tr(L"开头为");
    if (op == L"ends with")    return tr(L"结尾为");
    return op;
}

// ---- literal helpers (shared with the old panel; escaping lives here) ----
bool LooksNumeric(const wxString& v)
{
    if (v.empty()) return false;
    size_t i = 0;
    if (v[0] == '+' || v[0] == '-') i = 1;
    bool digits = false, dot = false;
    for (; i < v.size(); ++i) {
        const wxUniChar c = v[i];
        if (c >= '0' && c <= '9') digits = true;
        else if (c == '.') { if (dot) return false; dot = true; }
        else return false;
    }
    return digits;
}
// Escaping is dialect-aware and delegated to db::RenderLiteral — the single source
// of truth shared with the dump / data-sync paths. Doing our own e.Replace("'","''")
// here dropped MySQL's backslash escaping, so a value like `abc\` rendered `'abc\'`
// (an unterminated string → injection). Feeding a typed Cell through RenderLiteral
// kills that duplicate implementation and keeps every dialect consistent.
wxString StrLit(const wxString& v, db::Dialect d)
{
    return db::RenderLiteral(db::Cell{ db::CellKind::Text, v }, d);
}
wxString SqlLit(const wxString& v, db::Dialect d)
{
    if (LooksNumeric(v)) return db::RenderLiteral(db::Cell{ db::CellKind::Numeric, v }, d);
    if (v.CmpNoCase(L"NULL") == 0) return L"NULL";
    return StrLit(v, d);
}
wxString InItem(const wxString& v, db::Dialect d)
{
    return LooksNumeric(v) ? db::RenderLiteral(db::Cell{ db::CellKind::Numeric, v }, d)
                           : StrLit(v, d);
}

wxString Join(const std::vector<wxString>& xs, const wxString& sep)
{
    wxString out;
    for (size_t i = 0; i < xs.size(); ++i) { if (i) out += sep; out += xs[i]; }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
FilterPanel::FilterPanel(wxWindow* parent, std::function<void(const wxString&)> onApply)
    : wxPanel(parent, wxID_ANY)
    , onApply_(std::move(onApply))
{
    SetBackgroundColour(theme::kMenuBg);
    picker_ = new ColumnPicker(this);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- top bar: 筛选条件 · (stretch) · 应用 · 清除 ----
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* hdr = new wxStaticText(this, wxID_ANY, tr(L"筛选条件"));
    hdr->SetFont(ui::Ui(9, /*bold*/ true));
    hdr->SetForegroundColour(theme::kTextSecondary);
    bar->Add(hdr, 0, wxALIGN_CENTER_VERTICAL);
    bar->AddStretchSpacer(1);
    bar->Add(MakeIconButton(this, icons::Glyph::Commit, theme::kGreen, tr(L"应用"),
                            [this] { Apply(); }),
             0, wxLEFT | wxALIGN_CENTER_VERTICAL, 8);
    bar->Add(MakeIconButton(this, icons::Glyph::Close, theme::kDotRed, tr(L"清除"),
                            [this] { ResetToOneRow(); if (onApply_) onApply_(wxString()); }),
             0, wxLEFT | wxALIGN_CENTER_VERTICAL, 4);
    root->Add(bar, 0, wxEXPAND | wxALL, 8);

    // ---- scrollable stack of condition rows ----
    rowsHost_ = new wxScrolledWindow(this, wxID_ANY);
    rowsHost_->SetBackgroundColour(theme::kMenuBg);
    rowsHost_->SetScrollRate(0, 12);
    rowsHost_->SetSizer(new wxBoxSizer(wxVERTICAL));
    rowsHost_->SetMinSize(wxSize(-1, 96));   // floor height so the panel claims space
    root->Add(rowsHost_, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

    // ---- read-only live SQL preview ----
    preview_ = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                wxDefaultSize, wxST_ELLIPSIZE_END);
    preview_->SetFont(ui::Mono(9));
    preview_->SetForegroundColour(theme::kTextMuted);
    root->Add(preview_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, 8);

    SetSizer(root);

    // Esc collapses the panel (relayout the grid host so it reclaims the space).
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) { Hide(); if (GetParent()) GetParent()->Layout(); }
        else e.Skip();
    });

    AddRow();          // always start with one empty row
    UpdatePreview();
}

FilterPanel::~FilterPanel()
{
    for (Row* r : rows_) delete r;
}

int FilterPanel::IndexOf(const Row* r) const
{
    for (size_t i = 0; i < rows_.size(); ++i) if (rows_[i] == r) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------
FilterPanel::Row* FilterPanel::AddRow(int afterIndex)
{
    Row* r = new Row();
    auto* host = new wxPanel(rowsHost_, wxID_ANY);
    host->SetBackgroundColour(theme::kMenuBg);
    r->host = host;
    auto* s = new wxBoxSizer(wxHORIZONTAL);

    r->enable = new wxCheckBox(host, wxID_ANY, wxEmptyString);
    r->enable->SetValue(true);
    r->enable->Bind(wxEVT_CHECKBOX, [this, r](wxCommandEvent&) {
        r->enabled = r->enable->GetValue(); UpdateRowState(r); UpdatePreview();
    });
    s->Add(r->enable, 0, wxALIGN_CENTER_VERTICAL);

    // Field + operator: borderless "text ▾ + underline" segments that fit content.
    r->colField = new InlineDropdown(host, [this, r] { OpenPicker(r); });
    s->Add(r->colField, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 4);

    r->opField = new InlineDropdown(host, [this, r] { ShowOpMenu(r); });
    s->Add(r->opField, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 8);

    // Value: borderless input with an underline; fills the remaining width.
    r->valHost = new UnderlineText(host);
    r->val = r->valHost->Text();
    r->val->SetFont(ui::Ui(9));
    r->val->Bind(wxEVT_TEXT,       [this, r](wxCommandEvent&) { UpdateRowState(r); UpdatePreview(); });
    r->val->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Apply(); });
    s->Add(r->valHost, 1, wxLEFT | wxALIGN_CENTER_VERTICAL, 8);

    r->warn = new wxStaticText(host, wxID_ANY, L"!");
    r->warn->SetFont(ui::Ui(10, /*bold*/ true));
    r->warn->SetForegroundColour(theme::kDotAmber);
    r->warn->SetToolTip(tr(L"未填写值，生成时会跳过此行"));
    s->Add(r->warn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 2);

    r->connBtn = new wxButton(host, wxID_ANY, L"and", wxDefaultPosition, wxSize(42, -1),
                              wxBU_EXACTFIT);
    r->connBtn->SetFont(ui::Ui(8));
    r->connBtn->SetToolTip(tr(L"与下一条的连接词 (点击切换 AND/OR)"));
    r->connBtn->Bind(wxEVT_BUTTON, [this, r](wxCommandEvent&) {
        r->connAnd = !r->connAnd;
        r->connBtn->SetLabel(r->connAnd ? L"and" : L"or");
        UpdatePreview();
    });
    s->Add(r->connBtn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 6);

    r->addBtn = new wxButton(host, wxID_ANY, L"+", wxDefaultPosition, wxSize(26, -1),
                             wxBU_EXACTFIT);
    r->addBtn->SetFont(ui::Ui(9));
    r->addBtn->SetToolTip(tr(L"在下方添加条件"));
    r->addBtn->Bind(wxEVT_BUTTON, [this, r](wxCommandEvent&) {
        Row* nr = AddRow(IndexOf(r)); OpenPicker(nr);
    });
    s->Add(r->addBtn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 6);

    r->groupBtn = new wxButton(host, wxID_ANY, L"( )", wxDefaultPosition, wxSize(30, -1),
                               wxBU_EXACTFIT);
    r->groupBtn->SetFont(ui::Ui(8));
    r->groupBtn->SetToolTip(tr(L"手动分组 (即将支持)"));
    r->groupBtn->Enable(false);          // P2 — greyed
    s->Add(r->groupBtn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 2);

    r->remove = new wxButton(host, wxID_ANY, L"×", wxDefaultPosition, wxSize(26, -1),
                             wxBU_EXACTFIT);
    r->remove->SetFont(ui::Ui(9));
    r->remove->SetToolTip(tr(L"删除此条件"));
    r->remove->Bind(wxEVT_BUTTON, [this, r](wxCommandEvent&) { RemoveRow(r); });
    s->Add(r->remove, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 2);

    host->SetSizer(s);

    const int at = (afterIndex >= 0 && afterIndex < static_cast<int>(rows_.size()))
                 ? afterIndex + 1 : static_cast<int>(rows_.size());
    rows_.insert(rows_.begin() + at, r);

    RefreshColBtn(r);
    RefreshOpBtn(r);
    UpdateRowState(r);
    UpdateConnVisibility();
    Relayout();
    return r;
}

void FilterPanel::RemoveRow(Row* r)
{
    auto it = std::find(rows_.begin(), rows_.end(), r);
    if (it == rows_.end()) return;
    r->host->Destroy();
    rows_.erase(it);
    delete r;
    if (rows_.empty()) AddRow();          // never leave zero rows
    UpdateConnVisibility();
    UpdatePreview();
    Relayout();
}

void FilterPanel::ResetToOneRow()
{
    for (Row* r : rows_) { r->host->Destroy(); delete r; }
    rows_.clear();
    AddRow();
    UpdatePreview();
    Relayout();
}

void FilterPanel::SetColumns(const std::vector<wxString>& cols, db::Dialect dialect)
{
    columns_ = cols;
    dialect_ = dialect;
    for (Row* r : rows_) {
        if (r->col >= static_cast<int>(columns_.size())) r->col = -1;   // stale pick
        RefreshColBtn(r);
        UpdateRowState(r);
    }
    UpdatePreview();
    Relayout();
}

// ---------------------------------------------------------------------------
void FilterPanel::OpenPicker(Row* r)
{
    if (columns_.empty()) return;
    // Filtering allows the same column in several rows → no disabled entries.
    picker_->PopupAt(r->colField, columns_, {}, [this, r](int col) {
        r->col = col;
        RefreshColBtn(r);
        if (r->val) r->val->SetFocus();    // focus falls to the value field
        UpdateRowState(r);
        UpdatePreview();
    });
}

void FilterPanel::ShowOpMenu(Row* r)
{
    wxMenu m;
    for (const wxString& op : FilterOps()) {
        wxMenuItem* mi = m.Append(wxID_ANY, OpDisplay(op));
        m.Bind(wxEVT_MENU, [this, r, op](wxCommandEvent&) {
            r->op = op; RefreshOpBtn(r); UpdateRowState(r); UpdatePreview();
        }, mi->GetId());
    }
    r->opField->PopupMenu(&m);
}

void FilterPanel::RefreshColBtn(Row* r) const
{
    const bool ok = (r->col >= 0 && r->col < static_cast<int>(columns_.size()));
    r->colField->SetText(ok ? columns_[r->col] : tr(L"（选择列）"));   // adds " ▾" itself
}

void FilterPanel::RefreshOpBtn(Row* r) const
{
    r->opField->SetText(OpDisplay(r->op));
}

void FilterPanel::UpdateRowState(Row* r)
{
    const bool noVal = OpTakesNoValue(r->op);
    r->val->Enable(r->enabled && !noVal);
    r->val->SetHint(noVal ? L"—" : L"<?>");
    r->val->SetToolTip(OpIsInList(r->op) ? tr(L"多值请用英文逗号分隔") : wxString());

    // incomplete: enabled, column chosen, value-taking op, but the value is empty
    wxString raw = r->val->GetValue(); raw.Trim(true).Trim(false);
    const bool incomplete = r->enabled && r->col >= 0 && !noVal && raw.IsEmpty();
    r->val->SetBackgroundColour(incomplete ? theme::kDiffModBg : theme::kWhite);
    r->val->Refresh();
    if (r->warn) r->warn->Show(incomplete);

    // disabled row: grey the segments + strike the value through.
    wxFont f = ui::Ui(9); f.SetStrikethrough(!r->enabled);
    r->val->SetFont(f);
    r->colField->SetEnabledLook(r->enabled);
    r->opField->SetEnabledLook(r->enabled);
    r->connBtn->Enable(r->enabled);
    r->host->Layout();
}

void FilterPanel::UpdateConnVisibility()
{
    for (size_t i = 0; i < rows_.size(); ++i)
        rows_[i]->connBtn->Show(i + 1 < rows_.size());   // last row has no "next"
}

void FilterPanel::Relayout()
{
    auto* sizer = rowsHost_->GetSizer();
    sizer->Clear(/*delete_windows*/ false);
    for (Row* r : rows_) sizer->Add(r->host, 0, wxEXPAND | wxBOTTOM, 6);
    rowsHost_->FitInside();
    rowsHost_->Layout();
    Layout();
}

// ---------------------------------------------------------------------------
// One row → its SQL condition, or "" if the row is disabled / incomplete.
wxString FilterPanel::RowCond(const Row* r) const
{
    if (!r->enabled) return wxString();
    if (r->col < 0 || r->col >= static_cast<int>(columns_.size())) return wxString();
    const wxString qcol = db::QuoteIdent(columns_[r->col], dialect_);
    const wxString op = r->op;
    wxString raw = r->val->GetValue(); raw.Trim(true).Trim(false);

    if (OpTakesNoValue(op)) return qcol + L" " + op;

    if (OpIsInList(op)) {
        std::vector<wxString> items; wxString cur;
        for (size_t i = 0; i <= raw.size(); ++i) {
            if (i == raw.size() || raw[i] == ',') {
                cur.Trim(true).Trim(false);
                if (!cur.IsEmpty()) items.push_back(InItem(cur, dialect_));
                cur.clear();
            } else cur += raw[i];
        }
        if (items.empty()) return wxString();
        return qcol + L" " + op + L" (" + Join(items, L", ") + L")";
    }

    if (raw.IsEmpty()) return wxString();      // every remaining op needs a value

    if (OpIsLikeRaw(op)) return qcol + L" " + op + L" " + StrLit(raw, dialect_);  // user types own %

    // convenience predicates → LIKE with auto-wrapped % (quoting via StrLit)
    if (op == L"contains")     return qcol + L" LIKE "     + StrLit(L"%" + raw + L"%", dialect_);
    if (op == L"not contains") return qcol + L" NOT LIKE " + StrLit(L"%" + raw + L"%", dialect_);
    if (op == L"starts with")  return qcol + L" LIKE "     + StrLit(raw + L"%", dialect_);
    if (op == L"ends with")    return qcol + L" LIKE "     + StrLit(L"%" + raw, dialect_);

    return qcol + L" " + op + L" " + SqlLit(raw, dialect_);   // = <> > >= < <=
}

// Assemble the WHERE: group consecutive AND rows, wrap multi-condition AND groups
// in parens, join the groups with OR. Single group (no OR) → no outer parens.
wxString FilterPanel::AssembleWhere() const
{
    struct E { wxString cond; bool connAnd; };
    std::vector<E> es;
    for (const Row* r : rows_) {
        const wxString c = RowCond(r);
        if (!c.IsEmpty()) es.push_back({ c, r->connAnd });   // connAnd = relation to NEXT valid row
    }
    if (es.empty()) return wxString();

    std::vector<std::vector<wxString>> segs;
    std::vector<wxString> cur{ es[0].cond };
    for (size_t i = 1; i < es.size(); ++i) {
        if (es[i - 1].connAnd) cur.push_back(es[i].cond);    // stays in the AND run
        else { segs.push_back(cur); cur = { es[i].cond }; }  // OR → new group
    }
    segs.push_back(cur);

    if (segs.size() == 1) return Join(segs[0], L" AND ");     // no OR → no parens needed
    std::vector<wxString> parts;
    for (const auto& seg : segs)
        parts.push_back(seg.size() == 1 ? seg[0] : (L"(" + Join(seg, L" AND ") + L")"));
    return Join(parts, L" OR ");
}

void FilterPanel::UpdatePreview()
{
    if (!preview_) return;
    const wxString w = AssembleWhere();
    preview_->SetLabel(w.IsEmpty() ? tr(L"（无条件）") : (L"WHERE " + w));
}

void FilterPanel::Apply()
{
    if (onApply_) onApply_(AssembleWhere());
}

} // namespace ui
