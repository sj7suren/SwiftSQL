// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableDesignView_Options.cpp — 选项 tab of the table design view, split out per the
// ≤1000-line charter. Unlike the always-live grids, the 选项 tab is a plain whole-table
// form: one "label + control" row per profile_->TableOptionSpecs() item — a PopupListCell
// for options with a fixed candidate list (the first entry, "默认", stores ""), a plain
// wxTextCtrl otherwise. Dialects with no alterable table options (e.g. SQLite → empty
// TableOptionSpecs) show a centred hint instead. Save funnels through ExecuteEdit.
#include "ui/TableDesignView.h"
#include "ui/DesignGridCells.h"
#include "ui/I18n.h"

#include <wx/wx.h>
#include <wx/simplebook.h>
#include <functional>
#include <memory>
#include <vector>

namespace ui {

// Build (ctor) or rebuild+fill (Load) the whole-table options form. At ctor time
// profile_ is null → an empty page (the "no options" hint); Load re-invokes this once the
// dialect is resolved to lay out the real controls and seed them with current values
// (engine/charset/collation from GetTableSchema; other fields have no introspected value).
void TableDesignView::BuildOptionsForm()
{
    if (!optionsPage_) {
        optionsPage_ = new wxPanel(book_);
        optionsPage_->SetBackgroundColour(theme::kWhite);
        optionsSizer_ = new wxBoxSizer(wxVERTICAL);
        optionsPage_->SetSizer(optionsSizer_);
    }
    optionsPage_->Freeze();
    optionsSizer_->Clear(/*delete_windows=*/true);
    optionCtrls_.clear();

    const std::vector<db::DbCreateOption> specs =
        profile_ ? profile_->TableOptionSpecs() : std::vector<db::DbCreateOption>{};

    if (specs.empty()) {
        auto* hint = new wxStaticText(optionsPage_, wxID_ANY,
                                      tr(L"当前数据库无可修改的表选项"));
        hint->SetFont(Ui(10));
        hint->SetForegroundColour(theme::kTextFaint);
        optionsSizer_->AddStretchSpacer();
        optionsSizer_->Add(hint, 0, wxALIGN_CENTER);
        optionsSizer_->AddStretchSpacer();
        optionsPage_->Layout();
        optionsPage_->Thaw();
        return;
    }

    // Current values come from the live table schema (best-effort; empty when
    // unavailable). Only engine/charset/collation are introspected.
    db::TableSchema ts;
    bool haveTs = false;
    if (conn_ && !table_.IsEmpty()) { wxString e; haveTs = conn_->GetTableSchema(db_, table_, ts, e); }
    auto currentValue = [&](const wxString& id) -> wxString {
        if (!haveTs) return wxString();
        if (id == L"engine")    return ts.engine;
        if (id == L"charset")   return ts.charset;
        if (id == L"collation") return ts.collation;
        return wxString();
    };

    auto* title = new wxStaticText(optionsPage_, wxID_ANY, tr(L"表选项"));
    title->SetFont(Ui(10, true));
    title->SetForegroundColour(theme::kText);
    optionsSizer_->Add(title, 0, wxLEFT | wxTOP | wxRIGHT, 14);

    // Charset→collation linkage: the collation dropdown (a Choice with groupSource
    // set) shows only the collations belonging to the currently-selected charset.
    // Held on the heap (shared_ptr) so the charset cell's onChanged — created before
    // the collation cell exists — can reach the collation cell once both are built.
    struct CollLink {
        PopupListCell*     charsetCell = nullptr;
        PopupListCell*     collationCell = nullptr;
        db::DbCreateOption collationSpec;
    };
    auto link = std::make_shared<CollLink>();
    auto applyCollFilter = [link] {
        if (!link->charsetCell || !link->collationCell) return;
        const wxString cs = link->charsetCell->GetStored();
        const db::DbCreateOption& o = link->collationSpec;
        wxArrayString disp, vals;
        disp.Add(tr(L"默认")); vals.Add(wxString());              // stored "" = engine default
        for (size_t i = 0; i < o.choices.size(); ++i) {
            const bool match = cs.IsEmpty() || i >= o.choiceGroup.size()
                             || o.choiceGroup[i] == cs;           // no charset → show all
            if (match) { disp.Add(o.choices[i]); vals.Add(o.choices[i]); }
        }
        link->collationCell->SetChoices(disp, vals);
    };

    for (const db::DbCreateOption& o : specs) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);          // label + control on ONE line
        auto* lbl = new wxStaticText(optionsPage_, wxID_ANY, tr(o.label));
        lbl->SetFont(Ui(9));
        lbl->SetForegroundColour(theme::kTextSecondary);
        lbl->SetMinSize(wxSize(84, -1));                   // fixed label column → aligned
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

