// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ai_stream_error_test.cpp — every provider must surface an error that arrives
// INSIDE a 200 stream.
//
// THE BUG THIS PINS DOWN. All three ParseStreamEvent implementations used to look
// only for content. When a vendor answered 200 and then reported the failure as a
// stream event — an unknown model id, an exhausted quota, a rate limit, an Ollama
// model that was never pulled — the parser produced no delta, no error, and no
// terminator. The transport saw a clean stream, AiClient called onDone, and the
// chat panel printed its empty-reply fallback "(No response)". The provider's
// actual message was parsed and thrown away. A user could not tell a wrong model
// id from a dead network.
//
// So the assertions come in pairs, and the SECOND half matters most: an error
// event must be reported, AND an ordinary chunk must NOT be mistaken for one. A
// false positive here would break every working conversation, which is a worse
// failure than the silence it replaces.
#include "ai/AnthropicProvider.h"
#include "ai/OllamaProvider.h"
#include "ai/OpenAiCompatProvider.h"

#include <cstdio>
#include <wx/string.h>

using namespace ai;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

static void ExpectStr(const char* name, const wxString& got, const wxString& want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s\n    want: [%s]\n    got : [%s]\n", name,
                    (const char*)want.utf8_str(), (const char*)got.utf8_str());
    } else {
        std::printf("  ok   %s\n", name);
    }
}

// One parse in isolation: fresh outputs every time, exactly as AiClient does.
struct Parsed {
    wxString delta;
    bool     done = false;
    int      pt = -1, ct = -1;
    AiError  err;
};

static Parsed Run(const IAiProvider& p, const wxString& payload)
{
    Parsed r;
    p.ParseStreamEvent(payload, r.delta, r.done, r.pt, r.ct, r.err);
    return r;
}

