// TableImport.cpp — the four P0 parsers behind ImportTable.
//
// CSV/TXT is a byte-faithful port of ResultGridPanelOps.cpp::ImportCsv (RFC-4180:
// quoted fields, "" escape, embedded delimiters/newlines) generalized to a
// configurable delimiter and extended to (a) track source line numbers and (b)
// flag records rather than silently swallow malformed ones.
//
// JSON/XML are targeted recursive/scanning parsers for the exact shapes
// TableExport emits (an array of flat objects; <rows><row><field name=…>…), with
// enough tolerance to survive hand-edited files. Columns are the union of keys /
// field names in first-seen order; a value equal to JSON null / XML null="true"
// maps back to opt.nullText. Structural breakage is reported (fatal for JSON/XML,
// per-record for CSV) — never a crash.
#include "db/TableImport.h"

namespace db {

namespace {

long long LineAt(const wxString& s, size_t pos)
{
    long long line = 1;
    const size_t end = (pos <= s.length()) ? pos : s.length();
    for (size_t i = 0; i < end; ++i)
        if (s[i] == L'\n') ++line;
    return line;
}

// ---- CSV / TXT -------------------------------------------------------------
struct DRecord {
    long long             line = 1;      // 1-based source line where the record starts
    std::vector<wxString> fields;
    bool                  unterminated = false;   // quote left open at EOF
};

// Parse delimited text record-by-record, invoking `emit(DRecord&&)` as each record
// completes. Streaming (no full std::vector<DRecord>) so a large import never
// materializes every parsed record on top of the source text AND the caller's row
// sink — that was one of three simultaneous full copies of the dataset (peak memory
// ~3×). `emit` returns false to stop parsing early (e.g. the row sink was cancelled).
template <class Emit>
void ParseDelimitedStream(const wxString& text, wxChar delim, Emit&& emit)
{
    std::vector<wxString> cur;
    wxString field;
    bool inQ = false, any = false, recStarted = false;
    long long line = 1, recLine = 1;
    const size_t n = text.length();

    auto touch = [&]() { if (!recStarted) { recLine = line; recStarted = true; } };

    for (size_t i = 0; i < n; ++i) {
        const wxChar ch = text[i];
        if (inQ) {
            if (ch == L'"') {
                if (i + 1 < n && text[i + 1] == L'"') { field += L'"'; ++i; }
                else inQ = false;
            } else {
                if (ch == L'\n') ++line;
                field += ch;
            }
        } else if (ch == L'"') { inQ = true; any = true; touch(); }
        else if (ch == delim)  { cur.push_back(field); field.clear(); any = true; touch(); }
        else if (ch == L'\r')  { /* swallow; handled with \n */ }
        else if (ch == L'\n')  {
            cur.push_back(field); field.clear();
            DRecord r; r.line = recStarted ? recLine : line; r.fields = std::move(cur);
            cur.clear(); any = false; recStarted = false;
            ++line;
            if (!emit(std::move(r))) return;   // sink cancelled → stop, don't keep parsing
        } else { field += ch; any = true; touch(); }
    }
    if (any || !cur.empty() || inQ) {
        cur.push_back(field);
        DRecord r; r.line = recStarted ? recLine : line; r.fields = std::move(cur);
        r.unterminated = inQ;
        emit(std::move(r));
    }
}

ImportResult ImportDelimited(const wxString& content, const ImportOptions& opt,
                             const ImportRowSink& rowSink,
                             const ImportErrorSink& errorSink)
{
    ImportResult res;
    wxChar delim = L',';
    if (opt.format == ImportFormat::Txt)
        delim = opt.delimiter.IsEmpty() ? L'\t' : opt.delimiter[0];

    // Stream records straight from the parser into the row sink. `headerPending`
    // consumes the first record as the header (identical to the old "start=1" skip,
    // and only when a record actually exists — an empty file leaves columns empty).
    int  expectedWidth = -1;
    bool headerPending = opt.hasHeader;

    ParseDelimitedStream(content, delim, [&](DRecord&& rec) -> bool {
        if (headerPending) {
            headerPending = false;
            res.columns   = rec.fields;
            expectedWidth = static_cast<int>(res.columns.size());
            return true;   // header consumed; not a data row
        }
        if (expectedWidth < 0) expectedWidth = static_cast<int>(rec.fields.size());

        if (rec.unterminated && errorSink) {
            ImportError e; e.lineNo = rec.line; e.message = L"未闭合的引号（记录到文件末尾仍未结束）";
            for (size_t k = 0; k < rec.fields.size(); ++k) { if (k) e.raw += delim; e.raw += rec.fields[k]; }
            errorSink(e); ++res.errorCount;
        } else if (static_cast<int>(rec.fields.size()) != expectedWidth && errorSink) {
            ImportError e; e.lineNo = rec.line;
            e.message = wxString::Format(L"列数不匹配（期望 %d，实际 %d）",
                                         expectedWidth, static_cast<int>(rec.fields.size()));
            for (size_t k = 0; k < rec.fields.size(); ++k) { if (k) e.raw += delim; e.raw += rec.fields[k]; }
            errorSink(e); ++res.errorCount;
        }

        ++res.rowCount;
        return !(rowSink && !rowSink(rec.fields));   // false → sink cancelled, stop parsing
    });
    return res;
}

// ---- JSON ------------------------------------------------------------------
struct JField { wxString key; wxString value; bool isNull = false; };

struct JsonParser {
    const wxString& s;
    size_t i = 0;
    const size_t n;
    wxString err;
    size_t errPos = 0;

