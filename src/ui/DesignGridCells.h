// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// DesignGridCells.h — owner-drawn cell widgets shared by TableDesignView's
// always-live design grids (字段 / 索引 / 外键 tabs). Extracted verbatim from
// TableDesignView.cpp (charter: single file ≤1000 lines) so the field / index / fk
// translation units can share them. Header-only (inline): each cell is a small
// wxWindow / wxTextCtrl subclass that paints a flat white table cell.
//
// NOTE: the field-grid column tables (FormCol / kColMin / …) intentionally stay in
// TableDesignView.cpp — they are NOT moved here. The two cells that used kColMin for
// their default min width now use the equivalent literal (NumCell 42, BadgeCell 46);
// that default is immediately overridden by the caller's SetMinSize either way.
#pragma once

#include <wx/wx.h>
#include <wx/popupwin.h>
#include <wx/listbox.h>
#include <wx/checklst.h>
#include <algorithm>
#include <functional>
#include <vector>

#include "ui/Theme.h"
#include "ui/UiFonts.h"
#include "ui/IconFactory.h"
#include "ui/FlatControls.h"
#include "ui/I18n.h"

namespace ui {

// Row/header cell heights — taller than the controls' natural ~24px. Safe because a
// single-line wxTextCtrl vertically centres its text natively (see FieldRow in the
// header), so forcing the height doesn't top-align the text.
const int kRowH    = 24;   // compact data rows (text vertically centred natively)
const int kHeaderH = 28;
// Header bottom rule: thicker + darker than the 1px kBorderGrid cell lines, so the
// header reads as a header without needing a fill colour behind it.
const int      kHeaderRuleH = 2;
const wxColour kHeaderRule(0xCB, 0xD3, 0xDD);

// Lighten a colour toward white by fraction f (0..1) — the current-row number tint.
inline wxColour Lighten(const wxColour& c, double f)
{
    auto mix = [f](unsigned char ch) {
        return static_cast<unsigned char>(ch + (255 - ch) * f);
    };
    return wxColour(mix(c.Red()), mix(c.Green()), mix(c.Blue()));
}

// Owner-drawn header cell: white (no fill), muted bold label, and the thick bottom
// rule. A wxStaticText can't do this — it paints its whole rectangle with a flat
// background colour, covering any rule the parent tries to draw behind it.
// It also owns a right-edge resize grip: hovering the last few px shows a ↔ cursor,
// and dragging there resizes this column (reported via onResize(col, newWidth)).
class HeaderCell : public wxWindow {
public:
    static constexpr int kGrip = 5;   // right-edge drag zone width (px)

    HeaderCell(wxWindow* parent, const wxString& label, bool centred, int col, int width,
               std::function<void(int, int)> onResize)
        : wxWindow(parent, wxID_ANY), label_(label), centred_(centred), col_(col),
          onResize_(std::move(onResize))
    {
        SetBackgroundColour(theme::kWhite);
        SetMinSize(wxSize(width, kHeaderH));
        Bind(wxEVT_PAINT, &HeaderCell::OnPaint, this);
        Bind(wxEVT_MOTION, &HeaderCell::OnMotion, this);
        Bind(wxEVT_LEFT_DOWN, &HeaderCell::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &HeaderCell::OnLeftUp, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { dragging_ = false; });
    }

private:
    bool InGrip(int x) const { return x >= GetClientSize().x - kGrip; }

