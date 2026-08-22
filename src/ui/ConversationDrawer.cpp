// ConversationDrawer.cpp — see header. Pure-view history column: a "＋ 新对话"
// header button over a scrolled list of ConvRow items, each painted by hand
// (title + relative time + a delete ✕ on the right). Colours come from Theme
// tokens only; all Chinese literals go through tr().
//
// A "批量删除" entry in the header flips the drawer into SELECT MODE: each row grows
// a checkbox, clicking a row toggles its selection (instead of opening it), the
// header swaps to 全选 / 删除(N) / 取消, and confirming fires onDeleteMany once with
// every checked id. Exiting select mode restores click-to-open + per-row ✕.
#include "ui/ConversationDrawer.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/datetime.h>
#include <wx/graphics.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <algorithm>
#include <memory>

#include "ui/ConversationStore.h"
#include "ui/IconFactory.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {

namespace {

constexpr int kDrawerW = 220;
constexpr int kRowH    = 52;
constexpr int kDelBox  = 22;   // hit box for the ✕ on the right
constexpr int kCheckSz = 18;   // checkbox square drawn on the left in select mode
constexpr int kCheckPad = 12;  // left inset of the checkbox / text shift in select mode

// "刚刚 / N 分钟前 / N 小时前 / N 天前 / YYYY-MM-DD" from a unix-seconds stamp.
wxString RelativeTime(long long updatedAt)
{
    if (updatedAt <= 0) return wxString();
    const long long now = static_cast<long long>(wxDateTime::Now().GetTicks());
    const long long d = now - updatedAt;
    if (d < 60)      return tr(L"刚刚");
    if (d < 3600)    return wxString::Format(L"%lld ", d / 60)   + tr(L"分钟前");
    if (d < 86400)   return wxString::Format(L"%lld ", d / 3600) + tr(L"小时前");
    if (d < 7 * 86400) return wxString::Format(L"%lld ", d / 86400) + tr(L"天前");
    return wxDateTime(static_cast<time_t>(updatedAt)).FormatISODate();
}

// A flat text button used by the header strip (批量删除 / 全选 / 删除(N) / 取消). Sizes
// itself to its label + padding; paints a hover pill so it reads as clickable.
class TinyButton : public wxPanel {
public:
    TinyButton(wxWindow* parent, wxString label, std::function<void()> onClick,
               const wxColour& fg = theme::kTextSecondary, bool strong = false)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE)
        , label_(std::move(label))
        , fg_(fg)
        , onClick_(std::move(onClick))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(ui::Ui(9, strong));
        Bind(wxEVT_PAINT, &TinyButton::OnPaint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (onClick_) onClick_(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { hover_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { hover_ = false; Refresh(); e.Skip(); });
    }

    void SetText(const wxString& t)
    {
        if (t == label_) return;
        label_ = t;
        InvalidateBestSize();
        GetParent()->Layout();
        Refresh();
    }

    wxSize DoGetBestSize() const override
    {
        wxCoord tw = 0, th = 0;
        wxClientDC dc(const_cast<TinyButton*>(this));
        dc.SetFont(GetFont());
        dc.GetTextExtent(label_, &tw, &th);
        return wxSize(tw + 20, 32);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(theme::kWhite));
        dc.Clear();
        if (hover_) {
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (gc) {
                gc->SetBrush(wxBrush(theme::kChromeBg));
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->DrawRoundedRectangle(1, 3, sz.x - 2, sz.y - 6, 7);
            }
        }
        dc.SetFont(GetFont());
        dc.SetTextForeground(fg_);
        wxCoord tw = 0, th = 0;
        dc.GetTextExtent(label_, &tw, &th);
        dc.DrawText(label_, (sz.x - tw) / 2, (sz.y - th) / 2);
    }

    wxString label_;
    wxColour fg_;
    bool     hover_ = false;
    std::function<void()> onClick_;
};

// Full-width header entry: a stroked ＋ glyph + "新对话" label on a hover-tinted row.
class NewChatButton : public wxPanel {
public:
    NewChatButton(wxWindow* parent, std::function<void()> onClick)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 40), wxBORDER_NONE)
        , onClick_(std::move(onClick))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(ui::Ui(10));
        Bind(wxEVT_PAINT, &NewChatButton::OnPaint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (onClick_) onClick_(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { hover_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { hover_ = false; Refresh(); e.Skip(); });
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(theme::kWhite));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->SetBrush(wxBrush(hover_ ? theme::kChromeBg : theme::kWhite));
            gc->SetPen(wxPen(theme::kBorderInput, 1));
            gc->DrawRoundedRectangle(1.5, 1.5, sz.x - 3, sz.y - 3, 9);
        }
        dc.DrawBitmap(icons::Stroke(icons::Glyph::Plus, 16, theme::kAccent, 1.9),
                      12, (sz.y - 16) / 2, true);
        dc.SetFont(GetFont());
        dc.SetTextForeground(theme::kText);
        wxCoord tw = 0, th = 0;
        const wxString label = tr(L"新对话");
        dc.GetTextExtent(label, &tw, &th);
        dc.DrawText(label, 36, (sz.y - th) / 2);
    }

    std::function<void()> onClick_;
    bool hover_ = false;
};

