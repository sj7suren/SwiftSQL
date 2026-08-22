// RoutineExecDialog.cpp — see header.
#include "ui/RoutineExecDialog.h"

#include <wx/wx.h>
#include <wx/grid.h>

#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

// Parameter type labels (index-stable). 0 文本 / 1 数字 / 2 小数 / 3 集合.
// Kept in one place so the grid choice editor and the value formatter agree.
enum ParamType { kText = 0, kInteger = 1, kDecimal = 2, kSet = 3 };

wxArrayString TypeLabels()
{
    wxArrayString a;
    a.Add(tr(L"文本"));
    a.Add(tr(L"数字"));
    a.Add(tr(L"小数"));
    a.Add(tr(L"集合(SET/ARRAY)"));
    return a;
}

// Escape and single-quote a text literal (embedded ' → '').
wxString QuoteText(const wxString& raw)
{
    wxString e = raw;
    e.Replace(L"'", L"''");
    return L"'" + e + L"'";
}

// Format one argument by its type. Empty value → NULL for every type.
wxString FormatArg(int type, const wxString& rawIn)
{
    const wxString raw = wxString(rawIn).Trim(true).Trim(false);
    if (raw.IsEmpty()) return L"NULL";
    switch (type) {
    case kText:    return QuoteText(raw);
    case kInteger: // fallthrough — numbers are emitted bare (no quoting)
    case kDecimal: return raw;
    case kSet:
    default:
        // Collection literal syntax differs per engine (MySQL SET is a quoted
        // 'a,b' string; PostgreSQL ARRAY is ARRAY[…] or '{…}'). Passed through as
        // typed — the user supplies a valid literal. See header note.
        return raw;
    }
}

} // namespace

RoutineExecDialog::RoutineExecDialog(wxWindow* parent, const wxString& routineName,
                                     bool isProcedure, db::Dialect dialect)
    : CenteredDialog(parent, wxID_ANY, tr(L"执行") + L" — " + routineName,
                     wxDefaultPosition, wxSize(520, 420),
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , routine_(routineName)
    , isProcedure_(isProcedure)
    , dialect_(dialect)
{
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxStaticText(this, wxID_ANY,
        (isProcedure ? tr(L"执行存储过程") : tr(L"执行函数")) + L"  " + routineName);
    wxFont hf = header->GetFont(); hf.MakeBold(); header->SetFont(hf);
    root->Add(header, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    auto* hint = new wxStaticText(this, wxID_ANY,
        tr(L"按顺序填写每个参数(参数名仅供参考);留空的值按 NULL 传入。"));
    hint->SetForegroundColour(theme::kTextSecondary);
    root->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- parameter table ----
    grid_ = new wxGrid(this, wxID_ANY);
    grid_->CreateGrid(0, 3);
    grid_->SetColLabelValue(0, tr(L"参数名"));
    grid_->SetColLabelValue(1, tr(L"类型"));
    grid_->SetColLabelValue(2, tr(L"值"));
    grid_->SetColSize(0, 150);
    grid_->SetColSize(1, 140);
    grid_->SetColSize(2, 180);
    grid_->SetRowLabelSize(28);
    grid_->DisableDragRowSize();
    grid_->SetColLabelSize(26);
    grid_->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& e) { UpdatePreview(); e.Skip(); });
    root->Add(grid_, 1, wxEXPAND | wxALL, 12);

    // ---- add / remove row ----
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    auto* add = new wxButton(this, wxID_ANY, tr(L"添加参数"));
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddParamRow(); UpdatePreview(); });
    auto* del = new wxButton(this, wxID_ANY, tr(L"删除参数"));
    del->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DeleteSelectedRow(); UpdatePreview(); });
    btnRow->Add(add, 0);
    btnRow->Add(del, 0, wxLEFT, 6);
    root->Add(btnRow, 0, wxLEFT | wxRIGHT, 12);

    // ---- SQL preview ----
    root->Add(new wxStaticText(this, wxID_ANY, tr(L"SQL 预览")), 0,
              wxLEFT | wxRIGHT | wxTOP, 12);
    preview_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxSize(-1, 56), wxTE_MULTILINE | wxTE_READONLY);
    preview_->SetFont(Mono(10));
    root->Add(preview_, 0, wxEXPAND | wxALL, 12);

    if (wxSizer* btns = CreateButtonSizer(wxOK | wxCANCEL))
        root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    Bind(wxEVT_BUTTON, &RoutineExecDialog::OnOk, this, wxID_OK);

    AddParamRow();      // start with one blank parameter row
    UpdatePreview();
    SetSizer(root);
}

void RoutineExecDialog::AddParamRow()
{
    const int r = grid_->GetNumberRows();
    grid_->AppendRows(1);
    grid_->SetCellEditor(r, 1, new wxGridCellChoiceEditor(TypeLabels(), false));
    grid_->SetCellValue(r, 1, TypeLabels()[kText]);   // default type: 文本
    grid_->SetReadOnly(r, 1, false);
}

void RoutineExecDialog::DeleteSelectedRow()
{
    const int n = grid_->GetNumberRows();
    if (n <= 0) return;
    int r = grid_->GetGridCursorRow();
    const wxArrayInt sel = grid_->GetSelectedRows();
    if (!sel.IsEmpty()) r = sel[0];
    if (r < 0 || r >= n) r = n - 1;
    grid_->DeleteRows(r, 1);
}

wxString RoutineExecDialog::BuildStatement() const
{
    const wxArrayString labels = TypeLabels();
    wxString args;
    const int n = grid_->GetNumberRows();
    for (int r = 0; r < n; ++r) {
        const wxString typeLabel = grid_->GetCellValue(r, 1);
        int type = kText;
        const int idx = labels.Index(typeLabel);
        if (idx != wxNOT_FOUND) type = idx;
        if (!args.IsEmpty()) args += L", ";
        args += FormatArg(type, grid_->GetCellValue(r, 2));
    }
    const wxString call = isProcedure_
        ? (L"CALL " + routine_ + L"(" + args + L")")
        : (L"SELECT " + routine_ + L"(" + args + L")");
    return call + L";";
}

void RoutineExecDialog::UpdatePreview()
{
    if (preview_) preview_->SetValue(BuildStatement());
}

void RoutineExecDialog::OnOk(wxCommandEvent& ev)
{
    statement_ = BuildStatement();
    ev.Skip();   // default handler closes with wxID_OK
}

} // namespace ui
