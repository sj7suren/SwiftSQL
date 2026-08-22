// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// CellViewerPanel.cpp — see header. A read-only wxStyledTextCtrl that renders a
// single grid cell's value as plain text, a hex dump, pretty-printed JSON/XML,
// coloured HTML source, or (v1) a short image-detection note.
#include "ui/CellViewerPanel.h"

#include <wx/sizer.h>
#include <wx/choice.h>
#include <wx/arrstr.h>
#include <wx/base64.h>
#include <wx/buffer.h>
#include <wx/stc/stc.h>

#include <vector>

#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/UiFonts.h"

namespace ui {
namespace {

// ---------------------------------------------------------------------------
// JSON — lightweight balance check + string-aware pretty printer
// ---------------------------------------------------------------------------
// Cheap "is this JSON worth formatting" test: the trimmed text must start with
// { or [, all braces/brackets must balance, and no string may be left open. We
// deliberately don't validate grammar fully — just enough to avoid mangling a
// non-JSON cell.
bool JsonBalanced(const wxString& s)
{
    wxString t = s; t.Trim(true).Trim(false);
    if (t.IsEmpty()) return false;
    const wxChar first = t[0];
    if (first != '{' && first != '[') return false;

    int depth = 0;
    bool inStr = false, esc = false;
    for (wxUniChar c : t) {
        if (inStr) {
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  inStr = false;
            continue;
        }
        if (c == '"')                       inStr = true;
        else if (c == '{' || c == '[')      ++depth;
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return depth == 0 && !inStr;
}

// Re-indent with 2 spaces per level: newline after { [ and each top-level comma,
// a single space after ':'. String literals (with escaped quotes) are copied
// verbatim so their inner whitespace is never touched.
wxString PrettyJson(const wxString& src)
{
    wxString out;
    int indent = 0;
    bool inStr = false, esc = false;
    const size_t n = src.length();

    auto newline = [&](int lvl) {
        out += '\n';
        for (int i = 0; i < lvl; ++i) out += L"  ";
    };

    size_t i = 0;
    while (i < n) {
        const wxChar c = src[i];
        if (inStr) {
            out += c;
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  inStr = false;
            ++i; continue;
        }
        switch (c) {
        case '"':
            inStr = true; out += c; ++i;
            break;
        case '{':
        case '[': {
            // peek the next non-space char to keep empty containers on one line
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t' ||
                             src[j] == '\n' || src[j] == '\r')) ++j;
            const wxChar close = (c == '{') ? wxChar('}') : wxChar(']');
            out += c;
            if (j < n && src[j] == close) { out += close; i = j + 1; }
            else                          { ++indent; newline(indent); ++i; }
            break;
        }
        case '}':
        case ']':
            if (--indent < 0) indent = 0;
            newline(indent); out += c; ++i;
            break;
        case ',':
            out += c; newline(indent); ++i;
            break;
        case ':':
            out += L": "; ++i;
            break;
        case ' ': case '\t': case '\n': case '\r':
            ++i;    // collapse pre-existing whitespace between tokens
            break;
        default:
            out += c; ++i;
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// XML — tag-aware re-indenter
// ---------------------------------------------------------------------------
bool XmlLooksValid(const wxString& s)
{
    wxString t = s; t.Trim(true).Trim(false);
    return !t.IsEmpty() && t[0] == '<' && t.Find('>') != wxNOT_FOUND;
}

// Walk the source token by token: opening tags bump the indent, closing tags
// drop it, self-closing tags / declarations stay level, and text content lands
// on its own indented line. We treat the raw tag text as a unit (the lexer does
// the colouring), so malformed markup degrades gracefully rather than crashing.
wxString PrettyXml(const wxString& src)
{
    wxString out;
    int indent = 0;
    const size_t n = src.length();

    auto ind = [&](int lvl) { for (int i = 0; i < lvl; ++i) out += L"  "; };
    auto brk = [&]() { if (!out.IsEmpty() && out.Last() != '\n') out += '\n'; };

    size_t i = 0;
    while (i < n) {
        if (src[i] == '<') {
            size_t j = i;
            while (j < n && src[j] != '>') ++j;
            const wxString tag = src.Mid(i, (j < n ? j - i + 1 : n - i));
            i = (j < n) ? j + 1 : n;

            const wxString body    = tag.Mid(1);                 // drop '<'
            const bool closing     = body.StartsWith(L"/");
            const bool declaration = body.StartsWith(L"?") || body.StartsWith(L"!");
            const bool selfClose   = tag.EndsWith(L"/>");

            if (closing) {
                if (--indent < 0) indent = 0;
                brk(); ind(indent); out += tag;
            } else {
                brk(); ind(indent); out += tag;
                if (!selfClose && !declaration) ++indent;
            }
        } else {
            size_t j = i;
            while (j < n && src[j] != '<') ++j;
            wxString text = src.Mid(i, j - i);
            text.Trim(true).Trim(false);
            i = j;
            if (!text.IsEmpty()) { brk(); ind(indent); out += text; }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// HEX — classic `offset  bytes  |ascii|` dump of the UTF-8 encoding of value_
// ---------------------------------------------------------------------------
wxString HexDump(const wxString& value)
{
    const wxScopedCharBuffer utf8 = value.utf8_str();
    const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8.data());
    const size_t len = utf8.length();

    wxString out;
    for (size_t off = 0; off < len; off += 16) {
        wxString hex, asc;
        for (size_t k = 0; k < 16; ++k) {
            if (off + k < len) {
                const unsigned char b = p[off + k];
                hex += wxString::Format(L"%02X ", static_cast<unsigned>(b));
                asc += (b >= 0x20 && b < 0x7F) ? wxChar(b) : wxChar('.');
            } else {
                hex += L"   ";
                asc += L" ";
            }
            if (k == 7) hex += L" ";     // extra gap splitting the two 8-byte halves
        }
        out += wxString::Format(L"%08X  ", static_cast<unsigned>(off))
             + hex + L" " + asc + L"\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// IMAGE — best-effort magic-byte sniff (base64 or hex blob); v1 is text-only
// ---------------------------------------------------------------------------
bool MagicIsImage(const unsigned char* b, size_t n)
{
    if (n < 4) return false;
    if (b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47) return true; // PNG
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)                 return true; // JPEG
    if (b[0] == 0x47 && b[1] == 0x49 && b[2] == 0x46 && b[3] == 0x38) return true; // GIF8
    if (b[0] == 0x42 && b[1] == 0x4D)                                 return true; // BMP
    if (n >= 12 && b[0] == 0x52 && b[1] == 0x49 && b[2] == 0x46 && b[3] == 0x46 &&
                   b[8] == 0x57 && b[9] == 0x45 && b[10] == 0x42 && b[11] == 0x50) // WEBP
        return true;
    return false;
}

bool LooksLikeImageData(const wxString& value, size_t* outLen)
{
    wxString s = value; s.Trim(true).Trim(false);
    if (s.IsEmpty()) return false;

    // strip an optional data-URI wrapper (data:image/png;base64,....)
    if (s.StartsWith(L"data:")) {
        const int comma = s.Find(',');
        if (comma != wxNOT_FOUND) s = s.Mid(comma + 1);
    }

    // compact copy without whitespace (both base64 and hex tolerate stripping)
    wxString compact;
    for (wxUniChar c : s)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') compact += c;
    if (compact.IsEmpty()) return false;

    // 1) base64
    {
        const wxMemoryBuffer buf = wxBase64Decode(compact, wxBase64DecodeMode_SkipWS);
        if (buf.GetDataLen() >= 4 &&
            MagicIsImage(static_cast<const unsigned char*>(buf.GetData()),
                         buf.GetDataLen())) {
            if (outLen) *outLen = buf.GetDataLen();
            return true;
        }
    }

    // 2) hex blob (optionally 0x-prefixed)
    {
        wxString h = compact;
        if (h.StartsWith(L"0x") || h.StartsWith(L"0X")) h = h.Mid(2);
        if (h.length() >= 8 && (h.length() % 2 == 0)) {
            std::vector<unsigned char> bytes;
            bytes.reserve(h.length() / 2);
            bool ok = true;
            for (size_t k = 0; k + 1 < h.length(); k += 2) {
                long v = 0;
                if (!h.Mid(k, 2).ToLong(&v, 16)) { ok = false; break; }
                bytes.push_back(static_cast<unsigned char>(v));
            }
            if (ok && bytes.size() >= 4 && MagicIsImage(bytes.data(), bytes.size())) {
                if (outLen) *outLen = bytes.size();
                return true;
            }
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
CellViewerPanel::CellViewerPanel(wxWindow* parent, bool showSelector)
    : wxPanel(parent)
{
    SetBackgroundColour(theme::kEditorBg);
    auto* v = new wxBoxSizer(wxVERTICAL);

    // ---- render-type selector (kept as the render-type authority; hidden when the
    //      host drives the type from the toolbar, but still used via SetType) ----
    wxArrayString kinds;
    kinds.Add(tr(L"文本"));
    kinds.Add(L"HEX");
    kinds.Add(L"JSON");
    kinds.Add(L"XML");
    kinds.Add(L"WEB");
    kinds.Add(L"IMAGE");
    type_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, kinds);
    type_->SetSelection(0);
    type_->SetFont(Ui(9));
    type_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Render(); });

    auto* top = new wxBoxSizer(wxHORIZONTAL);
    top->Add(type_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, 6);
    v->Add(top, 0, wxEXPAND);
    if (!showSelector) { type_->Hide(); v->Hide(top, /*recursive*/ true); }

    // ---- read-only, syntax-coloured display ----
    view_ = new wxStyledTextCtrl(this, wxID_ANY);
    view_->SetMarginWidth(0, 0);              // no line-number / symbol / fold margins
    view_->SetMarginWidth(1, 0);
    view_->SetMarginWidth(2, 0);
    view_->SetWrapMode(wxSTC_WRAP_NONE);      // preserve hex / code alignment
    view_->SetCaretLineVisible(false);
    view_->SetScrollWidth(1);
    view_->SetScrollWidthTracking(true);
    view_->SetReadOnly(true);
    v->Add(view_, 1, wxEXPAND);

    SetSizer(v);
    Render();                                 // paint the (empty) default view
}

// ---------------------------------------------------------------------------
void CellViewerPanel::ShowValue(const wxString& value)
{
    value_ = value;
    Render();
}

void CellViewerPanel::Clear()
{
    value_.clear();
    if (!view_) return;
    view_->SetReadOnly(false);
    view_->ClearAll();
    view_->SetReadOnly(true);
}

void CellViewerPanel::SetType(int typeIndex)
{
    if (!type_) return;
    if (typeIndex < 0 || typeIndex >= static_cast<int>(type_->GetCount())) return;
    type_->SetSelection(typeIndex);
    Render();
}

int CellViewerPanel::GetType() const
{
    return type_ ? type_->GetSelection() : 0;
}

// ---------------------------------------------------------------------------
void CellViewerPanel::Render()
{
    if (!view_) return;
    view_->SetReadOnly(false);

    const int sel = type_ ? type_->GetSelection() : 0;

    // Reset every style to the mono base before the lexer-specific colours; this
    // makes each Render() self-contained regardless of the previous type.
    auto applyBase = [this](int lexer) {
        view_->SetLexer(lexer);
        const wxFont code = Mono(9);
        for (int st = 0; st < wxSTC_STYLE_MAX; ++st) {
            view_->StyleSetFont(st, code);
            view_->StyleSetBackground(st, theme::kEditorBg);
            view_->StyleSetForeground(st, theme::kSynDefault);
        }
    };

    // Shared HTML/XML colouring (WEB reuses XML's palette).
    auto applyMarkupStyles = [this]() {
        view_->StyleSetForeground(wxSTC_H_DEFAULT,          theme::kSynDefault);
        view_->StyleSetForeground(wxSTC_H_TAG,              theme::kSynKeyword); // <tag>
        view_->StyleSetForeground(wxSTC_H_TAGUNKNOWN,       theme::kSynKeyword);
        view_->StyleSetForeground(wxSTC_H_TAGEND,           theme::kSynKeyword);
        view_->StyleSetForeground(wxSTC_H_ATTRIBUTE,        theme::kSynTable);   // attr=
        view_->StyleSetForeground(wxSTC_H_ATTRIBUTEUNKNOWN, theme::kSynTable);
        view_->StyleSetForeground(wxSTC_H_DOUBLESTRING,     theme::kSynString);  // "val"
        view_->StyleSetForeground(wxSTC_H_SINGLESTRING,     theme::kSynString);  // 'val'
        view_->StyleSetForeground(wxSTC_H_VALUE,            theme::kSynString);
        view_->StyleSetForeground(wxSTC_H_NUMBER,           theme::kSynNumber);
        view_->StyleSetForeground(wxSTC_H_COMMENT,          theme::kSynComment);
        view_->StyleSetForeground(wxSTC_H_CDATA,            theme::kSynComment);
        view_->StyleSetForeground(wxSTC_H_ENTITY,           theme::kSynOperator);
        view_->StyleSetForeground(wxSTC_H_XMLSTART,         theme::kSynOperator); // <?
        view_->StyleSetForeground(wxSTC_H_XMLEND,           theme::kSynOperator); // ?>
        view_->StyleSetForeground(wxSTC_H_OTHER,            theme::kSynDefault);
    };

    switch (sel) {
    case 1: {                                       // HEX
        applyBase(wxSTC_LEX_NULL);
        view_->SetText(HexDump(value_));
        break;
    }
    case 2: {                                       // JSON
        if (JsonBalanced(value_)) {
            applyBase(wxSTC_LEX_JSON);
            view_->SetKeyWords(0, L"true false null");
            view_->StyleSetForeground(wxSTC_JSON_DEFAULT,        theme::kSynDefault);
            view_->StyleSetForeground(wxSTC_JSON_NUMBER,         theme::kSynNumber);
            view_->StyleSetForeground(wxSTC_JSON_STRING,         theme::kSynString);
            view_->StyleSetForeground(wxSTC_JSON_STRINGEOL,      theme::kSynString);
            view_->StyleSetForeground(wxSTC_JSON_ESCAPESEQUENCE, theme::kSynString);
            view_->StyleSetForeground(wxSTC_JSON_PROPERTYNAME,   theme::kSynTable); // "key":
            view_->StyleSetForeground(wxSTC_JSON_KEYWORD,        theme::kSynKeyword);// true/false/null
            view_->StyleSetBold(wxSTC_JSON_KEYWORD, true);
            view_->StyleSetForeground(wxSTC_JSON_LDKEYWORD,      theme::kSynKeyword);
            view_->StyleSetForeground(wxSTC_JSON_OPERATOR,       theme::kSynOperator);
            view_->StyleSetForeground(wxSTC_JSON_URI,            theme::kSynString);
            view_->StyleSetForeground(wxSTC_JSON_LINECOMMENT,    theme::kSynComment);
            view_->StyleSetForeground(wxSTC_JSON_BLOCKCOMMENT,   theme::kSynComment);
            view_->StyleSetForeground(wxSTC_JSON_ERROR,          theme::kSynDefault);
            view_->SetText(PrettyJson(value_));
        } else {
            applyBase(wxSTC_LEX_NULL);              // not JSON → verbatim
            view_->SetText(value_);
        }
        break;
    }
    case 3: {                                       // XML
        if (XmlLooksValid(value_)) {
            applyBase(wxSTC_LEX_XML);
            applyMarkupStyles();
            view_->SetText(PrettyXml(value_));
        } else {
            applyBase(wxSTC_LEX_NULL);              // not XML → verbatim
            view_->SetText(value_);
        }
        break;
    }
    case 4: {                                       // WEB (HTML source, coloured)
        applyBase(wxSTC_LEX_HTML);
        applyMarkupStyles();
        view_->SetText(value_);
        break;
    }
    case 5: {                                       // IMAGE (text note in v1)
        applyBase(wxSTC_LEX_NULL);
        size_t bytes = 0;
        const wxString note =
            LooksLikeImageData(value_, &bytes)
                ? tr(L"（图片预览：")
                      + wxString::Format(L"%lu", static_cast<unsigned long>(bytes))
                      + tr(L" 字节，图形预览将在后续版本支持）")
                : tr(L"（当前内容不是可识别的图片数据）");
        view_->SetText(note);
        break;
    }
    case 0:
    default: {                                      // 文本 (verbatim)
        applyBase(wxSTC_LEX_NULL);
        view_->SetText(value_);
        break;
    }
    }

    view_->SetReadOnly(true);
}

} // namespace ui
