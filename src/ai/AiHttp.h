// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AiHttp.h — the ONE place in SwiftSQL that touches wxWebRequest. An async, streaming
// HTTP POST engine: it fires a request, pumps raw response text back chunk-by-chunk
// (for streaming SSE/NDJSON) or hands over the whole body at the end (non-streaming),
// and reports transport-level failure. It knows NOTHING about LLM shapes — no JSON, no
// SSE framing, no provider vocabulary. That separation keeps the vendor providers pure
// (see IAiProvider.h) and confines all wx::net surface to a single translation unit.
//
// Threading: the wxWebSession backend does its socket I/O on a worker thread but posts
// wxEVT_WEBREQUEST_* events to the owning wxEvtHandler on the GUI thread's event loop,
// so every callback below fires on the GUI thread — callers need no CallAfter.
//
// UTF-8 safety: HTTP read boundaries can bisect a multi-byte UTF-8 code point. AiHttp
// buffers the trailing incomplete bytes and only ever emits COMPLETE, well-formed
// wxString chunks to onData — so downstream SseParser/providers never see mojibake.
//
// Timeout: wxWebRequest exposes no per-request timeout, so AiHttp arms a one-shot idle
// timer (reset on every data event); if it fires it cancels the request and reports a
// Timeout failure. This covers both connect stalls and mid-stream idle hangs.
#pragma once

#include <wx/string.h>

#include <functional>
#include <memory>

namespace ai {

struct HttpRequestSpec;   // ai/IAiProvider.h — kept out of this header on purpose

class AiHttp {
public:
    // Why the transport failed (mapped by AiClient onto the richer AiError taxonomy).
    enum class Fail { Network, Timeout };

    using DataCb = std::function<void(const wxString& chunk)>;         // one raw text chunk
    using DoneCb = std::function<void(int status, const wxString& body)>; // HTTP done (2xx or not)
    using ErrCb  = std::function<void(Fail reason, const wxString& detail)>; // transport failed

    AiHttp();
    ~AiHttp();
    AiHttp(const AiHttp&)            = delete;
    AiHttp& operator=(const AiHttp&) = delete;

    // Fire an async POST. `stream` selects Storage_None + per-chunk onData delivery;
    // otherwise the body is buffered and delivered once via onDone. `timeoutSec<=0`
    // falls back to 60s. Exactly ONE of onDone / onErr is invoked to terminate a call
    // (a user Cancel() is silent). Starting a new request cancels any in-flight one.
    void Start(const HttpRequestSpec& spec, bool stream, int timeoutSec,
               DataCb onData, DoneCb onDone, ErrCb onErr);

    // Abort the in-flight request, if any. Silent: no onDone / onErr will fire for it.
    void Cancel();

    // True between Start() and the terminal onDone / onErr (or a Cancel()).
    bool Active() const;

private:
    class Impl;                       // owns the wxWebRequest + timer; a wxEvtHandler
    std::unique_ptr<Impl> impl_;
};

} // namespace ai