    void OnMotion(wxMouseEvent& e)
    {
        if (dragging_) {
            const int dx = e.GetPosition().x - dragStartX_;
            if (onResize_) onResize_(col_, dragStartW_ + dx);   // clamped view-side
        } else {
            SetCursor(InGrip(e.GetPosition().x) ? wxCursor(wxCURSOR_SIZEWE) : wxNullCursor);
        }
        e.Skip();
    }
    void OnLeftDown(wxMouseEvent& e)
    {
        if (InGrip(e.GetPosition().x)) {
            dragging_   = true;
            dragStartX_ = e.GetPosition().x;
            dragStartW_ = GetClientSize().x;
            if (!HasCapture()) CaptureMouse();
        }
        e.Skip();
    }
    void OnLeftUp(wxMouseEvent& e)
    {
        if (dragging_) { dragging_ = false; if (HasCapture()) ReleaseMouse(); }
        e.Skip();
    }

    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(theme::kWhite));
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        dc.SetFont(Ui(8.5, true));
        dc.SetTextForeground(theme::kTextMuted);
        wxCoord tw = 0, th = 0;
        dc.GetTextExtent(label_, &tw, &th);
        const int x = centred_ ? (sz.x - tw) / 2 : 6;   // 6px ≈ the cells' left inset
        dc.DrawText(label_, x, (sz.y - kHeaderRuleH - th) / 2);
        dc.SetBrush(wxBrush(kHeaderRule));
        dc.DrawRectangle(0, sz.y - kHeaderRuleH, sz.x, kHeaderRuleH);
    }

    wxString label_;
    bool     centred_;
    int      col_;
    std::function<void(int, int)> onResize_;
    bool     dragging_ = false;
    int      dragStartX_ = 0, dragStartW_ = 0;
};

// 键单元格：自绘、不可获得焦点 → 不出现文本光标；用户只点击即切换主键（PK）。
// 用 wxTextCtrl(哪怕只读) 会显示插入符/可 Tab 聚焦；裸 wxWindow owner-draw 既无光标又
// 能画居中的 PKn / FK / UQ 徽章。文字水平+垂直居中，与其余单元格观感一致。
class BadgeCell : public wxWindow {
public:
    explicit BadgeCell(wxWindow* parent) : wxWindow(parent, wxID_ANY)
    {
        SetBackgroundColour(theme::kWhite);
        SetMinSize(wxSize(46, kRowH));        // == kColMin[F_Key]; overridden by caller
        SetCursor(wxCursor(wxCURSOR_HAND));   // affordance: click to (un)set PK
        Bind(wxEVT_PAINT, &BadgeCell::OnPaint, this);
    }

    void SetBadge(const wxString& text, const wxColour& colour)
    {
        if (text == text_ && colour == colour_) return;
        text_ = text; colour_ = colour; Refresh();
    }
    void SetCellBg(const wxColour& c) { if (c != bg_) { bg_ = c; Refresh(); } }

    // Never a tab-stop, never focusable → the cell shows no caret at all.
    bool AcceptsFocus() const override { return false; }
    bool AcceptsFocusFromKeyboard() const override { return false; }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(bg_));
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        if (text_.empty()) return;
        dc.SetFont(Ui(8.5, true));
        dc.SetTextForeground(colour_);
        wxCoord tw = 0, th = 0;
        dc.GetTextExtent(text_, &tw, &th);
        dc.DrawText(text_, (sz.x - tw) / 2, (sz.y - th) / 2);
    }

    wxString text_;
    wxColour colour_ = theme::kTextMuted;
    wxColour bg_ = theme::kWhite;
};