// A single conversation row: title (top) + relative time (bottom) + delete ✕.
// In select mode it draws a left-hand checkbox and clicking toggles selection.
class ConvRow : public wxPanel {
public:
    ConvRow(wxWindow* parent, const ConvMeta& meta, bool active,
            bool selectMode, bool selected,
            std::function<void(const wxString&)> onOpen,
            std::function<void(const wxString&)> onDelete,
            std::function<void(const wxString&, bool)> onToggle)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, kRowH), wxBORDER_NONE)
        , meta_(meta)
        , active_(active)
        , selectMode_(selectMode)
        , selected_(selected)
        , onOpen_(std::move(onOpen))
        , onDelete_(std::move(onDelete))
        , onToggle_(std::move(onToggle))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ConvRow::OnPaint, this);
        Bind(wxEVT_LEFT_UP, &ConvRow::OnClick, this);
        Bind(wxEVT_MOTION, &ConvRow::OnMotion, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { hover_ = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { hover_ = false; overX_ = false; Refresh(); e.Skip(); });
    }

    // Driven by 全选/全不选 so we can repaint without a full Reload (keeps scroll).
    void SetSelected(bool sel)
    {
        if (selected_ == sel) return;
        selected_ = sel;
        Refresh();
    }

private:
    wxRect DelRect() const
    {
        const wxSize sz = GetClientSize();
        return wxRect(sz.x - kDelBox - 6, (sz.y - kDelBox) / 2, kDelBox, kDelBox);
    }

    void OnMotion(wxMouseEvent& e)
    {
        if (selectMode_) { e.Skip(); return; }
        const bool over = DelRect().Contains(e.GetPosition());
        if (over != overX_) { overX_ = over; Refresh(); }
        e.Skip();
    }

    void OnClick(wxMouseEvent& e)
    {
        if (selectMode_) {
            selected_ = !selected_;
            Refresh();
            if (onToggle_) onToggle_(meta_.id, selected_);
            return;
        }
        if (DelRect().Contains(e.GetPosition())) {
            if (onDelete_) onDelete_(meta_.id);
            return;
        }
        if (onOpen_) onOpen_(meta_.id);
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(theme::kWhite));
        dc.Clear();

        // Selection / hover pill. In select mode a checked row also tints.
        const bool tint = active_ || hover_ || (selectMode_ && selected_);
        if (tint) {
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (gc) {
                const bool strong = active_ || (selectMode_ && selected_);
                gc->SetBrush(wxBrush(strong ? wxColour(0xEA, 0xF3, 0xFF) : theme::kChromeBg));
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->DrawRoundedRectangle(3.5, 3.5, sz.x - 7, sz.y - 7, 8);
            }
        }

        const int textLeft = 12 + (selectMode_ ? (kCheckSz + kCheckPad) : 0);

        // Checkbox (select mode only).
        if (selectMode_) {
            const int cy = (sz.y - kCheckSz) / 2;
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (gc) {
                gc->SetPen(wxPen(selected_ ? theme::kAccent : theme::kBorderInput, 1.4));
                gc->SetBrush(wxBrush(selected_ ? theme::kAccent : theme::kWhite));
                gc->DrawRoundedRectangle(kCheckPad, cy, kCheckSz, kCheckSz, 4);
                if (selected_) {
                    wxGraphicsPath p = gc->CreatePath();
                    const double x = kCheckPad, y = cy;
                    p.MoveToPoint(x + 4.0,  y + 9.0);
                    p.AddLineToPoint(x + 7.5, y + 12.5);
                    p.AddLineToPoint(x + 14.0, y + 5.5);
                    gc->SetPen(wxPen(theme::kWhite, 2.0));
                    gc->SetBrush(*wxTRANSPARENT_BRUSH);
                    gc->StrokePath(p);
                }
            }
        }

        const bool showX = !selectMode_ && (hover_ || active_);
        const int textRight = sz.x - 12 - (showX ? kDelBox + 2 : 0);

        // Title (primary).
        dc.SetFont(ui::Ui(9, active_));   // bold when this is the open conversation
        dc.SetTextForeground(active_ ? theme::kTextStrong : theme::kText);
        wxString title = meta_.title.IsEmpty() ? tr(L"无标题对话") : meta_.title;
        dc.SetClippingRegion(textLeft, 8, std::max(10, textRight - textLeft), 18);
        dc.DrawText(title, textLeft, 8);
        dc.DestroyClippingRegion();

        // Relative time (secondary).
        dc.SetFont(ui::Ui(8));
        dc.SetTextForeground(theme::kTextMuted);
        dc.DrawText(RelativeTime(meta_.updatedAt), textLeft, 28);

        // Delete ✕ (only while hovered / active, and never in select mode).
        if (showX) {
            const wxRect r = DelRect();
            if (overX_) {
                std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
                if (gc) {
                    gc->SetBrush(wxBrush(theme::kChromeBtnHover));
                    gc->SetPen(*wxTRANSPARENT_PEN);
                    gc->DrawRoundedRectangle(r.x, r.y, r.width, r.height, r.width / 2.0);
                }
            }
            dc.DrawBitmap(icons::Stroke(icons::Glyph::Close, 13,
                                        overX_ ? theme::kDotRed : theme::kTextGhost, 1.7),
                          r.x + (r.width - 13) / 2, r.y + (r.height - 13) / 2, true);
        }
    }

    ConvMeta meta_;
    bool     active_     = false;
    bool     selectMode_ = false;
    bool     selected_   = false;
    bool     hover_      = false;
    bool     overX_      = false;
    std::function<void(const wxString&)>       onOpen_;
    std::function<void(const wxString&)>       onDelete_;
    std::function<void(const wxString&, bool)> onToggle_;
};

} // namespace

