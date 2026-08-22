#include "ui/ScriptLibraryPanel.h"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <functional>

#include "core/ScriptStore.h"
#include "ui/I18n.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {
namespace {

// Human-readable byte size ("—" for unknown). Binary units, matching the
// database-overview grid's formatter.
wxString FmtBytes(long long b)
{
    if (b < 0) return L"—";
    const wchar_t* unit[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return u == 0 ? wxString::Format(L"%lld B", b)
                  : wxString::Format(L"%.1f %s", v, unit[u]);
}

wxString FmtTime(const wxDateTime& t)
{
    return t.IsValid() ? t.Format(L"%Y-%m-%d %H:%M") : wxString(L"—");
}

} // namespace

ScriptLibraryPanel::ScriptLibraryPanel(wxWindow* parent)
    : wxPanel(parent)
{
    SetBackgroundColour(theme::kWhite);
    auto* v = new wxBoxSizer(wxVERTICAL);

    // ---- toolbar: 打开 / 删除 / 刷新 (icon + label, matching the overview toolbar) ----
    auto* tb = new wxBoxSizer(wxHORIZONTAL);
    auto toolBtn = [&](const wxString& label, icons::Glyph g, std::function<void()> fn) {
        auto* b = new wxButton(this, wxID_ANY, label, wxDefaultPosition,
                               wxDefaultSize, wxBORDER_NONE);
        b->SetBitmap(icons::Stroke(g, 16, theme::kTextSecondary, 1.8));
        b->SetBitmapMargins(2, 0);
        b->SetFont(Ui(9.5));
        b->SetBackgroundColour(theme::kWhite);
        b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
        tb->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        return b;
    };
    toolBtn(tr(L"打开"), icons::Glyph::Code,     [this] { OpenSelected(); });
    toolBtn(tr(L"删除"), icons::Glyph::Stop,     [this] { DeleteSelected(); });
    toolBtn(tr(L"刷新"), icons::Glyph::Rollback, [this] { Reload(); });
    v->Add(tb, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    // ---- the script grid ----
    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    list_->AppendColumn(tr(L"名称"),     wxLIST_FORMAT_LEFT,  FromDIP(220));
    list_->AppendColumn(tr(L"文件大小"), wxLIST_FORMAT_RIGHT, FromDIP(100));
    list_->AppendColumn(tr(L"创建时间"), wxLIST_FORMAT_LEFT,  FromDIP(150));
    list_->AppendColumn(tr(L"修改时间"), wxLIST_FORMAT_LEFT,  FromDIP(150));
    list_->AppendColumn(tr(L"归属用户"), wxLIST_FORMAT_LEFT,  FromDIP(140));
    list_->AppendColumn(tr(L"执行用户"), wxLIST_FORMAT_LEFT,  FromDIP(140));

    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { OpenSelected(); });
    list_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent&) {
        wxMenu m;
        wxMenuItem* mo = m.Append(wxID_ANY, tr(L"打开"));
        m.Bind(wxEVT_MENU, [this](wxCommandEvent&) { OpenSelected(); }, mo->GetId());
        m.AppendSeparator();
        wxMenuItem* md = m.Append(wxID_ANY, tr(L"删除"));
        m.Bind(wxEVT_MENU, [this](wxCommandEvent&) { DeleteSelected(); }, md->GetId());
        PopupMenu(&m);
    });
    list_->Bind(wxEVT_SIZE, [this](wxSizeEvent& ev) { StretchColumns(); ev.Skip(); });
    v->Add(list_, 1, wxEXPAND | wxTOP, 6);

    SetSizer(v);
    Reload();
}

void ScriptLibraryPanel::Reload()
{
    if (!list_) return;
    list_->DeleteAllItems();
    long row = 0;
    for (const core::ScriptMeta& m : core::ScriptStore::List()) {
        const long idx = list_->InsertItem(row++, m.name);
        list_->SetItem(idx, 1, FmtBytes(m.sizeBytes));
        list_->SetItem(idx, 2, FmtTime(m.created));
        list_->SetItem(idx, 3, FmtTime(m.modified));
        list_->SetItem(idx, 4, m.owner.IsEmpty() ? L"—" : m.owner);
        list_->SetItem(idx, 5, m.exec.IsEmpty()  ? L"—" : m.exec);
    }
    StretchColumns();
}

wxString ScriptLibraryPanel::SelectedName() const
{
    if (!list_) return wxEmptyString;
    long i = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (i < 0) return wxEmptyString;
    return list_->GetItemText(i);
}

void ScriptLibraryPanel::OpenSelected()
{
    const wxString name = SelectedName();
    if (name.IsEmpty()) return;
    if (onOpen_) onOpen_(name);
}

void ScriptLibraryPanel::DeleteSelected()
{
    const wxString name = SelectedName();
    if (name.IsEmpty()) return;
    if (wxMessageBox(wxString::Format(tr(L"确定删除脚本「%s」吗?此操作不可撤销。"), name),
                     tr(L"删除脚本"), wxYES_NO | wxICON_WARNING,
                     wxGetTopLevelParent(this)) != wxYES)
        return;
    if (onDelete_) onDelete_(name);
    Reload();
}

// Widen the last column so the columns fill the whole list width (no grey gutter).
void ScriptLibraryPanel::StretchColumns()
{
    if (!list_) return;
    const int cols = list_->GetColumnCount();
    if (cols == 0) return;
    int used = 0;
    for (int c = 0; c < cols - 1; ++c) used += list_->GetColumnWidth(c);
    int last = list_->GetClientSize().GetWidth() - used;
    const int minLast = FromDIP(120);
    if (last < minLast) last = minLast;
    list_->SetColumnWidth(cols - 1, last);
}

} // namespace ui
