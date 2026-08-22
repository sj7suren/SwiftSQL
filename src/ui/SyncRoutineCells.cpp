// SyncRoutineCells.cpp — see header.
#include "ui/SyncRoutineCells.h"

#include "ui/I18n.h"
#include "ui/SyncDiffModel.h"
#include "ui/Theme.h"

namespace ui {

namespace {

// A zero-width space in the check cell's TEXT is the "no checkbox" signal. It
// renders as nothing if it ever reaches a cell we do draw, so getting this
// wrong degrades to invisible rather than to a stray glyph.
const wxString kNoCheck = wxString(wchar_t(0x200B));

} // namespace

wxString OpBadge(DiffOp op)
{
    switch (op) {
    case DiffOp::Add:    return L"＋";
    case DiffOp::Modify: return L"≠";
    case DiffOp::Drop:   return L"－";
    }
    return wxString();
}

wxColour ColourForOp(DiffOp op)
{
    switch (op) {
    case DiffOp::Add:    return theme::kDiffAddedFg;
    case DiffOp::Modify: return theme::kDiffModFg;
    case DiffOp::Drop:   return theme::kDiffDelFg;
    }
    return theme::kTextBody;
}

wxVariant NoCheckCell()
{
    wxVariant v;
    v << wxDataViewCheckIconText(kNoCheck, wxBitmapBundle(), wxCHK_UNDETERMINED);
    return v;
}

// ===========================================================================
// NoBoxCheckRenderer
// ===========================================================================

NoBoxCheckRenderer::NoBoxCheckRenderer()
    : wxDataViewCheckIconTextRenderer(wxDATAVIEW_CELL_ACTIVATABLE)
{
}

bool NoBoxCheckRenderer::SetValue(const wxVariant& value)
{
    wxDataViewCheckIconText v;
    v << value;
    suppress_ = (v.GetText() == kNoCheck);
    if (!suppress_) return wxDataViewCheckIconTextRenderer::SetValue(value);

    // Hand the base an empty cell anyway so its own state stays coherent for
    // anything that queries it while we are not drawing.
    wxVariant blank;
    blank << wxDataViewCheckIconText(wxString(), wxBitmapBundle(), wxCHK_UNDETERMINED);
    return wxDataViewCheckIconTextRenderer::SetValue(blank);
}

bool NoBoxCheckRenderer::Render(wxRect cell, wxDC* dc, int state)
{
    if (suppress_) return true;     // an empty cell, not a box
    return wxDataViewCheckIconTextRenderer::Render(cell, dc, state);
}

bool NoBoxCheckRenderer::ActivateCell(const wxRect& cell, wxDataViewModel* model,
                                      const wxDataViewItem& item, unsigned int col,
                                      const wxMouseEvent* mouseEvent)
{
    // Belt and braces: the model's IsEnabled() already returns false for every
    // node kind that gets a suppressed cell. This makes "no checkbox" true of
    // the INTERACTION as well as of the pixels, independently of that.
    if (suppress_) return false;
    return wxDataViewCheckIconTextRenderer::ActivateCell(cell, model, item, col, mouseEvent);
}

// ===========================================================================
// Cells
// ===========================================================================

void RoutineCategoryCell(wxVariant& v, const DiffNode& n, unsigned int col)
{
    switch (col) {
    case kColCheck:
    case kColDelete: v = NoCheckCell();  return;
    case kColName:   v = n.label;        return;
    // 「仅对比」 in the 类型 column is the whole boundary said in three
    // characters, in the column the user is already reading to learn what a row
    // IS. Every table row's 类型 says what will be done to it; this one says
    // that nothing will be.
    case kColType:   v = tr(L"仅对比");   return;
    case kColStruct: v = n.summary;      return;   // 共 N 项 · 差异 M 项
    case kColStatus: v = n.status;       return;   // 不可见（…） when degraded
    default:         v = wxString();     return;
    }
}

void RoutineCell(wxVariant& v, const DiffNode& n, unsigned int col)
{
    switch (col) {
    case kColCheck:
    case kColDelete: v = NoCheckCell();  return;
    case kColName:   v = OpBadge(n.op) + L" " + n.label; return;
    case kColType:   v = tr(L"例程");     return;
    case kColStruct: v = n.summary;      return;   // the data layer's reason text
    case kColStatus: v = n.status;       return;   // 仅源端存在 / 无法比较 / …
    default:         v = wxString();     return;
    }
}

bool RoutineAttr(const DiffNode& n, unsigned int col, wxDataViewItemAttr& attr)
{
    if (n.kind == DiffNodeKind::Category) {
        if (col == kColName) { attr.SetBold(true); return true; }
        // A 不可见 status IS a problem the user can act on (fix the account's
        // privileges), so it gets the warning colour.
        if (col == kColStatus && !n.status.IsEmpty()) {
            attr.SetColour(theme::kDotAmber);
            return true;
        }
        if (col == kColType || col == kColStruct) {
            attr.SetColour(theme::kTextSecondary);
            return true;
        }
        return false;
    }

    if (n.kind == DiffNodeKind::Routine) {
        if (col == kColName) { attr.SetColour(ColourForOp(n.op)); return true; }
        // 无法比较 is MUTED, not amber. It is a declined claim, not a defect:
        // cross-engine procedural code is shown for a human to read, and there
        // is nothing wrong for the user to fix.
        if (col == kColStatus && n.verdict == db::sync::RoutineVerdict::NotComparable) {
            attr.SetColour(theme::kTextMuted);
            return true;
        }
        return false;
    }

    return false;
}

} // namespace ui
