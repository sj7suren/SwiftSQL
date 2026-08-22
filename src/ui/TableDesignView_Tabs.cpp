// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// TableDesignView_Tabs.cpp — the context toolbar, 字段-tab context-menu / clipboard /
// type-inference operations, the right-hand attribute panel, the edit-model / save /
// DDL-generation subsystem, 注释 save, and the close / dirty-tracking guards. Split
// out of TableDesignView.cpp per the ≤1000-line charter; method bodies are the exact
// originals (behavior unchanged). BoolOn / ModelsEqual move here with their sole users.
#include "ui/TableDesignView.h"
#include "ui/DesignGridCells.h"
#include "ui/I18n.h"

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/clipbrd.h>
#include <wx/stc/stc.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace ui {

namespace {

// A bag/panel value read as a boolean ("1"/"true"/… = on; ""/"0"/"false" = off).
bool BoolOn(const wxString& v)
{
    return !v.IsEmpty() && v != L"0" && !v.IsSameAs(L"false", false);
}

// True when two column models are structurally identical. Used to diff a row's
// current edit against its load-time baseline: only a real difference counts as a
// Modify (empty bag entries are ignored so absent == "" compares equal, and the
// attrs bag is compared order-independently).
bool ModelsEqual(const db::ColumnModel& a, const db::ColumnModel& b)
{
    if (a.name != b.name
        || !a.type.IsSameAs(b.type, /*caseSensitive=*/false)
        || a.length != b.length
        || a.scale != b.scale
        || a.notNull != b.notNull
        || a.defaultVal != b.defaultVal
        || a.comment != b.comment
        || a.collation != b.collation
        || a.autoIncrement != b.autoIncrement
        || a.generatedExpr != b.generatedExpr
        || a.generatedStored != b.generatedStored)
        return false;

    auto bag = [](const db::ColumnModel& m) {
        std::map<wxString, wxString> mp;
        for (const auto& kv : m.attrs)
            if (!kv.second.IsEmpty()) mp[kv.first] = kv.second;
        return mp;
    };
    return bag(a) == bag(b);
}

} // namespace

// The context toolbar (M2.2): the top menu follows the active tab. Each tab supplies
// its own button set; 关闭 is always appended. Buttons needing an editable table are
// dimmed until one is loaded. Called on tab switch and on editability changes.
void TableDesignView::RebuildToolbar(int tab)
{
    if (!toolbar_ || !toolbarSizer_) return;
    toolbar_->Freeze();
    toolbarSizer_->Clear(/*delete_windows*/true);

    const bool editable = IsEditable();

    struct TbItem { icons::Glyph g; const wxChar* label; wxColour fg;
                    std::function<void()> fn; const wxChar* tip; bool needEd; };
    const TbItem save{ icons::Glyph::Save, L"保存", theme::kPrimary,
                       [this]{ OnSaveActiveTab(); }, L"保存该页变更", true };

    std::vector<TbItem> items;
    switch (tab) {
    case Tab_Fields:
        items = { save,
            { icons::Glyph::RowAdd,    L"增加字段", theme::kTextBody, [this]{ OnAddField(); },        L"在末尾追加字段",   true },
            { icons::Glyph::RowInsert, L"插入字段", theme::kTextBody, [this]{ OnInsertField(); },     L"在当前字段上方插入", true },
            { icons::Glyph::RowDelete, L"删除字段", theme::kTextBody, [this]{ OnDeleteField(); },     L"删除当前字段",     true },
            { icons::Glyph::Key,       L"主键",     theme::kDotAmber, [this]{ OnToggleKeyOfCurrent(); }, L"设为/取消主键", true },
            { icons::Glyph::ArrowUp,   L"上移",     theme::kTextBody, [this]{ OnMoveField(-1); },     L"上移当前字段",     true },
            { icons::Glyph::ArrowDown, L"下移",     theme::kTextBody, [this]{ OnMoveField(1); },      L"下移当前字段",     true } };
        break;
    case Tab_Indexes:
        items = { save,
            { icons::Glyph::RowAdd,    L"新增索引", theme::kTextBody, [this]{ OnAddIndex(); },    L"新增索引",     true },
            { icons::Glyph::RowDelete, L"删除索引", theme::kTextBody, [this]{ OnDeleteIndex(); }, L"删除选中索引", true } };
        break;
    case Tab_ForeignKeys:
        items = { save,
            { icons::Glyph::RowAdd,    L"新增外键", theme::kTextBody, [this]{ OnAddForeignKey(); },    L"新增外键",     true },
            { icons::Glyph::RowDelete, L"删除外键", theme::kTextBody, [this]{ OnDeleteForeignKey(); }, L"删除选中外键", true } };
        break;
    case Tab_Triggers:
        items = { save,
            { icons::Glyph::RowAdd,    L"新增", theme::kTextBody, [this]{ OnAddTrigger(); },    L"新增触发器", true },
            { icons::Glyph::RowDelete, L"删除", theme::kTextBody, [this]{ OnDeleteTrigger(); }, L"删除选中触发器", true },
            { icons::Glyph::ArrowUp,   L"上移", theme::kTextBody, [this]{ OnMoveTrigger(-1); }, L"上移触发器", true },
            { icons::Glyph::ArrowDown, L"下移", theme::kTextBody, [this]{ OnMoveTrigger(1); },  L"下移触发器", true } };
        break;
    case Tab_Options:
    case Tab_Comment:
        items = { save };
        break;
    case Tab_SqlPreview:
    case Tab_TableDdl:
        items = { { icons::Glyph::Copy, L"复制", theme::kTextBody,
                    [this]{ OnCopyActiveCode(); }, L"复制脚本到剪贴板", false } };
        break;
    default: break;
    }

    bool first = true;
    for (const auto& it : items) {
        auto* b = new FlatButton(toolbar_, it.g, tr(it.label), it.fg, it.fn);
        b->SetToolTip(tr(it.tip));
        if (it.needEd) b->SetEnabledLook(editable);
        toolbarSizer_->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, first ? 12 : 4);
        first = false;
    }
    // 关闭 is always available, set apart on the right of the strip.
    auto* closeB = new FlatButton(toolbar_, icons::Glyph::Close, tr(L"关闭"),
                                  theme::kTextSecondary, [this]{ OnCloseTab(); });
    closeB->SetToolTip(tr(L"关闭该表设计标签"));
    toolbarSizer_->Add(closeB, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

    toolbar_->Layout();
    if (toolbar_->GetParent()) toolbar_->GetParent()->Layout();   // re-fit the header
    toolbar_->Thaw();
    // Repaint the toolbar AND the header behind it: switching to a tab with fewer
    // buttons frees the strip the removed buttons (上移/下移/…) occupied, and without an
    // explicit refresh their pixels ghost on the header background.
    toolbar_->Refresh();
    if (toolbar_->GetParent()) toolbar_->GetParent()->Refresh();
}