// 序号单元格：自绘、不可获得焦点 → 无文本光标。左键点击选行（Ctrl/Shift 交给回调判定多选），
// 右键弹出字段右键菜单。当前行 = 加粗蓝色行号；被选中 = 选中底色铺满整格。
class NumCell : public wxWindow {
public:
    NumCell(wxWindow* parent, std::function<void(wxMouseEvent&)> onLeft,
            std::function<void(wxMouseEvent&)> onRight)
        : wxWindow(parent, wxID_ANY), onLeft_(std::move(onLeft)), onRight_(std::move(onRight))
    {
        SetBackgroundColour(theme::kWhite);
        SetMinSize(wxSize(42, kRowH));        // == kColMin[F_Num]; overridden by caller
        Bind(wxEVT_PAINT, &NumCell::OnPaint, this);
        // SetFocus on click so focus lands on the (caret-less) gutter, not a text cell —
        // this is what lets Ctrl+C/V act on ROWS here yet stay text ops inside a cell.
        Bind(wxEVT_LEFT_DOWN,  [this](wxMouseEvent& e) { SetFocus(); if (onLeft_)  onLeft_(e); });
        Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& e) { SetFocus(); if (onRight_) onRight_(e); });
    }

    void SetNumber(int n) { const wxString s = wxString::Format(L"%d", n);
                            if (s != text_) { text_ = s; Refresh(); } }
    void SetState(bool current, bool selected)
    {
        if (current == current_ && selected == selected_) return;
        current_ = current; selected_ = selected; Refresh();
    }
    // Focusable by mouse only (so a click can park focus here) but never a Tab stop;
    // it's a plain wxWindow, so focus shows NO text caret.
    bool AcceptsFocus() const override { return true; }
    bool AcceptsFocusFromKeyboard() const override { return false; }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        const wxSize sz = GetClientSize();
        const wxColour bg = selected_ ? theme::kRowHeaderActiveBg
                          : current_  ? Lighten(theme::kPrimary, 0.86)
                                      : theme::kWhite;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(bg));
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        dc.SetFont(Ui(9, current_));
        dc.SetTextForeground(current_ ? theme::kPrimary : theme::kTextMuted);
        wxCoord tw = 0, th = 0;
        dc.GetTextExtent(text_, &tw, &th);
        dc.DrawText(text_, (sz.x - tw) / 2, (sz.y - th) / 2);
    }

    std::function<void(wxMouseEvent&)> onLeft_, onRight_;
    wxString text_;
    bool current_ = false, selected_ = false;
};

// 类型单元格：无边框文本框（与其余扁平白格一致，无原生下拉边框/箭头）＋ 自绘过滤下拉浮层。
// 关键设计：焦点永远留在文本框，下拉列表（wxPopupWindow + wxListBox）只做展示，全程用键盘
// (↓/↑/Enter/Esc) 驱动选择——彻底避开原生 wxComboBox 在 EVT_TEXT 中重入 Popup() 触发的
// 崩溃（Windows 派发组合框通知期间重入 SendMessage 是访问违例，且常绕过崩溃处理器不弹窗）。
class TypeCell : public wxTextCtrl {
public:
    TypeCell(wxWindow* parent, std::function<void()> onChanged)
        : wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                     wxBORDER_NONE | wxTE_PROCESS_ENTER | wxTE_CENTRE),
          onChanged_(std::move(onChanged))
    {
        popup_ = new wxPopupWindow(this, wxBORDER_SIMPLE);
        list_  = new wxListBox(popup_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               0, nullptr, wxLB_SINGLE | wxLB_NEEDED_SB);
        list_->SetFont(Mono(9.5));
        auto* s = new wxBoxSizer(wxVERTICAL);
        s->Add(list_, 1, wxEXPAND);
        popup_->SetSizer(s);

        // Visible ▼ affordance at the right edge: click toggles the full type list.
        tri_ = new wxWindow(this, wxID_ANY);
        tri_->SetBackgroundColour(theme::kWhite);
        tri_->SetCursor(wxCursor(wxCURSOR_HAND));
        tri_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(tri_);
            const wxSize c = tri_->GetClientSize();
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(theme::kWhite));
            dc.DrawRectangle(0, 0, c.x, c.y);
            const int w = 8, h = 5, x = (c.x - w) / 2, y = (c.y - h) / 2;
            const wxPoint tri[3] = { wxPoint(x, y), wxPoint(x + w, y), wxPoint(x + w / 2, y + h) };
            dc.SetBrush(wxBrush(theme::kTextMuted));
            dc.DrawPolygon(3, tri);
        });
        tri_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (popup_->IsShown()) Dismiss();
            else { SetFocus(); ShowList(choices_); }   // ▼ → full list
        });

        Bind(wxEVT_TEXT,       &TypeCell::OnText, this);
        Bind(wxEVT_CHAR_HOOK,  &TypeCell::OnCharHook, this);
        Bind(wxEVT_KILL_FOCUS, &TypeCell::OnKillFocus, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
            const wxSize c = GetClientSize();
            const int tw = 16;
            tri_->SetSize(c.x - tw, 0, tw, c.y);       // pin ▼ to the right edge
            e.Skip();
        });
        list_->Bind(wxEVT_LEFT_DOWN, &TypeCell::OnListClick, this);
    }

    void SetChoices(const wxArrayString& c) { choices_ = c; }