int main()
{
    std::printf("== ai_stream_error_test ==\n");

    // ---------------------------------------------------------------- OpenAI-compat
    {
        OpenAiCompatProvider p;

        // Ordinary content chunk: text out, and explicitly NOT an error.
        Parsed ok = Run(p, LR"({"choices":[{"delta":{"content":"SELECT"}}]})");
        ExpectStr ("openai: content chunk yields delta", ok.delta, L"SELECT");
        ExpectTrue("openai: content chunk is not an error", ok.err.ok());

        // The regression this fix is for: 200 + an error object in the stream.
        Parsed bad = Run(p, LR"({"error":{"message":"Model Not Exist","type":"invalid_request_error"}})");
        ExpectTrue("openai: error event is reported", !bad.err.ok());
        ExpectStr ("openai: error message is the vendor's own", bad.err.message,
                   L"Model Not Exist");
        ExpectTrue("openai: error event yields no delta", bad.delta.IsEmpty());
        ExpectTrue("openai: raw payload kept for diagnostics",
                   !bad.err.providerRaw.IsEmpty());

        // An error object with no message must still be an error, not silence.
        Parsed nomsg = Run(p, LR"({"error":{"type":"server_error"}})");
        ExpectTrue("openai: message-less error still reported", !nomsg.err.ok());
        ExpectTrue("openai: message-less error carries some text",
                   !nomsg.err.message.IsEmpty());

        // FALSE-POSITIVE GUARD. ExtractErrorMessage() also accepts a bare
        // top-level "message" — correct for a whole body, wrong for a chunk.
        // A normal event carrying such a field must stay a normal event.
        Parsed msg = Run(p, LR"({"message":"processing","choices":[{"delta":{"content":"x"}}]})");
        ExpectTrue("openai: top-level 'message' is NOT an error", msg.err.ok());
        ExpectStr ("openai: ...and its content still flows", msg.delta, L"x");

        // Terminator and usage still behave.
        Parsed done = Run(p, L"[DONE]");
        ExpectTrue("openai: [DONE] sets done", done.done);
        ExpectTrue("openai: [DONE] is not an error", done.err.ok());
    }

    // ---------------------------------------------------------------- Anthropic
    {
        AnthropicProvider p;

        Parsed ok = Run(p, LR"({"type":"content_block_delta","delta":{"text":"hi"}})");
        ExpectStr ("anthropic: content_block_delta yields delta", ok.delta, L"hi");
        ExpectTrue("anthropic: content_block_delta is not an error", ok.err.ok());

        Parsed bad = Run(p, LR"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})");
        ExpectTrue("anthropic: error event is reported", !bad.err.ok());
        ExpectStr ("anthropic: error message is the vendor's own", bad.err.message,
                   L"Overloaded");
        ExpectTrue("anthropic: error event yields no delta", bad.delta.IsEmpty());

        // Events that legitimately carry nothing must remain silent, not error.
        Parsed ping = Run(p, LR"({"type":"ping"})");
        ExpectTrue("anthropic: ping is not an error", ping.err.ok());
        ExpectTrue("anthropic: ping yields no delta", ping.delta.IsEmpty());

        Parsed stop = Run(p, LR"({"type":"message_stop"})");
        ExpectTrue("anthropic: message_stop sets done", stop.done);
        ExpectTrue("anthropic: message_stop is not an error", stop.err.ok());
    }

    // ---------------------------------------------------------------- Ollama
    {
        OllamaProvider p;

        Parsed ok = Run(p, LR"({"message":{"content":"local"},"done":false})");
        ExpectStr ("ollama: content chunk yields delta", ok.delta, L"local");
        ExpectTrue("ollama: content chunk is not an error", ok.err.ok());

        // The most common local failure: the model was never pulled.
        Parsed bad = Run(p, LR"({"error":"model 'llama3.1' not found, try pulling it first"})");
        ExpectTrue("ollama: error line is reported", !bad.err.ok());
        ExpectStr ("ollama: error message is the daemon's own", bad.err.message,
                   L"model 'llama3.1' not found, try pulling it first");
        ExpectTrue("ollama: error line yields no delta", bad.delta.IsEmpty());

        Parsed fin = Run(p, LR"({"message":{"content":""},"done":true})");
        ExpectTrue("ollama: final chunk sets done", fin.done);
        ExpectTrue("ollama: final chunk is not an error", fin.err.ok());
    }

    // ------------------------------------------- non-SSE body on a stream request
    {
        // THE SECOND HALF OF THE SAME BUG. A provider may answer a *streaming*
        // request with an ordinary JSON body (an error object, or a whole
        // non-stream response). SseParser yields nothing for it — no line carries
        // a "data:" field — so ParseStreamEvent never runs and the turn used to end
        // blank. AiClient now re-reads such a body with ParseFullResponse, so these
        // assertions pin the parsing that fallback depends on.
        OpenAiCompatProvider o;
        AiError err;
        wxString text = o.ParseFullResponse(
            LR"({"error":{"message":"Model Not Exist","type":"invalid_request_error"}})", err);
        ExpectTrue("openai(full): error body is recognised", !err.ok());
        ExpectStr ("openai(full): error message preserved", err.message, L"Model Not Exist");
        ExpectTrue("openai(full): error body yields no text", text.IsEmpty());

        AiError ok2;
        wxString good = o.ParseFullResponse(
            LR"({"choices":[{"message":{"content":"SELECT 1"}}]})", ok2);
        ExpectTrue("openai(full): normal body is not an error", ok2.ok());
        ExpectStr ("openai(full): normal body yields text", good, L"SELECT 1");

        AnthropicProvider a;
        AiError aerr;
        wxString atext = a.ParseFullResponse(
            LR"({"type":"error","error":{"type":"not_found_error","message":"model: bogus"}})", aerr);
        ExpectTrue("anthropic(full): error body is recognised", !aerr.ok());
        ExpectStr ("anthropic(full): error message preserved", aerr.message, L"model: bogus");
        ExpectTrue("anthropic(full): error body yields no text", atext.IsEmpty());

        // An empty 200 body must NOT look like valid content — AiClient turns this
        // case into a reported error rather than a blank turn.
        AiError eerr;
        wxString empty = o.ParseFullResponse(L"", eerr);
        ExpectTrue("openai(full): empty body yields no text", empty.IsEmpty());
    }

    // ---------------------------------------------------------------- shared
    {
        // Blank/garbage payloads must never be reported as provider errors —
        // keep-alive newlines are routine in SSE.
        OpenAiCompatProvider o; AnthropicProvider a; OllamaProvider l;
        ExpectTrue("openai: empty payload is not an error",    Run(o, L"   ").err.ok());
        ExpectTrue("anthropic: empty payload is not an error", Run(a, L"   ").err.ok());
        ExpectTrue("ollama: empty payload is not an error",    Run(l, L"   ").err.ok());
    }

    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails;
}