// 保存 dispatcher: routes to the active tab's save path. Only 字段 is wired today;
// the index/FK/trigger/option/comment saves land in M3/M4.
void TableDesignView::OnSaveActiveTab()
{
    if (!book_) return;
    switch (book_->GetSelection()) {
    case Tab_Fields:      OnSaveChanges();     break;
    case Tab_Indexes:     SaveIndexChanges();  break;
    case Tab_ForeignKeys: SaveFkChanges();     break;
    case Tab_Triggers:    SaveTriggerChanges();break;
    case Tab_Options:     SaveOptions();       break;
    case Tab_Comment:     SaveComment();       break;
    default:
        wxMessageBox(tr(L"该页的保存将在后续版本实现。"), tr(L"开发中"),
                     wxOK | wxICON_INFORMATION, this);
    }
}

// 复制 for the two code panes (SQL 预览 / TABLE DDL) → system clipboard.
void TableDesignView::OnCopyActiveCode()
{
    wxStyledTextCtrl* pane = (book_ && book_->GetSelection() == Tab_SqlPreview) ? sqlPreview_ : ddl_;
    if (!pane) return;
    const wxString text = pane->GetText();
    if (text.IsEmpty()) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->Close();
    }
}

// Right-click context menu (mirrors the toolbar + copy/paste/duplicate), per the
// docs/UI reference. Copy/Paste act on the 序号 selection; the rest on the current row.
void TableDesignView::ShowFieldMenu(int /*row*/)
{
    wxMenu m;
    auto add = [&](const wxString& label, icons::Glyph g, std::function<void()> fn,
                   bool enabled = true) {
        auto* it = m.Append(wxID_ANY, label);
        it->SetBitmap(icons::Stroke(g, 14, theme::kTextBody, 1.7));
        it->Enable(enabled);
        m.Bind(wxEVT_MENU, [fn](wxCommandEvent&) { fn(); }, it->GetId());
    };
    add(tr(L"复制"),     icons::Glyph::Copy,      [this] { CopySelectedRows(); });
    add(tr(L"粘贴"),     icons::Glyph::Paste,     [this] { PasteRows(); }, !clip_.empty());
    m.AppendSeparator();
    add(tr(L"增加字段"), icons::Glyph::RowAdd,    [this] { OnAddField(); });
    add(tr(L"插入字段"), icons::Glyph::RowInsert, [this] { OnInsertField(); });
    add(tr(L"复制字段"), icons::Glyph::Copy,      [this] { DuplicateCurrentField(); });
    add(tr(L"删除字段"), icons::Glyph::RowDelete, [this] { OnDeleteField(); });
    m.AppendSeparator();
    add(tr(L"主键"),     icons::Glyph::Key,       [this] { OnToggleKeyOfCurrent(); });
    m.AppendSeparator();
    add(tr(L"上移"),     icons::Glyph::ArrowUp,   [this] { OnMoveField(-1); });
    add(tr(L"下移"),     icons::Glyph::ArrowDown, [this] { OnMoveField(1); });
    PopupMenu(&m);
}

TableDesignView::RowClip TableDesignView::CaptureRow(const FieldRow* fr) const
{
    RowClip c;
    c.name = fr->name->GetValue();     c.type    = fr->type->GetValue();
    c.length = fr->length->GetValue(); c.scale   = fr->scale->GetValue();
    c.comment = fr->comment->GetValue();
    c.defaultVal = fr->defaultVal;
    c.notNull = fr->notNull->GetValue(); c.virt  = fr->virt->GetValue();
    c.attrVals = fr->attrVals;
    return c;
}

void TableDesignView::ApplyRowClip(FieldRow* fr, const RowClip& c)
{
    fr->name->ChangeValue(c.name);     fr->type->ChangeValue(c.type);
    fr->length->ChangeValue(c.length); fr->scale->ChangeValue(c.scale);
    fr->comment->ChangeValue(c.comment);
    fr->defaultVal = c.defaultVal;
    fr->notNull->SetValue(c.notNull);  fr->virt->SetValue(c.virt);
    fr->attrVals = c.attrVals;
    fr->origName.clear();              // a pasted/duplicated row is a NEW column
    fr->hasBaseline = false;
}

// Ctrl+C: snapshot the selected 序号 rows (or the current row if none selected).
void TableDesignView::CopySelectedRows()
{
    FlushAttrPanel();
    std::vector<int> idx(selRows_.begin(), selRows_.end());
    if (idx.empty() && attrRow_ >= 0) idx.push_back(attrRow_);
    if (idx.empty()) return;
    std::sort(idx.begin(), idx.end());
    clip_.clear();
    for (int i : idx)
        if (i >= 0 && i < static_cast<int>(rows_.size()))
            clip_.push_back(CaptureRow(rows_[i].get()));
}