ConversationDrawer::ConversationDrawer(wxWindow* parent,
                                       std::function<void()>                   onNew,
                                       std::function<void(const wxString&)>    onOpen,
                                       std::function<void(const wxString&)>    onDelete,
                                       std::function<void(const std::vector<wxString>&)> onDeleteMany,
                                       std::function<void()>                   onCollapse)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(kDrawerW, -1), wxBORDER_NONE)
    , onNew_(std::move(onNew))
    , onOpen_(std::move(onOpen))
    , onDelete_(std::move(onDelete))
    , onDeleteMany_(std::move(onDeleteMany))
    , onCollapse_(std::move(onCollapse))
{
    SetBackgroundColour(theme::kWhite);
    SetMinSize(wxSize(kDrawerW, -1));

    // Self-paint so the drawer can draw a 1px right separator against the transcript.
    // We fill white ourselves, then stroke the rightmost column. The list below reserves
    // that 1px (see the spacer) so the child scroll window never covers the line.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(theme::kWhite));
        dc.Clear();
        dc.SetPen(wxPen(theme::kBorder, 1));
        dc.DrawLine(sz.x - 1, 0, sz.x - 1, sz.y);
    });

    root_ = new wxBoxSizer(wxVERTICAL);
    headerSizer_ = new wxBoxSizer(wxHORIZONTAL);
    root_->Add(headerSizer_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    list_ = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                 wxVSCROLL | wxBORDER_NONE);
    list_->SetBackgroundColour(theme::kWhite);
    list_->SetScrollRate(0, 12);
    list_->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);   // hidden bar; wheel still scrolls
    // Many rows scroll smoothly with a back-buffer (mirrors the transcript).
    list_->SetDoubleBuffered(true);
    listSizer_ = new wxBoxSizer(wxVERTICAL);
    list_->SetSizer(listSizer_);
    // Wrap the list with a 1px right spacer so the panel's painted separator column
    // (x = w-1) is never covered by the scroll window.
    auto* listWrap = new wxBoxSizer(wxHORIZONTAL);
    listWrap->Add(list_, 1, wxEXPAND);
    listWrap->AddSpacer(1);
    root_->Add(listWrap, 1, wxEXPAND | wxTOP, 6);

    SetSizer(root_);
    RebuildHeader();
}

// ----------------------------------------------------------------- header

