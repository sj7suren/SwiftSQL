// Json.h — a small, self-contained JSON reader/writer for the AI layer. We parse
// untrusted network responses (LLM SSE/JSON bodies) and build request bodies, so we
// want a dependency-free, unit-tested parser rather than pulling a vendored header.
//
// Scope: full JSON value model (null/bool/number/string/array/object) with UTF-8 in
// wxString, standard escapes + \uXXXX (incl. surrogate pairs). Read via typed
// accessors and a dotted/indexed Path() helper; write via a tiny JsonWriter for
// request bodies. NOT a general-purpose library — just what the providers need.
#pragma once

#include <wx/string.h>

#include <map>
#include <memory>
#include <vector>

namespace ai {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }

    // Typed accessors — safe defaults on type mismatch (never throw).
    bool     asBool(bool def = false) const;
    double   asNumber(double def = 0) const;
    long     asInt(long def = 0) const;
    wxString asString(const wxString& def = wxString()) const;

    // Object member (Null value if absent / not an object).
    const JsonValue& operator[](const wxString& key) const;
    // Array element (Null value if out of range / not an array).
    const JsonValue& operator[](size_t i) const;
    size_t size() const;                       // array/object element count, else 0
    bool   has(const wxString& key) const;     // object contains key

    // Dotted path with numeric indices, e.g. "choices.0.delta.content".
    const JsonValue& path(const wxString& dotted) const;

    // Parse; on failure returns a Null value and fills `err` (if provided).
    static JsonValue Parse(const wxString& text, wxString* err = nullptr);

private:
    Type                                   type_ = Type::Null;
    bool                                   bool_ = false;
    double                                 num_  = 0;
    wxString                               str_;
    std::vector<JsonValue>                 arr_;
    std::vector<std::pair<wxString, JsonValue>> obj_;   // insertion-ordered

    static const JsonValue kNull;              // shared "absent" sentinel
    friend class JsonParser;
};

// Minimal builder for request bodies. Emits compact JSON. Strings are escaped.
// Usage: JsonWriter w; w.BeginObject(); w.Key("model").Str(model); ... w.EndObject();
class JsonWriter {
public:
    JsonWriter& BeginObject(); JsonWriter& EndObject();
    JsonWriter& BeginArray();  JsonWriter& EndArray();
    JsonWriter& Key(const wxString& k);
    JsonWriter& Str(const wxString& v);
    JsonWriter& Num(double v);
    JsonWriter& Int(long v);
    JsonWriter& Bool(bool v);
    JsonWriter& Null();
    // Emit an already-serialised JSON fragment verbatim (advanced; no escaping).
    JsonWriter& Raw(const wxString& jsonFragment);

    wxString str() const { return out_; }

    // Escape a bare string into a JSON string literal (with surrounding quotes).
    static wxString EscapeString(const wxString& s);

private:
    void sep();                 // comma between siblings
    wxString          out_;
    std::vector<bool> firstStack_;   // per-container "is next element the first?"
    bool              expectValue_ = false;  // a Key() was just written
};

} // namespace ai