private:
    void OnText(wxCommandEvent& e)     // fires only on USER typing (ChangeValue is silent)
    {
        if (onChanged_) onChanged_();  // length/scale editability + attribute panel
        UpdatePopup();
        e.Skip();
    }

    void OnKillFocus(wxFocusEvent& e)
    {
        // A click on a list item shifts focus first; defer the dismiss so that click's
        // Commit still runs. Keep the popup open while focus stays within the cell or
        // the dropdown itself (guards against a popup that briefly grabs focus on show).
        CallAfter([this] {
            wxWindow* f = wxWindow::FindFocus();
            if (f != this && f != list_ && f != popup_ && f != tri_) Dismiss();
        });
        e.Skip();
    }

    void OnListClick(wxMouseEvent& e)
    {
        const int idx = list_->HitTest(e.GetPosition());
        if (idx == wxNOT_FOUND) { e.Skip(); return; }   // scrollbar/empty → let the list handle
        Commit(list_->GetString(static_cast<unsigned>(idx)));
        SetFocus();                    // keep typing in the cell after the pick
    }

    void OnCharHook(wxKeyEvent& e)
    {
        const int k = e.GetKeyCode();
        if (popup_->IsShown() && (k == WXK_DOWN || k == WXK_UP)) {
            const int n = static_cast<int>(list_->GetCount());
            if (n > 0) {
                int sel = list_->GetSelection();
                if (sel == wxNOT_FOUND) sel = (k == WXK_DOWN) ? 0 : n - 1;
                else sel = (k == WXK_DOWN) ? (sel + 1) % n : (sel + n - 1) % n;
                list_->SetSelection(sel);   // no event; just moves the highlight
            }
            return;                          // consume → don't move the text caret
        }
        if ((k == WXK_RETURN || k == WXK_NUMPAD_ENTER) && popup_->IsShown()
            && list_->GetSelection() != wxNOT_FOUND) {
            Commit(list_->GetString(static_cast<unsigned>(list_->GetSelection())));
            return;                          // consume → don't submit the form
        }
        if (k == WXK_ESCAPE && popup_->IsShown()) { Dismiss(); return; }
        e.Skip();
    }

    // Narrow the list to type names CONTAINING the typed text (substring, case-insensitive),
    // excluding an exact hit (nothing left to suggest once fully typed). Empty/exact/no-match
    // → hide.
    void UpdatePopup()
    {
        const wxString needle = GetValue().Lower();
        if (needle.IsEmpty()) { Dismiss(); return; }
        wxArrayString m;
        for (const wxString& t : choices_) {
            const wxString lt = t.Lower();
            if (lt.Contains(needle) && lt != needle) m.Add(t);
        }
        ShowList(m);
    }

    // Show `items` in a popup anchored EXACTLY under the cell (same width). Uses
    // GetScreenPosition (scroll-safe, unambiguous) + SetPosition (no auto-flip that
    // was displacing the list far from the cell). Empty → hide.
    void ShowList(const wxArrayString& items)
    {
        if (items.IsEmpty()) { Dismiss(); return; }
        list_->Set(items);
        list_->SetSelection(0);
        const int rows = std::min<int>(static_cast<int>(items.size()), 10);
        const int ih   = list_->GetCharHeight() + 4;
        const wxSize sz(GetSize().x, rows * ih + 4);
        popup_->SetSize(sz);
        list_->SetSize(sz);
        popup_->SetPosition(GetScreenPosition() + wxPoint(0, GetSize().y));
        if (!popup_->IsShown()) popup_->Show();
    }

    void Commit(const wxString& v)
    {
        ChangeValue(v);                 // event-free set → no reentrant OnText
        SetInsertionPointEnd();
        Dismiss();
        if (onChanged_) onChanged_();   // apply length/scale editability + panel for the picked type
    }

    void Dismiss() { if (popup_ && popup_->IsShown()) popup_->Hide(); }

    std::function<void()> onChanged_;
    wxArrayString  choices_;
    wxPopupWindow* popup_ = nullptr;
    wxListBox*     list_  = nullptr;
    wxWindow*      tri_   = nullptr;   // the ▼ dropdown affordance
};