// Ctrl+V / 粘贴. If the selection contains blank NEW rows (added-but-unfilled), the
// clipboard fills THOSE in order; anything left over (more copied than blank targets),
// or the "nothing suitable selected" case, appends as fresh rows. Filled rows are
// re-selected so the result is visible.
void TableDesignView::PasteRows()
{
    if (clip_.empty() || !IsEditable()) return;   // base: same condition; NewTableView relaxes
    FlushAttrPanel();

    // A fill target = a selected row that is a blank, newly-added column.
    auto isBlankNew = [](const FieldRow* fr) {
        auto empty = [](const wxString& s) { wxString t = s; return t.Trim().Trim(false).IsEmpty(); };
        return fr->origName.IsEmpty() && empty(fr->name->GetValue()) && empty(fr->type->GetValue());
    };
    std::vector<int> targets;
    for (int i : selRows_)
        if (i >= 0 && i < static_cast<int>(rows_.size()) && isBlankNew(rows_[i].get()))
            targets.push_back(i);
    std::sort(targets.begin(), targets.end());   // selRows_ is already ordered, but be explicit

    formScroll_->Freeze();
    std::vector<int> filled;
    size_t k = 0;
    for (; k < targets.size() && k < clip_.size(); ++k) {     // fill selected blank rows
        ApplyRowClip(rows_[targets[k]].get(), clip_[k]);
        ApplyRowTypeEditability(targets[k]);
        filled.push_back(targets[k]);
    }
    for (; k < clip_.size(); ++k) {                           // leftover / no-target → append
        FieldRow* fr = AddRowControls();
        ApplyRowClip(fr, clip_[k]);
        const int idx = static_cast<int>(rows_.size()) - 1;
        ApplyRowTypeEditability(idx);
        filled.push_back(idx);
    }
    RenumberRows();
    UpdateKeyBadges();
    formSizer_->Layout();
    formScroll_->FitInside();

    selRows_.clear();
    for (int i : filled) selRows_.insert(i);
    selAnchor_ = filled.empty() ? -1 : filled.front();
    attrRow_ = -1;                                            // force the panel to rebind
    if (!filled.empty()) SetCurrentRow(filled.front(), /*focusName=*/false);
    RepaintSelection();
    formScroll_->Thaw();
}

// Context menu 复制字段: clone the current row as a new row right below it.
void TableDesignView::DuplicateCurrentField()
{
    if (attrRow_ < 0 || attrRow_ >= static_cast<int>(rows_.size())) return;
    FlushAttrPanel();
    const RowClip c = CaptureRow(rows_[attrRow_].get());
    const int at = std::min(attrRow_ + 1, static_cast<int>(rows_.size()));  // clamped below
    selRows_.clear(); selAnchor_ = -1;
    formScroll_->Freeze();
    FieldRow* fr = AddRowControls();
    ApplyRowClip(fr, c);
    const int target = std::min(at, static_cast<int>(rows_.size()) - 1);
    for (int i = static_cast<int>(rows_.size()) - 1; i > target; --i) SwapAdjacentRows(i - 1);
    RenumberRows();
    UpdateKeyBadges();
    ApplyRowTypeEditability(target);
    formSizer_->Layout();
    formScroll_->FitInside();
    attrRow_ = -1;
    SetCurrentRow(target, /*focusName=*/true);
    RepaintSelection();                        // clear any prior selection tint
    formScroll_->Thaw();
}

// Name → default type: leaving the name field fills an EMPTY type from the field name.
// date* ⇒ datetime; id/number/type/state/status ⇒ int. The concrete name is drawn from
// the active dialect's own type list (ADR-014) so it's valid per-database.
void TableDesignView::InferTypeFromName(FieldRow* fr)
{
    if (populating_ || !fr || !profile_) return;
    wxString cur = fr->type->GetValue(); cur.Trim().Trim(false);
    if (!cur.IsEmpty()) return;                    // never override a user-set type
    const wxString name = fr->name->GetValue();
    if (name.IsEmpty()) return;

    // Tokenise the name on non-alphanumerics AND camelCase boundaries, lower-cased —
    // so we match WHOLE tokens, not naked substrings. This is what keeps "update_status"
    // → int (via "status"), and stops "width"/"guid"/"video"/"validate" false-matching.
    std::vector<wxString> toks;
    wxString t;
    for (size_t i = 0; i < name.length(); ++i) {
        const wxUniChar ch = name[i];
        if (!wxIsalnum(ch)) { if (!t.IsEmpty()) { toks.push_back(t.Lower()); t.Clear(); } continue; }
        if (i > 0 && wxIsupper(ch) && wxIslower(name[i - 1]) && !t.IsEmpty()) {
            toks.push_back(t.Lower()); t.Clear();     // camelCase split
        }
        t += ch;
    }
    if (!t.IsEmpty()) toks.push_back(t.Lower());
    auto hasTok = [&](const wchar_t* w) {
        for (const wxString& x : toks) if (x == w) return true;
        return false;
    };

    auto pick = [this](std::initializer_list<const wchar_t*> prefs) -> wxString {
        for (const wchar_t* pref : prefs)
            for (const auto& ty : profile_->Types())
                if (ty.name.IsSameAs(pref, /*caseSensitive=*/false)) return ty.name;
        return wxString(*prefs.begin());           // fallback: first preference literal
    };

    wxString want;
    if (hasTok(L"date") || hasTok(L"datetime") || hasTok(L"time") || hasTok(L"timestamp"))
        want = pick({ L"datetime", L"timestamp", L"date" });
    else if (hasTok(L"id") || hasTok(L"number") || hasTok(L"type")
             || hasTok(L"state") || hasTok(L"status"))
        want = pick({ L"int", L"integer", L"number" });
    if (want.IsEmpty()) return;

    fr->type->ChangeValue(want);                   // event-free set
    const int r = RowIndexOf(fr);
    if (r < 0) return;
    ApplyRowTypeEditability(r);
    if (r == attrRow_) {
        if (fieldLbl_) fieldLbl_->SetLabel(fr->name->GetValue() + L"  ·  " + want);
        if (AttrSignature(r) != panelSig_) { FlushAttrPanel(); BuildAttrPanel(r); }
    }
}

