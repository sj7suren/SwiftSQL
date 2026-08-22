// Json.cpp — implementation of the AI-layer JSON reader/writer declared in Json.h.
//
// Design notes:
//  * We parse UNTRUSTED network data (LLM responses). The parser must never throw or
//    crash: every malformed input path returns a Null value and fills `err`. There is a
//    recursion-depth cap so a pathologically nested payload cannot blow the stack.
//  * Parsing is done over the UTF-8 byte stream (text.utf8_str()). Working on bytes —
//    rather than on wxString's platform-dependent internal code units — keeps string
//    handling uniform: raw multi-byte UTF-8 passes through untouched, and \uXXXX escapes
//    (including surrogate pairs) are re-encoded to UTF-8 bytes, then the finished byte
//    buffer is handed to wxString::FromUTF8. This sidesteps UTF-16 surrogate quirks on
//    Windows entirely.
#include "ai/Json.h"

#include <cstdint>
#include <cstdlib>

namespace ai {

// Shared "absent" sentinel returned by every missing-member / type-mismatch access so
// callers can chain (json["a"]["b"].asString()) without null checks.
const JsonValue JsonValue::kNull;

// ---------------------------------------------------------------------------
// Typed accessors — strict: only the matching JSON type yields a value, anything
// else falls back to the caller's default. LLM parsing wants this: a field that is
// unexpectedly a string should not silently coerce to a number.
// ---------------------------------------------------------------------------
bool JsonValue::asBool(bool def) const
{
    return type_ == Type::Bool ? bool_ : def;
}

double JsonValue::asNumber(double def) const
{
    return type_ == Type::Number ? num_ : def;
}

long JsonValue::asInt(long def) const
{
    return type_ == Type::Number ? static_cast<long>(num_) : def;
}

wxString JsonValue::asString(const wxString& def) const
{
    return type_ == Type::String ? str_ : def;
}

const JsonValue& JsonValue::operator[](const wxString& key) const
{
    if (type_ != Type::Object) return kNull;
    for (const auto& kv : obj_)
        if (kv.first == key) return kv.second;
    return kNull;
}

const JsonValue& JsonValue::operator[](size_t i) const
{
    if (type_ != Type::Array || i >= arr_.size()) return kNull;
    return arr_[i];
}

size_t JsonValue::size() const
{
    if (type_ == Type::Array)  return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

bool JsonValue::has(const wxString& key) const
{
    if (type_ != Type::Object) return false;
    for (const auto& kv : obj_)
        if (kv.first == key) return true;
    return false;
}

// A segment made entirely of ASCII digits is treated as an array index (per the header
// contract "choices.0.delta.content"). Empty is not numeric.
static bool IsAllDigits(const wxString& s)
{
    if (s.empty()) return false;
    for (wxUniChar c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

const JsonValue& JsonValue::path(const wxString& dotted) const
{
    if (dotted.empty()) return *this;   // empty path addresses this value itself

    const JsonValue* cur = this;
    size_t start = 0;
    const size_t n = dotted.length();
    // Manual split on '.'. A trailing dot yields an empty final segment which simply
    // misses (kNull) rather than being treated specially.
    while (start <= n) {
        size_t dot = dotted.find(wxUniChar('.'), start);
        size_t end = (dot == wxString::npos) ? n : dot;
        wxString seg = dotted.Mid(start, end - start);
        if (cur->type_ == Type::Array && IsAllDigits(seg)) {
            unsigned long idx = 0;
            for (wxUniChar c : seg) idx = idx * 10 + (unsigned)(c.GetValue() - '0');
            cur = &(*cur)[static_cast<size_t>(idx)];
        } else if (cur->type_ == Type::Object) {
            cur = &(*cur)[seg];
        } else {
            return kNull;
        }
        if (dot == wxString::npos) break;
        start = dot + 1;
    }
    return *cur;
}

// ---------------------------------------------------------------------------
// Recursive-descent parser over a UTF-8 byte buffer. File-local (declared friend in the
// header) so it can populate JsonValue's private fields directly.
// ---------------------------------------------------------------------------
class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s), n_(s.size()) {}

    bool parse(JsonValue& out)
    {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        if (i_ != n_) { fail("trailing characters after JSON value"); return false; }
        return true;
    }

    std::string err;

private:
    static const int kMaxDepth = 200;  // guards against stack exhaustion on hostile input

    const std::string& s_;
    size_t             i_ = 0;
    size_t             n_ = 0;
    int                depth_ = 0;

    void fail(const char* msg)
    {
        if (err.empty()) {
            char pos[32];
            std::snprintf(pos, sizeof pos, " (at %zu)", i_);
            err = msg;
            err += pos;
        }
    }

    void skipWs()
    {
        while (i_ < n_) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }

    bool parseValue(JsonValue& out)
    {
        if (i_ >= n_) { fail("unexpected end of input"); return false; }
        char c = s_[i_];
        switch (c) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out.type_ = JsonValue::Type::String;
                out.str_  = wxString::FromUTF8(str.data(), str.size());
                return true;
            }
            case 't': return parseLiteral("true",  out, JsonValue::Type::Bool, true);
            case 'f': return parseLiteral("false", out, JsonValue::Type::Bool, false);
            case 'n': return parseLiteral("null",  out, JsonValue::Type::Null, false);
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
                fail("unexpected character");
                return false;
        }
    }