// 单选下拉单元格（封闭枚举，如索引"类型"/"方式"、FK OnDelete/OnUpdate）：自绘只读格
// （白底 + 居中显示值 + 右侧 ▼）+ wxPopupWindow/wxListBox 浮层。与 TypeCell 同款"普通
// wxPopupWindow + OnKillFocus 手动消隐"，但无输入无过滤。存储值(value_)可与显示值不同：
// 显示串来自 disp_，回填/存储值来自平行的 vals_（""=引擎默认，显示为"普通/默认"占位）。
class PopupListCell : public wxWindow {
public:
    PopupListCell(wxWindow* parent, std::function<void()> onChanged)
        : wxWindow(parent, wxID_ANY), onChanged_(std::move(onChanged))
    {
        SetBackgroundColour(theme::kWhite);
        SetMinSize(wxSize(60, kRowH));
        // wxPopupTransientWindow (not plain wxPopupWindow): it captures the mouse so
        // clicks inside the list register and any click OUTSIDE auto-dismisses it —
        // exactly the dropdown behaviour a plain popup lacked on MSW.
        popup_ = new wxPopupTransientWindow(this, wxBORDER_SIMPLE);
        list_  = new wxListBox(popup_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               0, nullptr, wxLB_SINGLE | wxLB_NEEDED_SB);
        list_->SetFont(Mono(9.5));
        auto* s = new wxBoxSizer(wxVERTICAL); s->Add(list_, 1, wxEXPAND); popup_->SetSizer(s);
        Bind(wxEVT_PAINT, &PopupListCell::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (popup_->IsShown()) Dismiss(); else ShowList(); });
        list_->Bind(wxEVT_LEFT_DOWN, &PopupListCell::OnListClick, this);
    }
    bool AcceptsFocus() const override { return true; }
    bool AcceptsFocusFromKeyboard() const override { return false; }
    void SetChoices(const wxArrayString& disp, const wxArrayString& vals)
    { disp_ = disp; vals_ = vals; }
    void SetStored(const wxString& v) { value_ = v; Refresh(); }
    wxString GetStored() const { return value_; }

private:
    wxString DisplayText() const {
        for (size_t i = 0; i < vals_.size(); ++i) if (vals_[i] == value_) return disp_[i];
        return value_;
    }
    void OnPaint(wxPaintEvent&) {
        wxPaintDC dc(this); const wxSize sz = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(theme::kWhite));
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        dc.SetFont(Mono(9.5)); dc.SetTextForeground(theme::kTextBody);
        const wxString t = DisplayText();
        wxCoord tw = 0, th = 0; dc.GetTextExtent(t, &tw, &th);
        dc.DrawText(t, 6, (sz.y - th) / 2);
        const int w = 8, h = 5, x = sz.x - 14, y = (sz.y - h) / 2;
        const wxPoint tri[3] = { wxPoint(x, y), wxPoint(x + w, y), wxPoint(x + w / 2, y + h) };
        dc.SetBrush(wxBrush(theme::kTextMuted)); dc.DrawPolygon(3, tri);
    }
    void ShowList() {
        if (disp_.IsEmpty()) return;
        list_->Set(disp_);
        int sel = 0;
        for (size_t i = 0; i < vals_.size(); ++i) if (vals_[i] == value_) { sel = (int)i; break; }
        list_->SetSelection(sel);
        const int rows = std::min<int>((int)disp_.size(), 10);
        const int ih = list_->GetCharHeight() + 4;
        const wxSize sz(std::max(GetSize().x, 90), rows * ih + 4);
        popup_->SetSize(sz); list_->SetSize(sz);
        popup_->SetPosition(GetScreenPosition() + wxPoint(0, GetSize().y));
        if (!popup_->IsShown()) popup_->Popup();
    }
    void OnListClick(wxMouseEvent& e) {
        const int idx = list_->HitTest(e.GetPosition());
        if (idx == wxNOT_FOUND) { e.Skip(); return; }
        if ((size_t)idx < vals_.size()) value_ = vals_[idx];
        Refresh(); Dismiss();
        if (onChanged_) onChanged_();
    }
    void Dismiss() { if (popup_ && popup_->IsShown()) popup_->Dismiss(); }
    std::function<void()> onChanged_;
    wxArrayString  disp_, vals_;
    wxString       value_;
    wxPopupTransientWindow* popup_ = nullptr;
    wxListBox*     list_  = nullptr;
};

