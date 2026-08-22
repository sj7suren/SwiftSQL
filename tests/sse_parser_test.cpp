// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// sse_parser_test.cpp — unit tests for ai::SseParser (streaming frame reassembly).
//
// Same dependency-free harness as tableio_test.cpp. HTTP does not respect line/event
// boundaries, so the core scenarios are: a "data:" line split across two Feed() calls,
// several events arriving in one chunk, CRLF vs LF, multi-line data joined with '\n',
// the literal "[DONE]", multi-byte (Chinese) content straddling a chunk boundary,
// NDJSON mode, and a trailing payload with no final newline flushed by Finish().
#include "ai/SseParser.h"

#include <cstdio>
#include <vector>
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

// Join a payload vector into "a|b|c" for compact comparison in one assertion.
static wxString Join(const std::vector<wxString>& v)
{
    wxString o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += '|';
        o += v[i];
    }
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
    std::printf("== ai::SseParser unit tests ==\n");

    // ---------------------------------------------------------------- half-line across chunks
    {
        SseParser p;
        std::vector<wxString> a = p.Feed(L"data: hel");   // partial line, no newline yet
        ExpectTrue("split: first chunk yields nothing", a.empty());
        std::vector<wxString> b = p.Feed(L"lo\n\n");       // completes line + blank flush
        ExpectStr("split: reassembled payload", Join(b), L"hello");
    }

    // ---------------------------------------------------------------- multiple events in one chunk
    {
        SseParser p;
        std::vector<wxString> a =
            p.Feed(L"data: one\n\ndata: two\n\ndata: three\n\n");
        ExpectStr("multi-event: three payloads", Join(a), L"one|two|three");
    }

    // ---------------------------------------------------------------- CRLF handling
    {
        SseParser p;
        std::vector<wxString> a = p.Feed(L"data: alpha\r\n\r\ndata: beta\r\n\r\n");
        ExpectStr("crlf: stripped, two payloads", Join(a), L"alpha|beta");
    }

    // ---------------------------------------------------------------- multi-line data joined with \n
    {
        SseParser p;
        std::vector<wxString> a = p.Feed(L"data: line1\ndata: line2\ndata: line3\n\n");
        ExpectStr("multi-data: joined with newline", Join(a), L"line1\nline2\nline3");
    }

    // ---------------------------------------------------------------- ignored fields + comments
    {
        SseParser p;
        std::vector<wxString> a =
            p.Feed(L": this is a comment\nevent: message\nid: 42\ndata: payload\n\n");
        ExpectStr("ignore: only data survives", Join(a), L"payload");
    }

    // ---------------------------------------------------------------- optional space after colon
    {
        SseParser p;
        // "data:x" (no space) and "data:  y" (two spaces -> one stripped, one kept).
        std::vector<wxString> a = p.Feed(L"data:x\n\ndata:  y\n\n");
        ExpectStr("colon: strips exactly one leading space", Join(a), L"x| y");
    }

    // ---------------------------------------------------------------- [DONE] sentinel
    {
        SseParser p;
        std::vector<wxString> a = p.Feed(L"data: {\"delta\":\"hi\"}\n\ndata: [DONE]\n\n");
        ExpectStr("done: emitted verbatim as its own payload",
                  Join(a), L"{\"delta\":\"hi\"}|[DONE]");
    }

    // ---------------------------------------------------------------- multi-byte split across chunks
    {
        // 你好世界 as UTF-8; the transport hands us decoded wxString chunks. Split the
        // Chinese payload across two Feed() calls (mid-string, before a newline arrives)
        // to prove multi-byte content is buffered and reassembled intact.
        wxString cn = wxString::FromUTF8("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C");
        SseParser p;
        std::vector<wxString> a = p.Feed(wxString(L"data: ") + cn.Mid(0, 2));  // partial
        ExpectTrue("multibyte: partial yields nothing", a.empty());
        std::vector<wxString> b = p.Feed(cn.Mid(2) + L"\n\n");                  // completes
        ExpectStr("multibyte: reassembled Chinese payload", Join(b), cn);
    }

    // ---------------------------------------------------------------- data field split at the colon
    {
        // Even the "data:" prefix itself can be split across chunks.
        SseParser p;
        p.Feed(L"da");
        p.Feed(L"ta: par");
        std::vector<wxString> a = p.Feed(L"tial\n\n");
        ExpectStr("split: prefix reassembled", Join(a), L"partial");
    }

    // ---------------------------------------------------------------- NDJSON mode
    {
        SseParser p(SseParser::Mode::Ndjson);
        std::vector<wxString> a =
            p.Feed(L"{\"a\":1}\n{\"b\":2}\n\n{\"c\":3}\n");   // blank line skipped
        ExpectStr("ndjson: line per payload, blank skipped",
                  Join(a), L"{\"a\":1}|{\"b\":2}|{\"c\":3}");
    }
    {
        // NDJSON half-line across chunks.
        SseParser p(SseParser::Mode::Ndjson);
        std::vector<wxString> a = p.Feed(L"{\"partial\":");
        ExpectTrue("ndjson: partial yields nothing", a.empty());
        std::vector<wxString> b = p.Feed(L"true}\n");
        ExpectStr("ndjson: completed line", Join(b), L"{\"partial\":true}");
    }

    // ---------------------------------------------------------------- Finish() flushes residual
    {
        // SSE: last event has data but the terminating blank line never arrived.
        SseParser p;
        std::vector<wxString> a = p.Feed(L"data: tail-no-newline");
        ExpectTrue("finish/sse: nothing before Finish", a.empty());
        std::vector<wxString> f = p.Finish();
        ExpectStr("finish/sse: residual data flushed", Join(f), L"tail-no-newline");
    }
    {
        // SSE: complete event already dispatched, blank line present, Finish adds nothing.
        SseParser p;
        p.Feed(L"data: done\n\n");
        ExpectTrue("finish/sse: no residual after clean event", p.Finish().empty());
    }
    {
        // NDJSON: last line has no trailing newline -> flushed by Finish.
        SseParser p(SseParser::Mode::Ndjson);
        p.Feed(L"{\"x\":1}\n{\"y\":2}");
        std::vector<wxString> f = p.Finish();
        ExpectStr("finish/ndjson: trailing line flushed", Join(f), L"{\"y\":2}");
    }

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails;
}