// View-level shortcuts: Ctrl+S save · Ctrl+C copy selected rows · Ctrl+V paste. Row
// copy/paste only fire when a 序号 selection / clipboard exists, so plain text Ctrl+C
// inside a cell still works (falls through via Skip).
void TableDesignView::OnViewKey(wxKeyEvent& e)
{
    if (e.ControlDown() && !e.AltDown()) {
        const int k = e.GetKeyCode();
        if (k == 'S') { SaveViaShortcut(); return; }   // base = 原字段保存; NewTableView = 创建
        // Row copy/paste only when focus is NOT in an editable text cell (e.g. the 序号
        // gutter is focused) — otherwise let the native EDIT do text clipboard.
        const bool inEdit = dynamic_cast<wxTextCtrl*>(wxWindow::FindFocus()) != nullptr;
        if (!inEdit) {
            if (k == 'C' && !selRows_.empty()) { CopySelectedRows(); return; }
            if (k == 'V' && !clip_.empty())    { PasteRows();        return; }
        }
    }
    e.Skip();
}

// 关闭: confirm (save / discard / cancel when dirty; a plain confirm when clean),
// then ask the host to remove this tab. onCloseRequest_ deletes the page deferred, so
// destroying `this` doesn't happen underneath us.
void TableDesignView::OnCloseTab()
{
    if (HasUnsavedChanges()) {
        const int r = wxMessageBox(
            tr(L"表结构已修改，是否保存更改？\n\n"
               L"「是」保存并关闭     「否」放弃更改并关闭     「取消」不关闭"),
            tr(L"关闭表设计"), wxYES_NO | wxCANCEL | wxICON_WARNING, this);
        if (r == wxCANCEL) return;
        if (r == wxYES && !SaveChanges()) return;   // save cancelled/failed → keep open
        // wxNO → discard and close
    } else if (wxMessageBox(tr(L"确定关闭该表设计？"), tr(L"关闭表设计"),
                            wxYES_NO | wxICON_QUESTION, this) != wxYES) {
        return;
    }
    if (onCloseRequest_) onCloseRequest_();
}