// 多选下拉单元格（如索引"字段"、FK 列）：自绘只读格（白底 + 逗号拼接已选 + 右侧 ▼）+
// wxPopupWindow/wxCheckListBox。wxCheckListBox 派生自 wxListBox（非 combobox，不触发
// 崩溃）。勾选顺序即列顺序（复合索引 (a,b)≠(b,a)），故 selected_ 按勾选先后维护。候选可
// 由 provider_ 在每次打开时实时提供（如取当前字段名）。
class MultiPickCell : public wxWindow {
public:
    MultiPickCell(wxWindow* parent, std::function<void()> onChanged)
        : wxWindow(parent, wxID_ANY), onChanged_(std::move(onChanged))
    {
        SetBackgroundColour(theme::kWhite);
        SetMinSize(wxSize(80, kRowH));
        // wxPopupTransientWindow: captures the mouse so the wxCheckListBox checkboxes
        // actually toggle on click, and any click outside auto-dismisses the dropdown
        // (a plain wxPopupWindow did neither on MSW — the reported "can't tick / won't
        // close" bug). Ticking items inside keeps it open (only outside-click closes).
        popup_ = new wxPopupTransientWindow(this, wxBORDER_SIMPLE);
        list_  = new wxCheckListBox(popup_, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr);
        list_->SetFont(Mono(9.5));
        auto* s = new wxBoxSizer(wxVERTICAL); s->Add(list_, 1, wxEXPAND); popup_->SetSizer(s);
        Bind(wxEVT_PAINT, &MultiPickCell::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (popup_->IsShown()) Dismiss(); else ShowList(); });
        // Whole-row toggle: wxCheckListBox only ticks when you hit the tiny checkbox
        // GLYPH — a click on the label just selects, so users "can't tick" (reported
        // bug). Intercept LEFT_DOWN, hit-test the row and toggle its check ourselves;
        // the popup stays open for multi-pick. Check() is programmatic (no
        // EVT_CHECKLISTBOX recursion); consuming the event (no Skip) stops the native
        // selection/checkbox handling from also firing. Check ORDER = column order.
        list_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& ev) {
            const int i = list_->HitTest(ev.GetPosition());
            if (i == wxNOT_FOUND || (unsigned)i >= cand_.size()) { ev.Skip(); return; }
            const bool nowChecked = !list_->IsChecked(i);
            list_->Check(i, nowChecked);
            const wxString col = cand_[i];
            auto it = std::find(selected_.begin(), selected_.end(), col);
            if (nowChecked) { if (it == selected_.end()) selected_.push_back(col); }
            else if (it != selected_.end())              selected_.erase(it);
            Refresh();
            if (onChanged_) onChanged_();
        });
    }
    bool AcceptsFocus() const override { return true; }
    bool AcceptsFocusFromKeyboard() const override { return false; }
    void SetCandidates(const wxArrayString& c) { cand_ = c; }
    void SetCandidateProvider(std::function<wxArrayString()> p) { provider_ = std::move(p); }
    void SetSelected(const std::vector<wxString>& sel) { selected_ = sel; Refresh(); }
    const std::vector<wxString>& GetSelected() const { return selected_; }