        const wxString cur = currentValue(o.id);
        OptCtrl oc; oc.id = o.id;
        if (!o.choices.empty()) {
            oc.isChoice = true;
            // The charset cell drives the collation filter; other cells are inert.
            std::function<void()> onChanged = [] {};
            if (o.id == L"charset") onChanged = [applyCollFilter] { applyCollFilter(); };
            auto* cell = new PopupListCell(optionsPage_, onChanged);
            wxArrayString disp, vals;
            disp.Add(tr(L"默认")); vals.Add(wxString());             // stored "" = engine default
            for (const auto& c : o.choices) { disp.Add(c); vals.Add(c); }
            cell->SetChoices(disp, vals);
            cell->SetStored(cur);
            cell->SetMinSize(wxSize(240, kRowH));
            oc.ctrl = cell;
            row->Add(cell, 0, wxALIGN_CENTER_VERTICAL);
            if (o.id == L"charset") link->charsetCell = cell;
            if (!o.groupSource.IsEmpty()) { link->collationCell = cell; link->collationSpec = o; }
        } else {
            oc.isChoice = false;
            auto* tc = new wxTextCtrl(optionsPage_, wxID_ANY, cur);
            tc->SetFont(Mono(9));
            tc->SetMinSize(wxSize(240, -1));
            oc.ctrl = tc;
            row->Add(tc, 0, wxALIGN_CENTER_VERTICAL);
        }
        optionsSizer_->Add(row, 0, wxLEFT | wxRIGHT | wxTOP, 14);
        optionCtrls_.push_back(oc);
    }

    applyCollFilter();   // seed the collation list from the initial charset value

    optionsPage_->Layout();
    optionsPage_->Thaw();
}

// Read one form control's current value (PopupListCell stored value / trimmed text).
// Takes the primitives, not OptCtrl (a private nested type these free helpers can't name).
namespace {
wxString ReadOpt(bool isChoice, wxWindow* ctrl)
{
    if (isChoice) return static_cast<PopupListCell*>(ctrl)->GetStored();
    wxString v = static_cast<wxTextCtrl*>(ctrl)->GetValue();
    v.Trim(true).Trim(false);
    return v;
}
// Route an option value into the matching TableOptions field by id.
void AssignOpt(db::TableOptions& opts, const wxString& id, const wxString& v)
{
    if (id == L"engine")             opts.engine = v;
    else if (id == L"charset")       opts.charset = v;
    else if (id == L"collation")     opts.collation = v;
    else if (id == L"rowFormat")     opts.rowFormat = v;
    else if (id == L"tablespace")    opts.tablespace = v;
    else if (id == L"maxRows")       opts.maxRows = v;
    else if (id == L"autoIncrement") opts.autoIncrement = v;
    else if (id == L"avgRowLength")  opts.avgRowLength = v;
    else if (id == L"checksum")      opts.checksum = v;
}
} // namespace

void TableDesignView::SaveOptions()
{
    if (!conn_ || !profile_ || table_.IsEmpty()) return;
    db::TableEdit edit;
    edit.db = db_; edit.table = table_;
    edit.hasOptions = true;
    for (const OptCtrl& oc : optionCtrls_)
        AssignOpt(edit.options, oc.id, ReadOpt(oc.isChoice, oc.ctrl));
    ExecuteEdit(edit);
}

// Fold the form's non-empty option values into a whole-table snapshot (⑦TABLE DDL).
// BuildTableModel helper — kept here so the options collection stays in this TU.
void TableDesignView::AppendOptionsModel(db::TableModel& model) const
{
    for (const OptCtrl& oc : optionCtrls_) {
        const wxString v = ReadOpt(oc.isChoice, oc.ctrl);
        if (v.IsEmpty()) continue;
        model.hasOptions = true;
        AssignOpt(model.options, oc.id, v);
    }
}

// Read the whole options form into `out`. False when there is no form (dialect has no
// alterable options).
bool TableDesignView::CurrentOptions(db::TableOptions& out) const
{
    if (optionCtrls_.empty()) return false;
    for (const OptCtrl& oc : optionCtrls_)
        AssignOpt(out, oc.id, ReadOpt(oc.isChoice, oc.ctrl));
    return true;
}

// Do the form's options differ from what the table was loaded with? engine/charset/
// collation compare to the loaded baseline; the extra fields are "changed" once set
// (row_format/checksum any value; the numeric ones any non-zero).
bool TableDesignView::OptionsChanged(const db::TableOptions& c) const
{
    auto numSet = [](const wxString& v) { return !v.IsEmpty() && v != L"0"; };
    return c.engine    != origOptions_.engine    ||
           c.charset   != origOptions_.charset   ||
           c.collation != origOptions_.collation ||
           !c.rowFormat.IsEmpty() || !c.checksum.IsEmpty() ||
           numSet(c.maxRows) || numSet(c.autoIncrement) || numSet(c.avgRowLength);
}

// Assemble EVERY pending edit into one TableEdit — the single source for the ⑥SQL 预览
// tab and the per-tab dirty check. Columns/index/FK/trigger fold in via their own
// builders; comment/options only when they differ from the loaded baseline.
void TableDesignView::BuildFullEdit(db::TableEdit& edit) const
{
    edit = db::TableEdit{};
    edit.db = db_; edit.table = table_;
    wxString e;
    BuildTableEdit(edit, e);                             // columns (may stay empty)
    db::TableEdit t;
    if (BuildIndexEdits(t, e))   edit.indexes  = std::move(t.indexes);
    t = db::TableEdit{};
    if (BuildFkEdits(t, e))      edit.fks       = std::move(t.fks);
    t = db::TableEdit{};
    if (BuildTriggerEdits(t, e)) edit.triggers  = std::move(t.triggers);
    if (commentEdit_) {
        const wxString c = commentEdit_->GetValue();
        if (c != origComment_) { edit.hasComment = true; edit.comment = c; }
    }
    db::TableOptions cur;
    if (CurrentOptions(cur) && OptionsChanged(cur)) { edit.hasOptions = true; edit.options = cur; }
}

} // namespace ui