// ---- right-hand dialect attribute panel -----------------------------------
// Rebuild the panel's controls for `row` from profile_->ApplicableAttrs(type):
// each attribute becomes a control chosen by AttrDescriptor::editor; the form
// already owns comment/notNull/virtual so 注释 is skipped here (default value +
// charset/collation + dialect attrs live in the panel).
void TableDesignView::BuildAttrPanel(int row)
{
    attrScroll_->Freeze();

    attrSizer_->Clear(/*delete_windows=*/true);   // destroy all prior controls
    panelCtrls_.clear();
    attrHint_ = nullptr;
    fieldLbl_ = nullptr;
    defaultCtrl_ = nullptr;
    charsetCombo_ = nullptr;
    collationCombo_ = nullptr;
    panelSig_.clear();

    // persistent panel title
    auto* attrTitle = new wxStaticText(attrScroll_, wxID_ANY, tr(L"字段属性"));
    attrTitle->SetFont(Ui(10, true));
    attrTitle->SetForegroundColour(theme::kText);
    attrSizer_->Add(attrTitle, 0, wxLEFT | wxTOP | wxRIGHT, 14);

    const bool valid = profile_ && row >= 0 && row < static_cast<int>(rows_.size())
                       && IsEditable();
    if (!valid) {
        attrHint_ = new wxStaticText(attrScroll_, wxID_ANY,
                                     tr(L"选择一个字段以编辑其方言属性"));
        attrHint_->SetFont(Ui(9));
        attrHint_->SetForegroundColour(theme::kTextFaint);
        attrSizer_->Add(attrHint_, 0, wxALL, 14);
        attrRow_ = -1;
        attrScroll_->FitInside();
        attrScroll_->Layout();
        attrScroll_->Thaw();
        return;
    }

    attrRow_ = row;
    FieldRow* fr = rows_[row].get();
    const wxString typeName = fr->type->GetValue();
    auto valueOf = [&](const wxString& id) -> wxString {
        for (const auto& kv : fr->attrVals) if (kv.first == id) return kv.second;
        return wxString();
    };

    fieldLbl_ = new wxStaticText(attrScroll_, wxID_ANY,
        fr->name->GetValue() + L"  ·  " + typeName);
    fieldLbl_->SetFont(Mono(9, true));
    fieldLbl_->SetForegroundColour(theme::kSynKeyword);
    attrSizer_->Add(fieldLbl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);

    auto addLabel = [&](const wxString& text) {
        auto* lbl = new wxStaticText(attrScroll_, wxID_ANY, text);
        lbl->SetFont(Ui(9));
        lbl->SetForegroundColour(theme::kTextSecondary);
        attrSizer_->Add(lbl, 0, wxLEFT | wxRIGHT | wxTOP, 14);
    };

    // 默认值 — a universal core property (every dialect), edited here rather than in
    // the form; written to ColumnModel.defaultVal via rows_[row].defaultVal.
    addLabel(tr(L"默认值"));
    defaultCtrl_ = new wxTextCtrl(attrScroll_, wxID_ANY, fr->defaultVal);
    defaultCtrl_->SetFont(Mono(9));
    attrSizer_->Add(defaultCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT, 14);

    for (const db::AttrDescriptor& a : profile_->ApplicableAttrs(typeName)) {
        if (a.id == L"comment") continue;   // owned by the form 注释 column

        addLabel(tr(a.label));

        wxControl* ctrl = nullptr;
        const wxString cur = valueOf(a.id);

        // charset / collation → linked, candidate-completing combos (data source =
        // the driver's GetCreateDatabaseCaps, same as the New-Database dialog). Not
        // a dialect `if`: keyed on the attribute id, applied wherever ApplicableAttrs
        // surfaces these ids; empty candidates simply degrade to free text.
        if (a.id == L"charset" || a.id == L"collation") {
            auto* combo = new wxComboBox(attrScroll_, wxID_ANY, wxEmptyString,
                                         wxDefaultPosition, wxDefaultSize,
                                         0, nullptr, wxCB_DROPDOWN);
            combo->SetFont(Mono(9));
            if (a.id == L"charset") {
                charsetCombo_ = combo;
                wxArrayString items;
                if (const db::DbCreateOption* co = FindCreateOption(L"charset"))
                    for (const auto& ch : co->choices) items.Add(ch);
                combo->Append(items);
                combo->AutoComplete(items);
                combo->SetValue(cur);
                auto refilter = [this](wxCommandEvent& e) {
                    if (charsetCombo_) RefillCollationCombo(charsetCombo_->GetValue());
                    e.Skip();
                };
                combo->Bind(wxEVT_COMBOBOX, refilter);
                combo->Bind(wxEVT_TEXT, refilter);
            } else {
                collationCombo_ = combo;
                combo->SetValue(cur);
                const wxString cs = charsetCombo_ ? charsetCombo_->GetValue()
                                                  : valueOf(L"charset");
                RefillCollationCombo(cs);   // filters candidates, keeps cur
            }
            ctrl = combo;
        }
        else switch (a.editor) {
        case db::AttrDescriptor::Bool: {
            auto* cb = new wxCheckBox(attrScroll_, wxID_ANY, wxEmptyString);
            cb->SetValue(BoolOn(cur));
            ctrl = cb;
            break;
        }
        case db::AttrDescriptor::Choice: {
            wxArrayString ch;
            ch.Add(wxEmptyString);
            for (const auto& c : a.choices) ch.Add(c);
            auto* cc = new wxChoice(attrScroll_, wxID_ANY,
                                    wxDefaultPosition, wxDefaultSize, ch);
            cc->SetStringSelection(cur.IsEmpty() ? wxString() : cur);
            if (cc->GetSelection() == wxNOT_FOUND) cc->SetSelection(0);
            ctrl = cc;
            break;
        }
        case db::AttrDescriptor::Expr:
        case db::AttrDescriptor::Int:
        case db::AttrDescriptor::Text:
        default: {
            auto* tc = new wxTextCtrl(attrScroll_, wxID_ANY, cur);
            tc->SetFont(Mono(9));
            ctrl = tc;
            break;
        }
        }
        attrSizer_->Add(ctrl, 0, wxEXPAND | wxLEFT | wxRIGHT, 14);
        panelCtrls_.push_back({ a.id, a.editor, ctrl });
    }

    panelSig_ = AttrSignature(row);   // record structure so a same-shape row reuses this
    attrScroll_->FitInside();
    attrScroll_->Layout();
    attrScroll_->Thaw();
}

// The ordered list of attribute ids the panel shows for a row's type — its
// structure key. Two rows with equal signatures have the exact same control set, so
// switching between them only needs a value reload (ReloadAttrPanel), not a rebuild.
wxString TableDesignView::AttrSignature(int row) const
{
    if (!profile_ || row < 0 || row >= static_cast<int>(rows_.size())) return wxString();
    const wxString typeName = rows_[row]->type->GetValue();
    wxString sig;
    for (const db::AttrDescriptor& a : profile_->ApplicableAttrs(typeName)) {
        if (a.id == L"comment") continue;
        sig += a.id; sig += L'|';
    }
    return sig;
}

// Fast path used when the new row's AttrSignature matches the built panel: keep all
// controls, just push the new row's values into them (no destroy/create). Values are
// set with ChangeValue/SetValue that do NOT emit change events, so the charset→
// collation link doesn't re-fire; collation is refilled only when charset differs.
void TableDesignView::ReloadAttrPanel(int row)
{
    attrRow_ = row;
    FieldRow* fr = rows_[row].get();
    auto valueOf = [&](const wxString& id) -> wxString {
        for (const auto& kv : fr->attrVals) if (kv.first == id) return kv.second;
        return wxString();
    };

    attrScroll_->Freeze();

    if (fieldLbl_)
        fieldLbl_->SetLabel(fr->name->GetValue() + L"  ·  " + fr->type->GetValue());
    if (defaultCtrl_)
        defaultCtrl_->ChangeValue(fr->defaultVal);

    if (charsetCombo_) {
        const wxString newCs = valueOf(L"charset");
        const bool csChanged = (charsetCombo_->GetValue() != newCs);
        charsetCombo_->ChangeValue(newCs);          // no event → no re-filter recursion
        if (collationCombo_) {
            if (csChanged) RefillCollationCombo(newCs);
            collationCombo_->ChangeValue(valueOf(L"collation"));
        }
    } else if (collationCombo_) {
        collationCombo_->ChangeValue(valueOf(L"collation"));
    }

    for (const PanelCtrl& pc : panelCtrls_) {
        if (pc.id == L"charset" || pc.id == L"collation") continue;   // handled above
        const wxString cur = valueOf(pc.id);
        if (auto* cb = wxDynamicCast(pc.ctrl, wxCheckBox))
            cb->SetValue(BoolOn(cur));
        else if (auto* combo = wxDynamicCast(pc.ctrl, wxComboBox))
            combo->ChangeValue(cur);
        else if (auto* choice = wxDynamicCast(pc.ctrl, wxChoice)) {
            choice->SetStringSelection(cur);
            if (choice->GetSelection() == wxNOT_FOUND) choice->SetSelection(0);
        } else if (auto* tc = wxDynamicCast(pc.ctrl, wxTextCtrl))
            tc->ChangeValue(cur);
    }

    attrScroll_->Layout();
    attrScroll_->Thaw();
}