    explicit JsonParser(const wxString& src) : s(src), n(src.length()) {}

    void skipWs() { while (i < n) { wxChar c = s[i]; if (c==L' '||c==L'\t'||c==L'\n'||c==L'\r') ++i; else break; } }

    bool fail(const wxString& m) { if (err.IsEmpty()) { err = m; errPos = i; } return false; }

    bool parseString(wxString& out) {
        if (i >= n || s[i] != L'"') return fail(L"期望字符串");
        ++i;
        while (i < n) {
            wxChar c = s[i++];
            if (c == L'"') return true;
            if (c == L'\\') {
                if (i >= n) return fail(L"字符串转义未结束");
                wxChar e = s[i++];
                switch (e) {
                case L'"':  out += L'"';  break;
                case L'\\': out += L'\\'; break;
                case L'/':  out += L'/';  break;
                case L'b':  out += L'\b'; break;
                case L'f':  out += L'\f'; break;
                case L'n':  out += L'\n'; break;
                case L'r':  out += L'\r'; break;
                case L't':  out += L'\t'; break;
                case L'u': {
                    if (i + 4 > n) return fail(L"\\u 转义位数不足");
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        wxChar h = s[i++]; unsigned d;
                        if (h >= L'0' && h <= L'9') d = static_cast<unsigned>(h - L'0');
                        else if (h >= L'a' && h <= L'f') d = static_cast<unsigned>(10 + h - L'a');
                        else if (h >= L'A' && h <= L'F') d = static_cast<unsigned>(10 + h - L'A');
                        else return fail(L"\\u 非法十六进制");
                        code = code * 16 + d;
                    }
                    out += wxUniChar(code);
                    break;
                }
                default: return fail(L"未知的字符串转义");
                }
            } else {
                out += c;
            }
        }
        return fail(L"字符串未闭合");
    }

    // Parse any value into text; sets isNull for a literal null. Nested
    // object/array are captured as their raw source text.
    bool parseValue(wxString& out, bool& isNull) {
        isNull = false;
        skipWs();
        if (i >= n) return fail(L"意外的结尾");
        wxChar c = s[i];
        if (c == L'"') return parseString(out);
        if (c == L'{' || c == L'[') return captureRaw(out);
        if (c == L'n') { if (match(L"null")) { isNull = true; return true; } return fail(L"非法字面量"); }
        if (c == L't') { if (match(L"true"))  { out = L"true";  return true; } return fail(L"非法字面量"); }
        if (c == L'f') { if (match(L"false")) { out = L"false"; return true; } return fail(L"非法字面量"); }
        // number
        size_t st = i;
        while (i < n) { wxChar d = s[i];
            if ((d>=L'0'&&d<=L'9')||d==L'-'||d==L'+'||d==L'.'||d==L'e'||d==L'E') ++i; else break; }
        if (i == st) return fail(L"非法的值");
        out = s.SubString(st, i - 1);
        return true;
    }

    bool match(const wxString& lit) {
        if (i + lit.length() > n) return false;
        for (size_t k = 0; k < lit.length(); ++k) if (s[i + k] != lit[k]) return false;
        i += lit.length();
        return true;
    }

    // Capture a balanced {…} / […] (string-aware) as raw text.
    bool captureRaw(wxString& out) {
        size_t st = i;
        int depth = 0; bool inStr = false;
        while (i < n) {
            wxChar c = s[i++];
            if (inStr) {
                if (c == L'\\') { if (i < n) ++i; }
                else if (c == L'"') inStr = false;
            } else {
                if (c == L'"') inStr = true;
                else if (c == L'{' || c == L'[') ++depth;
                else if (c == L'}' || c == L']') { if (--depth == 0) { out = s.SubString(st, i - 1); return true; } }
            }
        }
        return fail(L"对象/数组未闭合");
    }