    bool parseLiteral(const char* lit, JsonValue& out, JsonValue::Type t, bool boolVal)
    {
        size_t len = std::strlen(lit);
        if (i_ + len > n_ || s_.compare(i_, len, lit) != 0) {
            fail("invalid literal");
            return false;
        }
        i_ += len;
        out.type_ = t;
        if (t == JsonValue::Type::Bool) out.bool_ = boolVal;
        return true;
    }

    bool parseObject(JsonValue& out)
    {
        if (++depth_ > kMaxDepth) { fail("nesting too deep"); return false; }
        struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{depth_};

        ++i_;  // consume '{'
        out.type_ = JsonValue::Type::Object;
        skipWs();
        if (i_ < n_ && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            skipWs();
            if (i_ >= n_ || s_[i_] != '"') { fail("expected string key in object"); return false; }
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (i_ >= n_ || s_[i_] != ':') { fail("expected ':' in object"); return false; }
            ++i_;
            skipWs();
            JsonValue val;
            if (!parseValue(val)) return false;
            out.obj_.emplace_back(wxString::FromUTF8(key.data(), key.size()), std::move(val));
            skipWs();
            if (i_ >= n_) { fail("unterminated object"); return false; }
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            fail("expected ',' or '}' in object");
            return false;
        }
    }

    bool parseArray(JsonValue& out)
    {
        if (++depth_ > kMaxDepth) { fail("nesting too deep"); return false; }
        struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{depth_};

        ++i_;  // consume '['
        out.type_ = JsonValue::Type::Array;
        skipWs();
        if (i_ < n_ && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            skipWs();
            JsonValue val;
            if (!parseValue(val)) return false;
            out.arr_.push_back(std::move(val));
            skipWs();
            if (i_ >= n_) { fail("unterminated array"); return false; }
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            fail("expected ',' or ']' in array");
            return false;
        }
    }

    // Parse a JSON string literal (leading '"' at i_) into raw UTF-8 bytes.
    bool parseString(std::string& out)
    {
        ++i_;  // consume opening quote
        for (;;) {
            if (i_ >= n_) { fail("unterminated string"); return false; }
            unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') { ++i_; return true; }
            if (c == '\\') {
                ++i_;
                if (i_ >= n_) { fail("unterminated escape"); return false; }
                char e = s_[i_];
                switch (e) {
                    case '"':  out += '"';  ++i_; break;
                    case '\\': out += '\\'; ++i_; break;
                    case '/':  out += '/';  ++i_; break;
                    case 'b':  out += '\b'; ++i_; break;
                    case 'f':  out += '\f'; ++i_; break;
                    case 'n':  out += '\n'; ++i_; break;
                    case 'r':  out += '\r'; ++i_; break;
                    case 't':  out += '\t'; ++i_; break;
                    case 'u':  if (!parseUnicodeEscape(out)) return false; break;
                    default:   fail("invalid escape sequence"); return false;
                }
            } else if (c < 0x20) {
                // Control characters must be escaped in strict JSON; reject to flag junk.
                fail("unescaped control character in string");
                return false;
            } else {
                // Raw byte (ASCII or a UTF-8 continuation/lead byte) — pass through.
                out += static_cast<char>(c);
                ++i_;
            }
        }
    }