// Read the live panel controls back into the bound row's state. Detects the widget
// type (charset/collation are wxComboBox, not the wxTextCtrl their Text editor kind
// implies), so the read stays correct for the special combos.
void TableDesignView::FlushAttrPanel()
{
    if (attrRow_ < 0 || attrRow_ >= static_cast<int>(rows_.size())) return;
    FieldRow* fr = rows_[attrRow_].get();

    if (defaultCtrl_)
        fr->defaultVal = defaultCtrl_->GetValue().Trim().Trim(false);

    for (const PanelCtrl& pc : panelCtrls_) {
        wxString val;
        if (auto* cb = wxDynamicCast(pc.ctrl, wxCheckBox))
            val = cb->GetValue() ? L"1" : wxString();
        else if (auto* combo = wxDynamicCast(pc.ctrl, wxComboBox))
            val = combo->GetValue();
        else if (auto* choice = wxDynamicCast(pc.ctrl, wxChoice))
            val = choice->GetStringSelection();
        else if (auto* tc = wxDynamicCast(pc.ctrl, wxTextCtrl))
            val = tc->GetValue();
        val.Trim().Trim(false);
        bool found = false;
        for (auto& kv : fr->attrVals)
            if (kv.first == pc.id) { kv.second = val; found = true; break; }
        if (!found) fr->attrVals.emplace_back(pc.id, val);
    }
}

// The CREATE-DATABASE option carrying candidate values for `id` ("charset" /
// "collation"), or nullptr when the driver doesn't expose it.
const db::DbCreateOption* TableDesignView::FindCreateOption(const wxString& id) const
{
    for (const auto& o : createCaps_.options)
        if (o.id == id) return &o;
    return nullptr;
}

// Repopulate the collation combo with only the collations belonging to `charset`
// (via the option's parallel choiceGroup), preserving the current value. Empty
// charset ⇒ show all; no collation option ⇒ combo stays free-text/empty.
void TableDesignView::RefillCollationCombo(const wxString& charset)
{
    if (!collationCombo_) return;
    const wxString keep = collationCombo_->GetValue();
    collationCombo_->Clear();          // clears items and the text field
    wxArrayString items;
    if (const db::DbCreateOption* co = FindCreateOption(L"collation")) {
        for (size_t i = 0; i < co->choices.size(); ++i) {
            const bool match = charset.IsEmpty()
                || (i < co->choiceGroup.size() && co->choiceGroup[i] == charset);
            if (match) items.Add(co->choices[i]);
        }
    }
    collationCombo_->Append(items);
    collationCombo_->AutoComplete(items);
    collationCombo_->SetValue(keep);   // don't lose the chosen/typed value
}

// ---- edit model assembly --------------------------------------------------
// Assemble one form row (+ its side-panel attributes) into a db::ColumnModel: the
// form controls supply the strong-typed core; the panel's attrVals are dispatched by
// AttrDescriptor.core — core ids to core fields, everything else to the bag (ADR-014
// contract: bag Bools are "1"=on / ""=off).
db::ColumnModel TableDesignView::RowToModel(int r) const
{
    db::ColumnModel m;
    const FieldRow* fr = rows_[r].get();
    auto trimmed = [](const wxString& s) { wxString t = s; return t.Trim().Trim(false); };
    m.name       = trimmed(fr->name->GetValue());
    m.type       = trimmed(fr->type->GetValue());
    m.length     = trimmed(fr->length->GetValue());
    m.scale      = trimmed(fr->scale->GetValue());
    m.notNull    = fr->notNull->GetValue();
    m.comment    = trimmed(fr->comment->GetValue());
    // 虚拟 checkbox: checked = VIRTUAL storage, unchecked = STORED (default).
    m.generatedStored = !fr->virt->GetValue();
    m.origName   = fr->origName;
    m.defaultVal = fr->defaultVal;   // default value lives in the panel

    for (const auto& kv : fr->attrVals) {
        const wxString& id = kv.first;
        const wxString& v  = kv.second;
        const db::AttrDescriptor* ad = nullptr;
        if (profile_)
            for (const auto& a : profile_->Attributes())
                if (a.id == id) { ad = &a; break; }
        const bool core = ad && ad->core;
        if (core) {
            if (id == L"comment")            { if (!v.IsEmpty()) m.comment = v; }
            else if (id == L"autoIncrement") m.autoIncrement = BoolOn(v);
            else if (id == L"generated")     m.generatedExpr = v;
            else if (id == L"collation")     m.collation = v;
            else if (!v.IsEmpty())           m.attrs.emplace_back(id, v); // unknown core → bag
        } else if (!v.IsEmpty()) {
            m.attrs.emplace_back(id, v);     // long-tail bag (absent = "")
        }
    }
    return m;
}

