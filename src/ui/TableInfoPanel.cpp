#include "ui/TableInfoPanel.h"

#include <wx/wx.h>
#include <wx/statbmp.h>

#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {
namespace {
wxString GroupInt(long long n)   // thousands-grouped
{
    if (n < 0) return L"—";
    wxString s = wxString::Format(L"%lld", n), out; int c = 0;
    for (int i = static_cast<int>(s.length()) - 1; i >= 0; --i) {
        out.Prepend(s[i]);
        if (++c % 3 == 0 && i > 0) out.Prepend(L',');
    }
    return out;
}
wxString FmtBytes(long long b)   // human size + exact bytes, like Navicat
{
    if (b < 0) return L"—";
    const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = static_cast<double>(b); int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    return (i == 0) ? wxString::Format(L"%lld B", b)
                    : wxString::Format(L"%.2f %s (%s)", v, u[i], GroupInt(b));
}
} // namespace

TableInfoPanel::TableInfoPanel(wxWindow* parent)
    : wxPanel(parent)
{
    SetBackgroundColour(theme::kSidebarBg);
    auto* col = new wxBoxSizer(wxVERTICAL);

    // Title row: a single table glyph + the (ellipsized) table name. Per
    // design-visual the icon lives on the header only — not on every field.
    auto* titleRow = new wxBoxSizer(wxHORIZONTAL);
    auto* icon = new wxStaticBitmap(this, wxID_ANY,
                     icons::Stroke(icons::Glyph::Table, 16, theme::kTextSecondary, 1.8));
    title_ = new wxStaticText(this, wxID_ANY, tr(L"表信息"), wxDefaultPosition,
                              wxDefaultSize, wxST_ELLIPSIZE_END);   // long names truncate
    title_->SetFont(Ui(11, /*bold*/ true));
    title_->SetForegroundColour(theme::kText);
    titleRow->Add(icon,   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    titleRow->Add(title_, 1, wxALIGN_CENTER_VERTICAL);
    col->Add(titleRow, 0, wxEXPAND | wxALL, 12);

    auto* grid = new wxFlexGridSizer(2, 6, 8);
    grid->AddGrowableCol(1, 1);
    auto field = [&](const wxString& label) {
        auto* l = new wxStaticText(this, wxID_ANY, label);
        l->SetFont(Ui(8.5));
        l->SetForegroundColour(theme::kTextSecondary);
        auto* val = new wxStaticText(this, wxID_ANY, L"—");
        val->SetFont(Ui(9));
        val->SetForegroundColour(theme::kText);
        grid->Add(l, 0);
        grid->Add(val, 1, wxEXPAND);
        vals_.push_back(val);
    };
    field(tr(L"行数"));       field(tr(L"引擎"));       field(tr(L"自增"));
    field(tr(L"行格式"));     field(tr(L"数据长度"));   field(tr(L"索引长度"));
    field(tr(L"最大数据长度")); field(tr(L"数据空闲"));   field(tr(L"排序规则"));
    field(tr(L"创建选项"));   field(tr(L"创建时间"));   field(tr(L"修改时间"));
    field(tr(L"检查时间"));   field(tr(L"注释"));
    col->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    SetSizer(col);
}

void TableInfoPanel::SetTitle(const wxString& title)
{
    const wxString t = title.IsEmpty() ? tr(L"表信息") : title;
    title_->SetLabel(t);
    title_->SetToolTip(t);      // full name on hover (the label may be truncated)
}

void TableInfoPanel::Reset()
{
    for (auto* v : vals_) v->SetLabel(L"—");
}

void TableInfoPanel::Populate(const db::TableDetail& d)
{
    if (vals_.size() < 14) return;
    SetTitle(d.name);
    auto txt = [](const wxString& s) { return s.IsEmpty() ? wxString(L"—") : s; };
    vals_[0]->SetLabel(GroupInt(d.rows));
    vals_[1]->SetLabel(txt(d.engine));
    vals_[2]->SetLabel(d.autoIncrement < 0 ? tr(L"无") : GroupInt(d.autoIncrement));
    vals_[3]->SetLabel(txt(d.rowFormat));
    vals_[4]->SetLabel(FmtBytes(d.dataLength));
    vals_[5]->SetLabel(FmtBytes(d.indexLength));
    vals_[6]->SetLabel(FmtBytes(d.maxDataLength));
    vals_[7]->SetLabel(FmtBytes(d.dataFree));
    vals_[8]->SetLabel(txt(d.collation));
    vals_[9]->SetLabel(txt(d.createOptions));
    vals_[10]->SetLabel(txt(d.createdAt));
    vals_[11]->SetLabel(txt(d.updatedAt));
    vals_[12]->SetLabel(txt(d.checkTime));
    vals_[13]->SetLabel(txt(d.comment));
    Layout();
}

} // namespace ui