    // At '\u...': read 4 hex digits, honour high+low surrogate pairs, emit UTF-8 bytes.
    bool parseUnicodeEscape(std::string& out)
    {
        ++i_;  // consume 'u'
        uint32_t cp;
        if (!readHex4(cp)) return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: a valid low surrogate must follow as another \uXXXX.
            if (i_ + 1 < n_ && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                i_ += 2;
                uint32_t lo;
                if (!readHex4(lo)) return false;
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    cp = 0xFFFD;  // unpaired high surrogate -> replacement char
                }
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // lone low surrogate
        }
        appendUtf8(out, cp);
        return true;
    }

    bool readHex4(uint32_t& out)
    {
        if (i_ + 4 > n_) { fail("truncated \\u escape"); return false; }
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s_[i_++];
            v <<= 4;
            if      (h >= '0' && h <= '9') v |= (h - '0');
            else if (h >= 'a' && h <= 'f') v |= (h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= (h - 'A' + 10);
            else { fail("invalid hex digit in \\u escape"); return false; }
        }
        out = v;
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t cp)
    {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseNumber(JsonValue& out)
    {
        size_t start = i_;
        if (i_ < n_ && s_[i_] == '-') ++i_;
        // integer part
        if (i_ < n_ && s_[i_] == '0') {
            ++i_;  // leading zero: no further integer digits allowed
        } else if (i_ < n_ && s_[i_] >= '1' && s_[i_] <= '9') {
            while (i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        } else {
            fail("invalid number");
            return false;
        }
        // fraction
        if (i_ < n_ && s_[i_] == '.') {
            ++i_;
            if (!(i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9')) { fail("invalid number fraction"); return false; }
            while (i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        // exponent
        if (i_ < n_ && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < n_ && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (!(i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9')) { fail("invalid number exponent"); return false; }
            while (i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        // Convert locale-independently: JSON always uses '.' as the decimal separator,
        // whereas strtod() would honour the process locale.
        wxString token = wxString::FromUTF8(s_.data() + start, i_ - start);
        double d = 0;
        token.ToCDouble(&d);
        out.type_ = JsonValue::Type::Number;
        out.num_  = d;
        return true;
    }
};

JsonValue JsonValue::Parse(const wxString& text, wxString* err)
{
    // Take a stable UTF-8 byte copy to parse against.
    std::string bytes(text.utf8_str());
    JsonParser p(bytes);
    JsonValue v;
    if (!p.parse(v)) {
        if (err) *err = wxString::FromUTF8(p.err.c_str());
        return JsonValue();  // Null
    }
    if (err) err->clear();
    return v;
}

// ---------------------------------------------------------------------------
// JsonWriter — compact serialiser for request bodies.
//
// firstStack_ tracks, per open container, whether the next element is the first (so we
// know when to emit a separating comma). expectValue_ suppresses the comma for the value
// that directly follows a Key(): the comma already went before the key.
// ---------------------------------------------------------------------------
void JsonWriter::sep()
{
    if (expectValue_) { expectValue_ = false; return; }
    if (!firstStack_.empty()) {
        if (firstStack_.back()) firstStack_.back() = false;
        else                    out_ += ',';
    }
}

JsonWriter& JsonWriter::BeginObject()
{
    sep();
    out_ += '{';
    firstStack_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::EndObject()
{
    out_ += '}';
    if (!firstStack_.empty()) firstStack_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::BeginArray()
{
    sep();
    out_ += '[';
    firstStack_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::EndArray()
{
    out_ += ']';
    if (!firstStack_.empty()) firstStack_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::Key(const wxString& k)
{
    // A key is a member boundary: comma between members, but never before the value.
    if (!firstStack_.empty()) {
        if (firstStack_.back()) firstStack_.back() = false;
        else                    out_ += ',';
    }
    out_ += EscapeString(k);
    out_ += ':';
    expectValue_ = true;
    return *this;
}

JsonWriter& JsonWriter::Str(const wxString& v)
{
    sep();
    out_ += EscapeString(v);
    return *this;
}

JsonWriter& JsonWriter::Num(double v)
{
    sep();
    // FromCDouble is locale-independent (always '.'); default precision picks the
    // shortest representation that round-trips.
    out_ += wxString::FromCDouble(v);
    return *this;
}

JsonWriter& JsonWriter::Int(long v)
{
    sep();
    out_ += wxString::Format(wxT("%ld"), v);
    return *this;
}

JsonWriter& JsonWriter::Bool(bool v)
{
    sep();
    out_ += v ? wxT("true") : wxT("false");
    return *this;
}

JsonWriter& JsonWriter::Null()
{
    sep();
    out_ += wxT("null");
    return *this;
}

JsonWriter& JsonWriter::Raw(const wxString& jsonFragment)
{
    sep();
    out_ += jsonFragment;
    return *this;
}

wxString JsonWriter::EscapeString(const wxString& s)
{
    wxString o;
    o += '"';
    for (wxUniChar c : s) {
        wxUint32 v = c.GetValue();
        switch (v) {
            case '"':  o += wxT("\\\""); break;
            case '\\': o += wxT("\\\\"); break;
            case '\b': o += wxT("\\b");  break;
            case '\f': o += wxT("\\f");  break;
            case '\n': o += wxT("\\n");  break;
            case '\r': o += wxT("\\r");  break;
            case '\t': o += wxT("\\t");  break;
            default:
                if (v < 0x20) o += wxString::Format(wxT("\\u%04x"), v);
                else          o += c;    // pass all other code points through verbatim
                break;
        }
    }
    o += '"';
    return o;
}

} // namespace ai