    bool parseObject(std::vector<JField>& fields) {
        skipWs();
        if (i >= n || s[i] != L'{') return fail(L"期望对象 '{'");
        ++i; skipWs();
        if (i < n && s[i] == L'}') { ++i; return true; }
        while (true) {
            skipWs();
            JField f;
            if (!parseString(f.key)) return false;
            skipWs();
            if (i >= n || s[i] != L':') return fail(L"期望 ':'");
            ++i;
            if (!parseValue(f.value, f.isNull)) return false;
            fields.push_back(std::move(f));
            skipWs();
            if (i >= n) return fail(L"对象未闭合");
            if (s[i] == L',') { ++i; continue; }
            if (s[i] == L'}') { ++i; return true; }
            return fail(L"期望 ',' 或 '}'");
        }
    }
};

ImportResult ImportJson(const wxString& content, const ImportOptions& opt,
                        const ImportRowSink& rowSink,
                        const ImportErrorSink& errorSink)
{
    ImportResult res;
    JsonParser p(content);
    p.skipWs();
    if (p.i >= p.n || content[p.i] != L'[') {
        res.ok = false; res.fatal = L"JSON 顶层必须是数组 '[...]'"; return res;
    }
    ++p.i; p.skipWs();

    std::vector<wxString> columns;
    auto colIndex = [&](const wxString& name) -> size_t {
        for (size_t k = 0; k < columns.size(); ++k) if (columns[k] == name) return k;
        columns.push_back(name); return columns.size() - 1;
    };
    // (colIndex, value) sparse rows, resolved to full width after column union.
    std::vector<std::vector<std::pair<size_t, wxString>>> sparse;

    if (!(p.i < p.n && content[p.i] == L']')) {
        while (true) {
            std::vector<JField> fields;
            if (!p.parseObject(fields)) {
                res.ok = false;
                res.fatal = wxString::Format(L"第 %ld 行: %s",
                                             static_cast<long>(LineAt(content, p.errPos)), p.err);
                return res;
            }
            std::vector<std::pair<size_t, wxString>> row;
            for (const JField& f : fields)
                row.emplace_back(colIndex(f.key), f.isNull ? opt.nullText : f.value);
            sparse.push_back(std::move(row));
            p.skipWs();
            if (p.i >= p.n) { res.ok = false; res.fatal = L"数组未闭合"; return res; }
            if (content[p.i] == L',') { ++p.i; continue; }
            if (content[p.i] == L']') { ++p.i; break; }
            res.ok = false;
            res.fatal = wxString::Format(L"第 %ld 行: 期望 ',' 或 ']'", static_cast<long>(LineAt(content, p.i)));
            return res;
        }
    }

    res.columns = columns;
    for (const auto& sr : sparse) {
        std::vector<wxString> full(columns.size(), opt.nullText);  // missing key → null
        for (const auto& kv : sr) full[kv.first] = kv.second;
        ++res.rowCount;
        if (rowSink && !rowSink(full)) break;
    }
    (void)errorSink;
    return res;
}

// ---- XML -------------------------------------------------------------------
wxString XmlUnescape(const wxString& in)
{
    wxString out;
    const size_t n = in.length();
    for (size_t i = 0; i < n; ++i) {
        if (in[i] != L'&') { out += in[i]; continue; }
        size_t semi = in.find(L';', i + 1);
        if (semi == wxString::npos || semi - i > 12) { out += in[i]; continue; }
        wxString ent = in.SubString(i + 1, semi - 1);
        if      (ent == L"amp")  out += L'&';
        else if (ent == L"lt")   out += L'<';
        else if (ent == L"gt")   out += L'>';
        else if (ent == L"quot") out += L'"';
        else if (ent == L"apos") out += L'\'';
        else if (!ent.IsEmpty() && ent[0] == L'#') {
            unsigned long code = 0;
            wxString num = ent.Mid(1);
            bool okNum = (num[0] == L'x' || num[0] == L'X')
                       ? num.Mid(1).ToULong(&code, 16) : num.ToULong(&code, 10);
            if (okNum && code != 0) out += wxUniChar(static_cast<int>(code));
            else { out += in.SubString(i, semi); }
        } else { out += in.SubString(i, semi); i = semi; continue; }
        i = semi;
    }
    return out;
}

// Pull name="value" pairs out of a start-tag's body (name + attrs, no '<'/'>').
wxString XmlAttr(const wxString& tagBody, const wxString& attr)
{
    wxString needle = attr + L"=";
    int pos = tagBody.Find(needle);
    if (pos == wxNOT_FOUND) return wxEmptyString;
    size_t p = static_cast<size_t>(pos) + needle.length();
    if (p >= tagBody.length()) return wxEmptyString;
    wxChar quote = tagBody[p];
    if (quote != L'"' && quote != L'\'') return wxEmptyString;
    size_t end = tagBody.find(quote, p + 1);
    if (end == wxString::npos) return wxEmptyString;
    return XmlUnescape(tagBody.SubString(p + 1, end - 1));
}

ImportResult ImportXml(const wxString& content, const ImportOptions& opt,
                       const ImportRowSink& rowSink,
                       const ImportErrorSink& errorSink)
{
    ImportResult res;
    const wxString& s = content;
    const size_t n = s.length();
    size_t i = 0;
    long long line = 1;

    std::vector<wxString> columns;
    auto colIndex = [&](const wxString& name) -> size_t {
        for (size_t k = 0; k < columns.size(); ++k) if (columns[k] == name) return k;
        columns.push_back(name); return columns.size() - 1;
    };
    std::vector<std::vector<std::pair<size_t, wxString>>> sparse;
    std::vector<std::pair<size_t, wxString>> cur;
    bool inRow = false;

    auto advanceTo = [&](const wxString& target) -> size_t {
        size_t pos = s.find(target, i);
        size_t stop = (pos == wxString::npos) ? n : pos;
        for (size_t k = i; k < stop; ++k) if (s[k] == L'\n') ++line;
        return pos;
    };

    while (i < n) {
        if (s[i] != L'<') { if (s[i] == L'\n') ++line; ++i; continue; }
        // skip declaration / comments
        if (i + 1 < n && s[i + 1] == L'?') {
            size_t e = advanceTo(L"?>"); if (e == wxString::npos) break; i = e + 2; continue;
        }
        if (i + 3 < n && s[i + 1] == L'!' && s[i + 2] == L'-' && s[i + 3] == L'-') {
            size_t e = advanceTo(L"-->"); if (e == wxString::npos) break; i = e + 3; continue;
        }
        // find end of this tag
        size_t gt = s.find(L'>', i);
        if (gt == wxString::npos) break;
        for (size_t k = i; k < gt; ++k) if (s[k] == L'\n') ++line;
        wxString tag = s.SubString(i + 1, gt - 1);   // between < and >
        i = gt + 1;

        bool closing = (!tag.IsEmpty() && tag[0] == L'/');
        bool selfClose = (!tag.IsEmpty() && tag.Last() == L'/');
        if (closing) tag = tag.Mid(1);
        if (selfClose) tag.RemoveLast();
        tag.Trim(true).Trim(false);
        wxString name = tag.BeforeFirst(L' ');
        wxString body = tag.AfterFirst(L' ');   // attributes ("" if none)

        if (name == L"row") {
            if (closing) {
                if (inRow) { sparse.push_back(std::move(cur)); cur.clear(); inRow = false; }
            } else {
                inRow = true; cur.clear();
                if (selfClose) { sparse.push_back(std::move(cur)); cur.clear(); inRow = false; }
            }
        } else if (name == L"field" && !closing) {
            wxString colName = XmlAttr(body, L"name");
            bool isNull = (XmlAttr(body, L"null") == L"true");
            wxString value;
            if (!selfClose) {
                size_t close = advanceTo(L"</field>");
                if (close == wxString::npos) {
                    if (errorSink) { ImportError e; e.lineNo = line;
                        e.message = L"<field> 未闭合"; e.raw = colName; errorSink(e); ++res.errorCount; }
                    break;
                }
                value = XmlUnescape(s.SubString(i, close - 1));
                i = close + 8;   // past "</field>"
            }
            if (inRow)
                cur.emplace_back(colIndex(colName), isNull ? opt.nullText : value);
        }
        // ignore <rows>, </rows>, </field>, and any unknown elements
    }

    res.columns = columns;
    for (const auto& sr : sparse) {
        std::vector<wxString> full(columns.size(), opt.nullText);
        for (const auto& kv : sr) full[kv.first] = kv.second;
        ++res.rowCount;
        if (rowSink && !rowSink(full)) break;
    }
    return res;
}

} // namespace

// ---------------------------------------------------------------------------
ImportFormat DetectFormatByExtension(const wxString& path)
{
    wxString ext = path.AfterLast(L'.').Lower();
    if (ext == L"csv")  return ImportFormat::Csv;
    if (ext == L"txt")  return ImportFormat::Txt;
    if (ext == L"json") return ImportFormat::Json;
    if (ext == L"xml")  return ImportFormat::Xml;
    return ImportFormat::Unknown;   // .sql handled by RunScriptDialog, not here
}

ImportResult ImportTable(const wxString& content, const ImportOptions& opt,
                         const ImportRowSink& rowSink,
                         const ImportErrorSink& errorSink)
{
    switch (opt.format) {
    case ImportFormat::Csv:
    case ImportFormat::Txt:  return ImportDelimited(content, opt, rowSink, errorSink);
    case ImportFormat::Json: return ImportJson(content, opt, rowSink, errorSink);
    case ImportFormat::Xml:  return ImportXml(content, opt, rowSink, errorSink);
    case ImportFormat::Unknown:
    default: {
        ImportResult r; r.ok = false; r.fatal = L"不支持的导入格式"; return r;
    }
    }
}

} // namespace db
