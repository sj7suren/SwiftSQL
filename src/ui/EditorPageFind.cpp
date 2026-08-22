// EditorPageFind.cpp — third translation unit for ui::EditorPage: the Ctrl+F
// find/replace bar, whole and entire.
//
// Build (lazily, on first Ctrl+F), show/hide, step through matches with wrap,
// replace one, replace all as a single undo step, and highlight every match with
// a Scintilla indicator. It is one subsystem: every function here reads the same
// two text controls and one checkbox, and all of them drive the SAME
// wxStyledTextCtrl search API.
//
// This is a WIDGET-LEVEL seam (the ResultGridPanelChrome.cpp precedent), not a
// logic seam, and the distinction is deliberate rather than lazy: the searching
// is not ours to extract. DoFind delegates to Scintilla's SearchNext/SearchPrev
// and DoReplaceAll to its target/ReplaceTarget protocol, so there is no
// dialect-free algorithm hiding in here that a headless test could pin — pulling
// it out would produce a "pure" module that was really just a wxSTC puppet. The
// genuinely pure logic carved out of EditorPage lives in EditorSqlFormat.{h,cpp}
// and EditorCompletion.{h,cpp}, and those ARE unit-tested.
#include "ui/EditorPage.h"

#include <wx/wx.h>
#include <wx/checkbox.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <algorithm>

#include "ui/I18n.h"           // tr()
#include "ui/Theme.h"