private:
    void OnPaint(wxPaintEvent&) {
        wxPaintDC dc(this); const wxSize sz = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(theme::kWhite));
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        wxString t; for (size_t i = 0; i < selected_.size(); ++i) { if (i) t += L", "; t += selected_[i]; }
        const bool empty = t.IsEmpty();
        if (empty) t = tr(L"选择字段");
        dc.SetFont(Mono(9.5));
        dc.SetTextForeground(empty ? theme::kTextFaint : theme::kTextBody);
        wxCoord tw = 0, th = 0; dc.GetTextExtent(t, &tw, &th);
        { wxDCClipper clip(dc, 6, 0, std::max(0, sz.x - 6 - 16), sz.y);
          dc.DrawText(t, 6, (sz.y - th) / 2); }
        const int w = 8, h = 5, x = sz.x - 14, y = (sz.y - h) / 2;
        const wxPoint tri[3] = { wxPoint(x, y), wxPoint(x + w, y), wxPoint(x + w / 2, y + h) };
        dc.SetBrush(wxBrush(theme::kTextMuted)); dc.DrawPolygon(3, tri);
    }
    void ShowList() {
        if (provider_) cand_ = provider_();
        if (cand_.IsEmpty()) return;
        list_->Set(cand_);
        for (unsigned i = 0; i < cand_.size(); ++i)
            list_->Check(i, std::find(selected_.begin(), selected_.end(), cand_[i]) != selected_.end());
        const int rows = std::min<int>((int)cand_.size(), 10);
        const int ih = list_->GetCharHeight() + 4;
        const wxSize sz(std::max(GetSize().x, 140), rows * ih + 6);
        popup_->SetSize(sz); list_->SetSize(sz);
        popup_->SetPosition(GetScreenPosition() + wxPoint(0, GetSize().y));
        if (!popup_->IsShown()) popup_->Popup();
    }
    void Dismiss() { if (popup_ && popup_->IsShown()) popup_->Dismiss(); }
    std::function<void()> onChanged_;
    std::function<wxArrayString()> provider_;
    wxArrayString  cand_;
    std::vector<wxString> selected_;
    wxPopupTransientWindow* popup_ = nullptr;
    wxCheckListBox* list_  = nullptr;
};

// A painted tab in the design-view tab strip: compact, a clear divider between tabs,
// and a distinct selected look — white ("connected to the content") background, an
// accent underline and bold accent label; a subtle tint on hover.
class TabCell : public wxWindow {
public:
    TabCell(wxWindow* parent, const wxString& label, std::function<void()> onClick)
        : wxWindow(parent, wxID_ANY), label_(label), onClick_(std::move(onClick))
    {
        Bind(wxEVT_PAINT, &TabCell::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { if (onClick_) onClick_(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
        SetCursor(wxCursor(wxCURSOR_HAND));
        wxClientDC dc(this); dc.SetFont(Ui(9.5, true));
        wxCoord w = 0, h = 0; dc.GetTextExtent(label_, &w, &h);
        SetMinSize(wxSize(w + 22, -1));   // label + horizontal padding
    }
    void SetSelected(bool s) { if (s != sel_) { sel_ = s; Refresh(); } }

private:
    void OnPaint(wxPaintEvent&) {
        wxPaintDC dc(this);
        const wxSize sz = GetClientSize();
        const wxColour bg = sel_ ? theme::kWhite
                          : hover_ ? Lighten(theme::kTabStripBg, 0.4)
                                   : theme::kTabStripBg;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(bg));
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        dc.SetPen(wxPen(theme::kBorder));                    // divider on the right edge
        dc.DrawLine(sz.x - 1, 5, sz.x - 1, sz.y - 5);
        dc.SetFont(Ui(9.5, sel_));
        dc.SetTextForeground(sel_ ? theme::kPrimary : theme::kTextSecondary);
        wxCoord tw = 0, th = 0; dc.GetTextExtent(label_, &tw, &th);
        dc.DrawText(label_, (sz.x - tw) / 2, (sz.y - th) / 2);
        if (sel_) {                                          // accent underline
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(theme::kPrimary));
            dc.DrawRectangle(0, sz.y - 2, sz.x, 2);
        }
    }
    wxString label_;
    std::function<void()> onClick_;
    bool sel_ = false, hover_ = false;
};

} // namespace ui