// Collect every form row into a db::TableEdit (Add / Modify / Drop), diffing against
// the loaded snapshot. Returns false + err when nothing changed or a row is invalid.
bool TableDesignView::BuildTableEdit(db::TableEdit& edit, wxString& err) const
{
    if (!conn_ || !profile_) { err = tr(L"未连接"); return false; }
    edit.db = db_;
    edit.table = table_;
    edit.columns.clear();

    std::set<wxString> seen;
    std::map<wxString, wxString> reorder;    // origName → AFTER target ("" = FIRST)
    ComputeColumnReorder(reorder);           // existing columns moved via 上移/下移 (MySQL)
    for (int r = 0; r < static_cast<int>(rows_.size()); ++r) {
        db::ColumnModel m = RowToModel(r);
        // A brand-new row left entirely blank (user added it but didn't fill it) is
        // silently ignored — not an error. A partially-filled new row still must have
        // both a name and a type.
        if (m.origName.IsEmpty() && m.name.IsEmpty() && m.type.IsEmpty()) continue;
        if (m.name.IsEmpty() || m.type.IsEmpty()) {
            err = wxString::Format(tr(L"第 %d 行:字段名和类型不能为空"), r + 1);
            return false;
        }

        if (m.origName.IsEmpty()) {                 // freshly added row → ADD
            db::ColumnEdit ce; ce.op = db::ColumnEdit::Add; ce.model = m;
            edit.columns.push_back(std::move(ce));
            continue;
        }

        seen.insert(m.origName);

        // Modify when content differs from the load-time baseline (rename/retype/…) OR
        // the column was reordered (上移/下移). A reorder carries a FIRST/AFTER clause.
        const FieldRow* fr = rows_[r].get();
        const auto rit = reorder.find(m.origName);
        const bool moved = (rit != reorder.end());
        if (!fr->hasBaseline || !ModelsEqual(m, fr->baseline) || moved) {
            db::ColumnEdit ce; ce.op = db::ColumnEdit::Modify; ce.model = m;
            if (moved) { ce.positioned = true; ce.afterColumn = rit->second; }
            edit.columns.push_back(std::move(ce));
        }
    }

    // dropped columns: present originally, absent from the form now
    for (const auto& c : origCols_) {
        if (seen.find(c.name) != seen.end()) continue;
        db::ColumnEdit ce; ce.op = db::ColumnEdit::Drop;
        ce.model.name = c.name;
        edit.columns.push_back(std::move(ce));
    }

    if (edit.columns.empty()) {
        err = tr(L"没有检测到任何结构变更");
        return false;
    }
    return true;
}

wxString TableDesignView::ScriptedAddColumn(const wxString& name,
                                            const wxString& type,
                                            const wxString& length)
{
    if (!conn_ || !conn_->IsConnected() || table_.IsEmpty() || !profile_)
        return tr(L"ERR: 未连接或未选表");

    formScroll_->Freeze();
    FieldRow* fr = AddRowControls();
    fr->name->ChangeValue(name);
    fr->type->ChangeValue(type);
    fr->length->ChangeValue(length);
    fr->origName.clear();
    fr->hasBaseline = false;
    ApplyRowTypeEditability(static_cast<int>(rows_.size()) - 1);
    formSizer_->Layout();
    formScroll_->FitInside();
    formScroll_->Thaw();

    wxString err;
    db::TableEdit edit;
    if (!BuildTableEdit(edit, err)) return L"ERR: " + err;

    std::vector<wxString> stmts;
    if (!profile_->RenderAlter(edit, stmts, err)) return L"ERR: " + err;

    wxString joined;
    for (const wxString& s : stmts) {
        db::QueryResult res;
        if (!conn_->Execute(s, res, err))
            return L"ERR: " + err + L"\n-- DDL:\n" + s;
        joined += s + L";\n";
    }

    Load(conn_, db_, table_, dbType_);   // reload from server
    return joined;
}

void TableDesignView::OnSaveChanges() { SaveChanges(); }

// Preview + execute the ALTER. Returns true when the change was applied (so the caller
// — e.g. 保存并关闭 — may proceed), false when the user cancelled the preview or a step
// failed / had nothing to run (the tab should stay open in those cases).
bool TableDesignView::SaveChanges()
{
    FlushAttrPanel();   // capture any pending panel edits on the current row

    wxString err;
    db::TableEdit edit;
    if (!BuildTableEdit(edit, err)) {
        wxMessageBox(err, tr(L"保存修改"), wxOK | wxICON_INFORMATION, this);
        return false;
    }
    return ExecuteEdit(edit);
}

// Shared save primitive for every tab: render the TableEdit → preview → confirm →
// execute each statement → reload. Mutates real schema, so the preview-confirm is
// mandatory. Returns true only when fully applied.
bool TableDesignView::ExecuteEdit(db::TableEdit& edit)
{
    if (!conn_ || !profile_) return false;

    wxString err;
    std::vector<wxString> stmts;
    if (!profile_->RenderAlter(edit, stmts, err) || stmts.empty()) {
        wxMessageBox(err.IsEmpty() ? tr(L"没有可执行的语句") : err,
                     tr(L"保存修改"), wxOK | wxICON_INFORMATION, this);
        return false;
    }

    // The full DDL is shown in the ⑥SQL 预览 tab — keep the confirm prompt terse
    // (no script dump) per the design request.
    if (wxMessageBox(tr(L"确认将当前修改保存到数据库?"), tr(L"确认保存"),
                     wxYES_NO | wxICON_WARNING, this) != wxYES)
        return false;

    for (const wxString& s : stmts) {
        db::QueryResult res;
        if (!conn_->Execute(s, res, err)) {
            wxMessageBox(tr(L"执行失败:\n\n") + err + tr(L"\n\n-- 语句:\n") + s,
                         tr(L"结构变更"), wxOK | wxICON_ERROR, this);
            Load(conn_, db_, table_, dbType_);   // resync to whatever committed
            return false;
        }
    }
    wxMessageBox(tr(L"✓ 结构已更新"), tr(L"结构变更"), wxOK | wxICON_INFORMATION, this);
    Load(conn_, db_, table_, dbType_);   // reload from server to reflect the new truth
    return true;
}