namespace ui {

int EditorPage::SearchFlags() const
{
    return (matchCase_ && matchCase_->IsChecked()) ? wxSTC_FIND_MATCHCASE : 0;
}

void EditorPage::BuildFindBar()
{
    if (findBar_) return;
    auto* bar = new wxPanel(this);
    bar->SetBackgroundColour(theme::kChromeBg);
    auto* h = new wxBoxSizer(wxHORIZONTAL);

    auto* lbl = new wxStaticText(bar, wxID_ANY, tr(L"查找"));
    lbl->SetForegroundColour(theme::kTextSecondary);
    h->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

    findCtrl_ = new wxTextCtrl(bar, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize(180, -1), wxTE_PROCESS_ENTER);
    h->Add(findCtrl_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

    auto* prev = new wxButton(bar, wxID_ANY, tr(L"上一个"), wxDefaultPosition,
                              wxDefaultSize, wxBU_EXACTFIT);
    auto* next = new wxButton(bar, wxID_ANY, tr(L"下一个"), wxDefaultPosition,
                              wxDefaultSize, wxBU_EXACTFIT);
    h->Add(prev, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
    h->Add(next, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

    auto* rlbl = new wxStaticText(bar, wxID_ANY, tr(L"替换"));
    rlbl->SetForegroundColour(theme::kTextSecondary);
    h->Add(rlbl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

    replaceCtrl_ = new wxTextCtrl(bar, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxSize(180, -1), wxTE_PROCESS_ENTER);
    h->Add(replaceCtrl_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

    auto* rep    = new wxButton(bar, wxID_ANY, tr(L"替换"), wxDefaultPosition,
                                wxDefaultSize, wxBU_EXACTFIT);
    auto* repAll = new wxButton(bar, wxID_ANY, tr(L"全部替换"), wxDefaultPosition,
                                wxDefaultSize, wxBU_EXACTFIT);
    h->Add(rep,    0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
    h->Add(repAll, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

    matchCase_ = new wxCheckBox(bar, wxID_ANY, tr(L"区分大小写"));
    matchCase_->SetForegroundColour(theme::kTextSecondary);
    h->Add(matchCase_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

    matchLabel_ = new wxStaticText(bar, wxID_ANY, wxEmptyString);
    matchLabel_->SetForegroundColour(theme::kTextMuted);
    h->Add(matchLabel_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

    h->AddStretchSpacer(1);
    auto* close = new wxButton(bar, wxID_ANY, L"×", wxDefaultPosition,
                               wxSize(26, -1), wxBU_EXACTFIT);
    h->Add(close, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    bar->SetSizer(h);
    findBar_ = bar;

    // wire events
    findCtrl_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { MarkAllMatches(); });
    findCtrl_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { DoFind(true); });
    findCtrl_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& ev) {
        if (ev.GetKeyCode() == WXK_ESCAPE) { HideFindBar(); return; }
        ev.Skip();
    });
    replaceCtrl_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { DoReplace(); });
    replaceCtrl_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& ev) {
        if (ev.GetKeyCode() == WXK_ESCAPE) { HideFindBar(); return; }
        ev.Skip();
    });
    prev->Bind(wxEVT_BUTTON,   [this](wxCommandEvent&) { DoFind(false); });
    next->Bind(wxEVT_BUTTON,   [this](wxCommandEvent&) { DoFind(true); });
    rep->Bind(wxEVT_BUTTON,    [this](wxCommandEvent&) { DoReplace(); });
    repAll->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DoReplaceAll(); });
    close->Bind(wxEVT_BUTTON,  [this](wxCommandEvent&) { HideFindBar(); });
    matchCase_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { MarkAllMatches(); });

    // Insert above the editor/results split, but below the object-editor selector
    // bar (index 0) when that exists so the selector always stays on top.
    if (auto* s = GetSizer()) {
        s->Insert(objToolbar_ ? 1 : 0, findBar_, 0, wxEXPAND);
        findBar_->Hide();
        s->Layout();
    }
}

void EditorPage::ShowFindBar()
{
    BuildFindBar();
    // Seed the query from the current selection (single-line only).
    const wxString sel = stc_->GetSelectedText();
    if (!sel.IsEmpty() && sel.Find('\n') == wxNOT_FOUND)
        findCtrl_->ChangeValue(sel);
    findBar_->Show();
    if (auto* s = GetSizer()) s->Layout();
    MarkAllMatches();
    findCtrl_->SetFocus();
    findCtrl_->SelectAll();
}

void EditorPage::HideFindBar()
{
    if (!findBar_) return;
    findBar_->Hide();
    if (auto* s = GetSizer()) s->Layout();
    stc_->SetIndicatorCurrent(kFindIndicator);
    stc_->IndicatorClearRange(0, stc_->GetLength());
    stc_->SetFocus();
}

void EditorPage::DoFind(bool forward)
{
    if (!findCtrl_) return;
    const wxString needle = findCtrl_->GetValue();
    if (needle.IsEmpty()) return;
    // Advance past the current selection so repeated presses step through matches.
    const int caret = forward ? std::max(stc_->GetSelectionStart(), stc_->GetSelectionEnd())
                              : std::min(stc_->GetSelectionStart(), stc_->GetSelectionEnd());
    stc_->SetCurrentPos(caret);
    stc_->SetAnchor(caret);
    stc_->SearchAnchor();
    int pos = forward ? stc_->SearchNext(SearchFlags(), needle)
                      : stc_->SearchPrev(SearchFlags(), needle);
    if (pos < 0) {   // wrap around
        const int wrap = forward ? 0 : stc_->GetLength();
        stc_->SetCurrentPos(wrap);
        stc_->SetAnchor(wrap);
        stc_->SearchAnchor();
        pos = forward ? stc_->SearchNext(SearchFlags(), needle)
                      : stc_->SearchPrev(SearchFlags(), needle);
    }
    if (pos >= 0) stc_->EnsureCaretVisible();   // SearchNext/Prev select the match
}

void EditorPage::DoReplace()
{
    if (!findCtrl_) return;
    const wxString needle = findCtrl_->GetValue();
    if (needle.IsEmpty()) return;
    const wxString sel = stc_->GetSelectedText();
    const bool matches = (SearchFlags() & wxSTC_FIND_MATCHCASE)
                             ? (sel == needle) : sel.IsSameAs(needle, false);
    if (matches) stc_->ReplaceSelection(replaceCtrl_->GetValue());
    DoFind(true);
    MarkAllMatches();
}

void EditorPage::DoReplaceAll()
{
    if (!findCtrl_) return;
    const wxString needle = findCtrl_->GetValue();
    if (needle.IsEmpty()) return;
    const wxString repl = replaceCtrl_->GetValue();
    const int flags = SearchFlags();
    stc_->SetSearchFlags(flags);
    stc_->BeginUndoAction();
    int pos = 0, count = 0;
    int docLen = stc_->GetLength();
    while (pos <= docLen) {
        stc_->SetTargetStart(pos);
        stc_->SetTargetEnd(docLen);
        if (stc_->SearchInTarget(needle) < 0) break;
        const int mstart = stc_->GetTargetStart();
        const int mend   = stc_->GetTargetEnd();
        stc_->SetTargetStart(mstart);
        stc_->SetTargetEnd(mend);
        const int newLen = stc_->ReplaceTarget(repl);
        ++count;
        pos = mstart + (newLen > 0 ? newLen : 1);   // avoid a zero-width loop
        docLen = stc_->GetLength();                  // length shifts as we replace
    }
    stc_->EndUndoAction();
    MarkAllMatches();
    if (matchLabel_)
        matchLabel_->SetLabel(wxString::Format(tr(L"已替换 %d 处"), count));
    if (auto* s = findBar_ ? findBar_->GetSizer() : nullptr) s->Layout();
}

void EditorPage::MarkAllMatches()
{
    stc_->SetIndicatorCurrent(kFindIndicator);
    stc_->IndicatorClearRange(0, stc_->GetLength());
    const wxString needle = findCtrl_ ? findCtrl_->GetValue() : wxString();
    if (needle.IsEmpty()) { if (matchLabel_) matchLabel_->SetLabel(wxEmptyString); return; }
    stc_->SetSearchFlags(SearchFlags());
    int pos = 0, count = 0;
    const int docLen = stc_->GetLength();
    while (pos < docLen) {
        stc_->SetTargetStart(pos);
        stc_->SetTargetEnd(docLen);
        if (stc_->SearchInTarget(needle) < 0) break;
        const int mstart = stc_->GetTargetStart();
        const int mend   = stc_->GetTargetEnd();
        if (mend <= mstart) { pos = mstart + 1; continue; }
        stc_->IndicatorFillRange(mstart, mend - mstart);
        ++count;
        pos = mend;
    }
    if (matchLabel_) {
        matchLabel_->SetLabel(count ? wxString::Format(tr(L"%d 处匹配"), count)
                                    : tr(L"无匹配"));
        if (auto* s = findBar_ ? findBar_->GetSizer() : nullptr) s->Layout();
    }
}

} // namespace ui