void ConversationDrawer::RebuildHeader()
{
    if (!headerSizer_) return;
    headerSizer_->Clear(true);   // destroy old header buttons
    delCountBtn_ = nullptr;

    if (!selectMode_) {
        auto* newBtn = new NewChatButton(this, [this] { if (onNew_) onNew_(); });
        headerSizer_->Add(newBtn, 1, wxALIGN_CENTER_VERTICAL);
        auto* batchBtn = new TinyButton(this, tr(L"批量删除"),
                                        [this] { EnterSelectMode(); });
        headerSizer_->Add(batchBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        // "«" — collapse (hide) the whole drawer, freeing the space for the transcript.
        // The drawer lives on the left edge, so a left-pointing chevron reads as "push
        // me away"; the host's onCollapse toggles visibility (mirrors the top 历史 button).
        auto* collapseBtn = new TinyButton(this, L"«",
                                           [this] { if (onCollapse_) onCollapse_(); });
        collapseBtn->SetToolTip(tr(L"收起历史"));
        headerSizer_->Add(collapseBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
    } else {
        auto* allBtn = new TinyButton(this, tr(L"全选"),
                                      [this] { ToggleSelectAll(); });
        headerSizer_->Add(allBtn, 0, wxALIGN_CENTER_VERTICAL);
        headerSizer_->AddStretchSpacer(1);
        auto* del = new TinyButton(this, tr(L"删除"),
                                   [this] { CommitBatchDelete(); }, theme::kDotRed, true);
        delCountBtn_ = del;
        headerSizer_->Add(del, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
        auto* cancel = new TinyButton(this, tr(L"取消"),
                                      [this] { ExitSelectMode(); });
        headerSizer_->Add(cancel, 0, wxALIGN_CENTER_VERTICAL);
        UpdateDeleteLabel();
    }
    root_->Layout();
}

void ConversationDrawer::UpdateDeleteLabel()
{
    if (!delCountBtn_) return;
    static_cast<TinyButton*>(delCountBtn_)->SetText(
        wxString::Format(L"%s(%d)", tr(L"删除"), static_cast<int>(selected_.size())));
}

// ----------------------------------------------------------------- select mode

void ConversationDrawer::EnterSelectMode()
{
    selectMode_ = true;
    selected_.clear();
    RebuildHeader();
    Reload(activeId_);
}

void ConversationDrawer::ExitSelectMode()
{
    selectMode_ = false;
    selected_.clear();
    RebuildHeader();
    Reload(activeId_);
}

bool ConversationDrawer::IsSelected(const wxString& id) const
{
    return std::find(selected_.begin(), selected_.end(), id) != selected_.end();
}

void ConversationDrawer::OnRowToggled(const wxString& id, bool selected)
{
    auto it = std::find(selected_.begin(), selected_.end(), id);
    if (selected) {
        if (it == selected_.end()) selected_.push_back(id);
    } else if (it != selected_.end()) {
        selected_.erase(it);
    }
    UpdateDeleteLabel();
}

void ConversationDrawer::ToggleSelectAll()
{
    const bool selectAll = selected_.size() < allIds_.size();
    selected_ = selectAll ? allIds_ : std::vector<wxString>{};
    for (wxWindow* w : rows_)
        static_cast<ConvRow*>(w)->SetSelected(selectAll);
    UpdateDeleteLabel();
}

void ConversationDrawer::CommitBatchDelete()
{
    if (selected_.empty()) return;
    const int n = static_cast<int>(selected_.size());
    const int r = wxMessageBox(
        wxString::Format(tr(L"确定删除选中的 %d 个对话？"), n),
        tr(L"批量删除"), wxYES_NO | wxICON_QUESTION, this);
    if (r != wxYES) return;

    std::vector<wxString> ids = selected_;
    // Leave select mode BEFORE the host mutates the store, so the host's own
    // RefreshDrawer()/Reload() repaints the fresh list in plain (open-on-click) mode.
    selectMode_ = false;
    selected_.clear();
    RebuildHeader();
    if (onDeleteMany_) onDeleteMany_(ids);
}

// ----------------------------------------------------------------- list

void ConversationDrawer::Reload(const wxString& activeId)
{
    activeId_ = activeId;
    if (!list_ || !listSizer_) return;

    listSizer_->Clear(true);   // destroy old row windows
    rows_.clear();
    allIds_.clear();

    for (const ConvMeta& m : ConversationStore::List()) {
        allIds_.push_back(m.id);
        auto* row = new ConvRow(
            list_, m, m.id == activeId_, selectMode_, IsSelected(m.id),
            [this](const wxString& id) { if (onOpen_) onOpen_(id); },
            [this](const wxString& id) { if (onDelete_) onDelete_(id); },
            [this](const wxString& id, bool sel) { OnRowToggled(id, sel); });
        listSizer_->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
        rows_.push_back(row);
    }

    // Drop any checked ids that no longer exist (e.g. deleted elsewhere).
    if (!selected_.empty()) {
        selected_.erase(
            std::remove_if(selected_.begin(), selected_.end(),
                           [this](const wxString& id) {
                               return std::find(allIds_.begin(), allIds_.end(), id) == allIds_.end();
                           }),
            selected_.end());
        UpdateDeleteLabel();
    }

    listSizer_->Layout();
    list_->FitInside();
    list_->Scroll(0, 0);
}

} // namespace ui