// 注释 tab save: one ALTER … COMMENT (MySQL) / COMMENT ON TABLE (PG/Oracle);
// SQLite/SQL Server render nothing (RenderAlter no-op) → honest "无可执行语句".
void TableDesignView::SaveComment()
{
    if (!conn_ || !profile_ || table_.IsEmpty()) return;
    db::TableEdit edit;
    edit.db = db_;
    edit.table = table_;
    edit.hasComment = true;
    edit.comment = commentEdit_ ? commentEdit_->GetValue() : wxString();
    ExecuteEdit(edit);
}

// Assemble the full current table structure from the field form (+ PK + comment) for
// RenderCreate. Indexes/FKs/triggers/options are folded in once those tabs land (M3+).
void TableDesignView::BuildTableModel(db::TableModel& model) const
{
    model.db = db_;
    model.table = table_;
    for (int r = 0; r < static_cast<int>(rows_.size()); ++r) {
        db::ColumnModel m = RowToModel(r);
        if (m.name.IsEmpty() && m.type.IsEmpty()) continue;   // skip blank rows
        model.columns.push_back(std::move(m));
    }
    for (const FieldRow* fr : pkOrder_) {
        const int r = RowIndexOf(fr);
        if (r < 0) continue;
        const wxString name = RowToModel(r).name;
        if (!name.IsEmpty()) model.primaryKey.push_back(name);
    }
    for (int r = 0; r < static_cast<int>(idxRows_.size()); ++r) {
        db::IndexModel m = IndexRowToModel(r);
        if (m.name.IsEmpty() || m.columns.empty()) continue;
        model.indexes.push_back(std::move(m));
    }
    for (int r = 0; r < static_cast<int>(fkRows_.size()); ++r) {
        db::ForeignKeyModel m = FkRowToModel(r);
        if (m.columns.empty() || m.refTable.IsEmpty() || m.refColumns.empty()) continue;
        model.fks.push_back(std::move(m));
    }
    AppendTriggerModels(model);   // 触发器 tab → model.triggers (helper in _Trigger.cpp)
    AppendOptionsModel(model);    // 选项 tab   → model.options  (helper in _Options.cpp)
    if (commentEdit_) model.comment = commentEdit_->GetValue();
}

// Regenerate the two read-only code panes from the current edit state. ⑥SQL 预览 =
// the pending incremental ALTER (RenderAlter over the field diff); ⑦TABLE DDL = the
// full CREATE (RenderCreate over the whole current structure). Called on tab switch.
void TableDesignView::RefreshGeneratedTabs()
{
    if (!conn_ || !profile_ || table_.IsEmpty()) return;
    const_cast<TableDesignView*>(this)->FlushAttrPanel();

    if (sqlPreview_) {
        // Show EVERY pending operation, and render each category on its OWN so the
        // preview reads as distinct statements (列改一条 / 加索引一条 / …) instead of
        // MySQL folding them into a single comma-joined ALTER where a field change
        // looks lost behind the index one.
        db::TableEdit full; BuildFullEdit(full);
        wxString text, err;
        db::TableEdit p; p.db = full.db; p.table = full.table;
        auto emit = [&] {
            std::vector<wxString> stmts;
            if (profile_->RenderAlter(p, stmts, err))
                for (const wxString& s : stmts) text += s + L";\n";
        };
        if (!full.columns.empty())  { p.columns  = std::move(full.columns);  emit(); p.columns.clear(); }
        if (!full.indexes.empty())  { p.indexes  = std::move(full.indexes);  emit(); p.indexes.clear(); }
        if (!full.fks.empty())      { p.fks       = std::move(full.fks);      emit(); p.fks.clear(); }
        if (!full.triggers.empty()) { p.triggers = std::move(full.triggers); emit(); p.triggers.clear(); }
        if (full.hasOptions) { p.hasOptions = true;  p.options = full.options; emit(); p.hasOptions = false; }
        if (full.hasComment) { p.hasComment = true;  p.comment = full.comment; emit(); p.hasComment = false; }
        SetCodePane(sqlPreview_, text.IsEmpty() ? tr(L"-- 无待执行的结构变更") : text);
    }

    if (ddl_) {
        db::TableModel model;
        BuildTableModel(model);
        std::vector<wxString> stmts;
        wxString text, err;
        if (profile_->RenderCreate(model, stmts, err))
            for (const wxString& s : stmts) text += s + L";\n";
        SetCodePane(ddl_, text);
    }
}

// True when the edit form differs from the loaded original snapshot (a column was
// added / dropped / renamed / retyped / null-default-comment-or-attr changed).
bool TableDesignView::HasUnsavedChanges() const
{
    if (!formScroll_) return false;
    const_cast<TableDesignView*>(this)->FlushAttrPanel();  // fold in pending panel edits
    // Unsaved iff ANY tab has a pending edit — reuse BuildFullEdit (the same aggregate
    // that feeds ⑥SQL 预览) so close covers columns/index/FK/trigger/comment/options.
    // Switching inner tabs no longer prompts; only the close flow uses this.
    db::TableEdit e;
    BuildFullEdit(e);
    return !e.columns.empty() || !e.indexes.empty() || !e.fks.empty() ||
           !e.triggers.empty() || e.hasComment || e.hasOptions;
}

bool TableDesignView::ConfirmClose()
{
    if (!HasUnsavedChanges()) return true;
    return wxMessageBox(tr(L"放弃未保存的结构改动?"), tr(L"关闭表设计"),
                        wxYES_NO | wxICON_WARNING, this) == wxYES;
}

} // namespace ui
