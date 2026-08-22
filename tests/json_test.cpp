// json_test.cpp — unit tests for ai::JsonValue (parser + accessors) and ai::JsonWriter.
//
// Same dependency-free harness as tableio_test.cpp: a tiny assert loop, no framework.
// We parse untrusted network shapes, so the emphasis is on: correct escape/\uXXXX +
// surrogate decoding, safe defaults on type mismatch, dotted-path navigation, and — most
// importantly — that malformed input never crashes (it must return Null + set err).
#include "ai/Json.h"

#include <cstdio>
#include <wx/string.h>

using namespace ai;

static int g_checks = 0;
static int g_fails  = 0;

static wxString Vis(const wxString& s)
{
    wxString o = s;
    o.Replace(L"\r", L"\\r");
    o.Replace(L"\n", L"\\n");
    o.Replace(L"\t", L"\\t");
    return o;
}

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)Vis(want).utf8_str(),
                    (const char*)Vis(got).utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

int main()
{
    std::printf("== ai::Json unit tests ==\n");

    // ---------------------------------------------------------------- primitives
    {
        wxString err = L"x";
        JsonValue v = JsonValue::Parse(L"true", &err);
        ExpectTrue("parse true", v.type() == JsonValue::Type::Bool && v.asBool() == true);
        ExpectTrue("parse true: err cleared", err.empty());

        ExpectTrue("parse false", !JsonValue::Parse(L"false").asBool());
        ExpectTrue("parse null", JsonValue::Parse(L"null").isNull());
        ExpectTrue("parse int", JsonValue::Parse(L"42").asInt() == 42);
        ExpectTrue("parse negative", JsonValue::Parse(L"-7").asInt() == -7);
        ExpectTrue("parse double", JsonValue::Parse(L"3.5").asNumber() == 3.5);
        ExpectTrue("parse exp", JsonValue::Parse(L"1e3").asNumber() == 1000.0);
        ExpectTrue("parse zero", JsonValue::Parse(L"0").asInt() == 0);
        ExpectStr ("parse string", JsonValue::Parse(L"\"hi\"").asString(), L"hi");
        ExpectTrue("leading whitespace + trailing newline",
                   JsonValue::Parse(L"  \n\t 5 \n").asInt() == 5);
    }

    // ---------------------------------------------------------------- nested object/array
    {
        wxString js =
            L"{\"model\":\"gpt\",\"n\":2,\"ok\":true,"
            L"\"choices\":[{\"delta\":{\"content\":\"Hello\"}},"
            L"{\"delta\":{\"content\":\"World\"}}],"
            L"\"meta\":{\"nested\":{\"deep\":[10,20,30]}}}";
        JsonValue v = JsonValue::Parse(js);
        ExpectTrue("obj: type", v.isObject());
        ExpectTrue("obj: size 5", v.size() == 5);
        ExpectStr ("obj: string member", v[L"model"].asString(), L"gpt");
        ExpectTrue("obj: int member", v[L"n"].asInt() == 2);
        ExpectTrue("obj: bool member", v[L"ok"].asBool());
        ExpectTrue("obj: has present", v.has(L"choices"));
        ExpectTrue("obj: has absent", !v.has(L"missing"));

        ExpectTrue("arr: size 2", v[L"choices"].size() == 2);
        ExpectStr ("arr: index nav",
                   v[L"choices"][0][L"delta"][L"content"].asString(), L"Hello");
        ExpectStr ("arr: index 1", v[L"choices"][1][L"delta"][L"content"].asString(), L"World");

        // dotted path (numeric segment == array index)
        ExpectStr ("path: choices.0.delta.content",
                   v.path(L"choices.0.delta.content").asString(), L"Hello");
        ExpectStr ("path: choices.1.delta.content",
                   v.path(L"choices.1.delta.content").asString(), L"World");
        ExpectTrue("path: meta.nested.deep.2",
                   v.path(L"meta.nested.deep.2").asInt() == 30);
        ExpectTrue("path: missing segment -> null",
                   v.path(L"choices.9.delta").isNull());
        ExpectTrue("path: wrong type mid-path -> null",
                   v.path(L"model.foo").isNull());
        ExpectTrue("path: empty path -> self", v.path(L"").isObject());
    }

    // ---------------------------------------------------------------- escapes
    {
        JsonValue v = JsonValue::Parse(L"\"a\\nb\\tc\\\"d\\\\e\\/f\\r\\b\\f\"");
        ExpectStr("escapes: standard set", v.asString(),
                  wxString(L"a\nb\tc\"d\\e/f\r\b\f"));
    }
    {
        // \uXXXX for a BMP char: U+0041 = 'A', U+4E2D = 中
        JsonValue v = JsonValue::Parse(L"\"\\u0041\\u4e2d\"");
        ExpectStr("uXXXX: BMP", v.asString(),
                  wxString(L"A") + wxString::FromUTF8("\xE4\xB8\xAD"));
    }
    {
        // Surrogate pair: U+1F600 GRINNING FACE encodes to 😀
        JsonValue v = JsonValue::Parse(L"\"x\\uD83D\\uDE00y\"");
        wxString want = wxString(L"x") + wxString::FromUTF8("\xF0\x9F\x98\x80") + L"y";
        ExpectStr("uXXXX: surrogate pair -> U+1F600", v.asString(), want);
    }
    {
        // Lone/unpaired high surrogate must degrade to U+FFFD, not crash.
        JsonValue v = JsonValue::Parse(L"\"\\uD83Dz\"");
        wxString want = wxString::FromUTF8("\xEF\xBF\xBD") + L"z";
        ExpectStr("uXXXX: unpaired high surrogate -> replacement", v.asString(), want);
    }
    {
        // Raw multi-byte UTF-8 inside the string passes through untouched.
        wxString cn = wxString::FromUTF8("\xE4\xBD\xA0\xE5\xA5\xBD"); // 你好
        JsonValue v = JsonValue::Parse(wxString(L"\"") + cn + L"\"");
        ExpectStr("raw utf-8 passthrough", v.asString(), cn);
    }

    // ---------------------------------------------------------------- type mismatch defaults
    {
        JsonValue v = JsonValue::Parse(L"{\"s\":\"text\",\"n\":5,\"b\":true}");
        ExpectTrue("mismatch: asInt on string -> def", v[L"s"].asInt(-1) == -1);
        ExpectStr ("mismatch: asString on number -> def", v[L"n"].asString(L"D"), L"D");
        ExpectTrue("mismatch: asBool on number -> def", v[L"n"].asBool(true) == true);
        ExpectTrue("mismatch: asNumber on bool -> def", v[L"b"].asNumber(9.0) == 9.0);
        ExpectTrue("absent member -> null sentinel", v[L"missing"].isNull());
        ExpectStr ("absent member -> asString def", v[L"missing"].asString(L"z"), L"z");
        ExpectTrue("index into non-array -> null", v[0].isNull());
        ExpectTrue("key into non-object -> null", v[L"n"][L"k"].isNull());
    }

    // ---------------------------------------------------------------- malformed: no crash, err set
    {
        const wchar_t* bad[] = {
            L"", L"   ", L"{", L"[", L"[1,2", L"{\"a\":}", L"{\"a\" 1}",
            L"{\"a\":1,}", L"nul", L"tru", L"\"unterminated", L"\"bad\\x\"",
            L"01", L"-", L"1.", L"1e", L".5", L"{\"a\":1}extra", L"}", L"]",
            L"\"\\uZZZZ\"", L"\"\\uD83D", L"[1 2]", L"{1:2}",
        };
        int nn = sizeof(bad) / sizeof(bad[0]);
        for (int k = 0; k < nn; ++k) {
            wxString err;
            JsonValue v = JsonValue::Parse(bad[k], &err);
            bool good = v.isNull() && !err.empty();
            if (!good) {
                std::printf("  FAIL malformed[%d] want Null+err, got type=%d err=[%s]\n",
                            k, (int)v.type(), (const char*)err.utf8_str());
                ++g_fails;
            } else {
                std::printf("  ok   malformed[%d] rejected\n", k);
            }
            ++g_checks;
        }
        // Deep nesting must not blow the stack — it hits the depth cap and errors out.
        {
            wxString deep;
            for (int k = 0; k < 5000; ++k) deep += '[';
            wxString err;
            JsonValue v = JsonValue::Parse(deep, &err);
            ExpectTrue("malformed: deep nesting rejected safely", v.isNull() && !err.empty());
        }
    }

    // ---------------------------------------------------------------- JsonWriter
    {
        JsonWriter w;
        w.BeginObject();
        w.Key(L"model").Str(L"deepseek-chat");
        w.Key(L"temperature").Num(0.7);
        w.Key(L"max_tokens").Int(2048);
        w.Key(L"stream").Bool(true);
        w.Key(L"stop").Null();
        w.Key(L"messages").BeginArray();
            w.BeginObject();
                w.Key(L"role").Str(L"user");
                w.Key(L"content").Str(L"hi \"there\"\nline2");
            w.EndObject();
        w.EndArray();
        w.EndObject();

        wxString out = w.str();
        ExpectStr("writer: compact output",
                  out,
                  L"{\"model\":\"deepseek-chat\",\"temperature\":0.7,\"max_tokens\":2048,"
                  L"\"stream\":true,\"stop\":null,\"messages\":[{\"role\":\"user\","
                  L"\"content\":\"hi \\\"there\\\"\\nline2\"}]}");

        // Round-trip: parse it back and confirm the structure.
        JsonValue v = JsonValue::Parse(out);
        ExpectStr ("round-trip: model", v[L"model"].asString(), L"deepseek-chat");
        ExpectTrue("round-trip: temperature", v[L"temperature"].asNumber() == 0.7);
        ExpectTrue("round-trip: max_tokens", v[L"max_tokens"].asInt() == 2048);
        ExpectTrue("round-trip: stream", v[L"stream"].asBool());
        ExpectTrue("round-trip: stop is null", v[L"stop"].isNull());
        ExpectStr ("round-trip: nested content",
                   v.path(L"messages.0.content").asString(), L"hi \"there\"\nline2");
    }
    {
        // EscapeString directly: control chars -> \u00XX, quotes/backslash escaped.
        // NB: build the control char via wxUniChar, not a "\x01b" literal — C++ hex
        // escapes are greedy and would fold the following 'b' into the value (0x1B).
        wxString ctl = wxString(L"a") + wxUniChar(0x01) + wxString(L"b\"c\\d");
        ExpectStr("escape: control + quote",
                  JsonWriter::EscapeString(ctl),
                  L"\"a\\u0001b\\\"c\\\\d\"");
        ExpectStr("escape: empty", JsonWriter::EscapeString(L""), L"\"\"");
    }
    {
        // Top-level array writer and Raw fragment passthrough.
        JsonWriter w;
        w.BeginArray().Int(1).Int(2).Raw(L"{\"k\":3}").EndArray();
        ExpectStr("writer: array + raw", w.str(), L"[1,2,{\"k\":3}]");
    }

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails;
}
