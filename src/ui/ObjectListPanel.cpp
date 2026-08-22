// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "ui/ObjectListPanel.h"

#include <wx/listctrl.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>

#include "ui/FlatControls.h"   // ui::FlatButton
#include "ui/Theme.h"
#include "ui/UiFonts.h"
#include "ui/I18n.h"

namespace ui {

ObjectListPanel::ObjectListPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(theme::kWhite);
    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- top toolbar: type-specific action buttons (left) + a filter (right) ----
    auto* barPanel = new wxPanel(this, wxID_ANY);
    barPanel->SetBackgroundColour(theme::kWhite);
    toolbar_ = new wxBoxSizer(wxHORIZONTAL);
    toolbar_->AddSpacer(8);

    search_ = new wxSearchCtrl(barPanel, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize(200, 28));
    search_->SetDescriptiveText(tr(L"筛选…"));
    search_->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) { RenderRows(); e.Skip(); });

    // toolbar_ holds the buttons; a stretch spacer pushes the filter to the right.
    // The buttons themselves are appended in Setup().
    auto* barRow = new wxBoxSizer(wxHORIZONTAL);
    barRow->Add(toolbar_, 0, wxALIGN_CENTER_VERTICAL);
    barRow->AddStretchSpacer(1);
    barRow->Add(search_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    barPanel->SetSizer(barRow);
    root->Add(barPanel, 0, wxEXPAND | wxTOP | wxBOTTOM, 6);

    // ---- object list (report; multi-select on — no wxLC_SINGLE_SEL) ----
    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT | wxBORDER_NONE);
    list_->SetBackgroundColour(theme::kWhite);
    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &ObjectListPanel::OnItemActivated, this);
    list_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &ObjectListPanel::OnContextMenu, this);
    list_->Bind(wxEVT_LIST_ITEM_SELECTED,   [this](wxListEvent& e){ UpdateActionEnabled(); e.Skip(); });
    list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent& e){ UpdateActionEnabled(); e.Skip(); });
    root->Add(list_, 1, wxEXPAND);

    SetSizer(root);
}

void ObjectListPanel::Setup(std::vector<Column> columns, std::vector<Action> actions,
                            int doubleClickAction)
{
    columns_   = std::move(columns);
    actions_   = std::move(actions);
    dblAction_ = doubleClickAction;

    // Build the toolbar buttons from the action list (flat, semantic-coloured).
    wxWindow* barPanel = search_->GetParent();
    for (const Action& a : actions_) {
        auto* b = new FlatButton(barPanel, a.glyph, a.label, a.color,
                                 [this, a] { DispatchAction(a); });
        b->SetToolTipText(a.label);
        toolbar_->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
        buttons_.push_back(b);
    }
    barPanel->Layout();

    // Build the report columns.
    list_->ClearAll();
    for (size_t i = 0; i < columns_.size(); ++i) {
        const Column& c = columns_[i];
        list_->InsertColumn(static_cast<long>(i), c.title,
                            c.rightAlign ? wxLIST_FORMAT_RIGHT : wxLIST_FORMAT_LEFT,
                            c.width);
    }
    UpdateActionEnabled();
}

void ObjectListPanel::SetLoader(std::function<bool(std::vector<Row>&, wxString&)> loader)
{
    loader_ = std::move(loader);
}

void ObjectListPanel::Reload()
{
    if (!loader_) return;
    std::vector<Row> out;
    wxString err;
    if (!loader_(out, err)) {
        wxMessageBox(tr(L"读取对象列表失败:") + L"\n\n" + err, tr(L"对象列表"),
                     wxOK | wxICON_ERROR, this);
        return;
    }
    rows_ = std::move(out);
    RenderRows();
}

void ObjectListPanel::RenderRows()
{
    if (!list_) return;
    const wxString filter = search_ ? search_->GetValue().Lower() : wxString();
    list_->DeleteAllItems();
    long idx = 0;
    for (const Row& r : rows_) {
        if (r.cells.empty()) continue;
        if (!filter.IsEmpty() && !r.cells[0].Lower().Contains(filter)) continue;
        const long row = list_->InsertItem(idx, r.cells[0]);
        for (size_t c = 1; c < r.cells.size() && c < columns_.size(); ++c)
            list_->SetItem(row, static_cast<long>(c), r.cells[c]);
        ++idx;
    }
    UpdateActionEnabled();
}

void ObjectListPanel::UpdateActionEnabled()
{
    const bool hasSel = list_ && list_->GetSelectedItemCount() > 0;
    for (size_t i = 0; i < buttons_.size() && i < actions_.size(); ++i)
        buttons_[i]->SetEnabledLook(!actions_[i].needsSelection || hasSel);
}

wxString ObjectListPanel::SelectedName() const
{
    if (!list_) return {};
    long i = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i == -1 ? wxString() : list_->GetItemText(i);
}

std::vector<wxString> ObjectListPanel::SelectedNames() const
{
    std::vector<wxString> out;
    if (!list_) return out;
    long i = -1;
    while ((i = list_->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1)
        out.push_back(list_->GetItemText(i));
    return out;
}

void ObjectListPanel::DispatchAction(const Action& a)
{
    if (!a.run) return;
    const wxString first = SelectedName();
    if (a.needsSelection && first.IsEmpty()) return;   // guard: nothing selected
    a.run(first, SelectedNames());
}

void ObjectListPanel::OnItemActivated(wxListEvent& ev)
{
    if (dblAction_ < 0 || dblAction_ >= static_cast<int>(actions_.size())) { ev.Skip(); return; }
    // Ensure the double-clicked row is the one acted upon.
    const wxString name = ev.GetText();
    const Action& a = actions_[dblAction_];
    if (a.run) a.run(name, { name });
}

void ObjectListPanel::OnContextMenu(wxListEvent& ev)
{
    ev.Skip(false);
    const wxString first = SelectedName();
    const std::vector<wxString> all = SelectedNames();
    wxMenu menu;
    for (const Action& a : actions_) {
        wxMenuItem* mi = menu.Append(wxID_ANY, a.label);
        if (a.needsSelection && first.IsEmpty()) { mi->Enable(false); continue; }
        menu.Bind(wxEVT_MENU, [a, first, all](wxCommandEvent&) { if (a.run) a.run(first, all); },
                  mi->GetId());
    }
    PopupMenu(&menu);
}

} // namespace ui
