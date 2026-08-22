// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AiClient.cpp — see AiClient.h. Orchestration only; no I/O, no framing, no vendor JSON
// live here — those belong to AiHttp, SseParser and the providers respectively.
#include "ai/AiClient.h"

#include "ai/IAiProvider.h"
#include "ai/SseParser.h"

#include <utility>

namespace ai {

namespace {

// A turn that ends with neither text nor a recognised error must still SAY
// something — a blank reply with no reason is indistinguishable from a hang.
//
// What it says is a HINT, NOT A DUMP. This case means the server answered 2xx
// yet produced nothing usable, which in practice is a configuration problem
// (wrong model id, wrong base URL, a key without access to that model) rather
// than anything the user can read out of the bytes. So the message points at
// the setting to check; the raw body goes to providerRaw, which AiTypes.h keeps
// for diagnostics and never shows verbatim.
AiError BlankResponseError(int status, const wxString& body)
{
    AiError e;
    e.kind       = AiErrorKind::Unknown;
    e.httpStatus = status;
    e.message    = (body.IsEmpty() ? wxString(L"AI 未返回任何内容。")
                                   : wxString(L"AI 返回的内容无法识别。"))
                 + L"请检查 AI API 接口配置是否正确（模型名称、Base URL、API Key）。";
    e.providerRaw = body;
    return e;
}

} // namespace

AiClient::AiClient(AiProviderConfig cfg)
    : cfg_(std::move(cfg))
{
}

// Out-of-line so unique_ptr<IAiProvider>/unique_ptr<SseParser> can hold incomplete types
// in the header.
AiClient::~AiClient() = default;

void AiClient::Chat(const AiRequest& req, const AiCallbacks& cb)
{
    if (busy_)
        return;   // single in-flight request; caller gates on Busy()

    cb_       = cb;
    provider_ = CreateProvider(cfg_.kind);
    if (!provider_) {
        if (cb_.onError) {
            AiError e;
            e.kind    = AiErrorKind::Unknown;
            e.message = wxS("no AI provider for configured kind");
            cb_.onError(e);
        }
        return;
    }

    // Fill the model default, then let the provider translate to a wire spec.
    AiRequest r = req;
    if (r.model.empty())
        r.model = cfg_.model;
    stream_ = r.stream;

    const HttpRequestSpec spec = provider_->BuildRequest(r, cfg_);

    parser_ = std::make_unique<SseParser>(
        cfg_.kind == ProviderKind::OllamaNative ? SseParser::Mode::Ndjson
                                                : SseParser::Mode::Sse);

    promptTokens_     = -1;
    completionTokens_ = -1;
    gotData_          = false;
    gotDelta_         = false;
    streamError_      = AiError{};   // per-turn: a previous turn's error must not leak in
    busy_             = true;

    http_.Start(
        spec, stream_, cfg_.timeoutSec,
        [this](const wxString& chunk)          { HandleData(chunk); },
        [this](int status, const wxString& b)  { HandleDone(status, b); },
        [this](AiHttp::Fail f, const wxString& d) { HandleErr(f, d); });
}

void AiClient::ListModels(const ModelsSink& sink)
{
    modelsSink_     = sink;
    modelsProvider_ = CreateProvider(cfg_.kind);
    if (!modelsProvider_) {
        if (modelsSink_) {
            AiError e;
            e.kind    = AiErrorKind::Unknown;
            e.message = wxS("no AI provider for configured kind");
            modelsSink_({}, e);
        }
        return;
    }

    const HttpRequestSpec spec = modelsProvider_->BuildModelsRequest(cfg_);

    // Non-streaming GET: the whole body is buffered and handed over via onDone.
    http_.Start(
        spec, /*stream=*/false, cfg_.timeoutSec,
        [](const wxString&) {},                             // no per-chunk data for a GET
        [this](int status, const wxString& b)  { HandleModelsDone(status, b); },
        [this](AiHttp::Fail f, const wxString& d) { HandleModelsErr(f, d); });
}

void AiClient::Cancel()
{
    http_.Cancel();
    busy_ = false;
}

void AiClient::HandleData(const wxString& chunk)
{
    gotData_ = true;
    if (!parser_)
        return;
    for (const auto& payload : parser_->Feed(chunk))
        EmitPayload(payload);
}

void AiClient::EmitPayload(const wxString& payload)
{
    wxString delta;
    bool     done = false;   // vendor terminator ([DONE]/message_stop/final chunk)
    AiError  err;
    // promptTokens_/completionTokens_ are left untouched unless this event carries usage.
    provider_->ParseStreamEvent(payload, delta, done, promptTokens_, completionTokens_, err);
    // Keep the FIRST error: vendors often emit an error event and then tear the
    // stream down, and a later generic event must not overwrite the real cause.
    if (!err.ok() && streamError_.ok())
        streamError_ = err;
    if (!delta.empty()) {
        gotDelta_ = true;
        if (cb_.onDelta) cb_.onDelta(delta);
    }
    // `done` needs no action: the transport still drives us to onDone; any further
    // post-terminator events (rare) simply yield no delta.
}

void AiClient::HandleDone(int status, const wxString& body)
{
    // Non-2xx: the provider turns status + body into a normalised AiError.
    if (status < 200 || status >= 300) {
        Finish();
        if (cb_.onError)
            cb_.onError(provider_->MapHttpError(status, body));
        return;
    }

    if (!stream_) {
        // Non-streaming: parse the whole body → full text, delivered as a single delta.
        AiError  err;
        wxString full = provider_->ParseFullResponse(body, err);
        if (!err.ok()) {
            Finish();
            if (cb_.onError) cb_.onError(err);
            return;
        }
        if (full.empty()) {
            // Same rule as the streaming path: a 2xx that carries no text and no
            // parsable error is reported, not passed off as an empty answer.
            Finish();
            if (cb_.onError) cb_.onError(BlankResponseError(status, body));
            return;
        }
        // Deliver, then finish — onDelta fires while still Busy(), exactly as the
        // streaming path does. Only the terminal callback follows Finish().
        if (cb_.onDelta) cb_.onDelta(full);
        Finish();
        if (cb_.onDone) cb_.onDone(promptTokens_, completionTokens_);
        return;
    }

    // Streaming: flush any payload the parser had buffered without a trailing newline.
    if (parser_)
        for (const auto& payload : parser_->Finish())
            EmitPayload(payload);
    Finish();
    // A 200 stream that carried an error event is a FAILED turn, not an empty one.
    // Reporting it here — after the stream is fully drained and exactly once —
    // is what turns the old silent "(No response)" into the provider's real
    // reason. Exactly one terminal callback fires, never both.
    if (!streamError_.ok()) {
        if (cb_.onError) cb_.onError(streamError_);
        return;
    }

    // A 2xx STREAM THAT PRODUCED NOTHING WAS NEVER REALLY A STREAM. Providers
    // answer a stream request with an ordinary JSON body far more often than
    // their docs admit — a bare error object, or an entire non-stream response.
    // SseParser discards every line of it, because SSE framing only collects
    // "data:" fields and such a body has none; the turn then ended with zero
    // deltas and the panel printed "(No response)" while the real explanation
    // sat unread in `body`. Re-read it as a whole response and say what it was.
    if (!gotDelta_) {
        AiError err;
        const wxString full = provider_->ParseFullResponse(body, err);
        if (!err.ok()) {
            if (cb_.onError) cb_.onError(err);
            return;
        }
        if (!full.empty()) {
            if (cb_.onDelta) cb_.onDelta(full);
            if (cb_.onDone)  cb_.onDone(promptTokens_, completionTokens_);
            return;
        }
        // Neither text nor a recognised error shape — never end quietly.
        if (cb_.onError) cb_.onError(BlankResponseError(status, body));
        return;
    }

    if (cb_.onDone) cb_.onDone(promptTokens_, completionTokens_);
}

void AiClient::HandleModelsDone(int status, const wxString& body)
{
    const ModelsSink sink = modelsSink_;   // copy: sink may re-enter (start another call)
    if (status < 200 || status >= 300) {
        AiError err = modelsProvider_->MapHttpError(status, body);
        if (sink) sink({}, err);
        return;
    }
    std::vector<wxString> models = modelsProvider_->ParseModels(body);
    if (sink) sink(std::move(models), AiError{});
}

void AiClient::HandleModelsErr(AiHttp::Fail reason, const wxString& detail)
{
    const ModelsSink sink = modelsSink_;
    AiError e;
    e.kind        = (reason == AiHttp::Fail::Timeout) ? AiErrorKind::Timeout
                                                      : AiErrorKind::Network;
    e.message     = detail;
    e.providerRaw = detail;
    if (sink) sink({}, e);
}

void AiClient::HandleErr(AiHttp::Fail reason, const wxString& detail)
{
    Finish();
    AiError e;
    if (reason == AiHttp::Fail::Timeout)
        e.kind = AiErrorKind::Timeout;
    else if (stream_ && gotData_)
        e.kind = AiErrorKind::StreamInterrupted;   // cut after data began flowing
    else
        e.kind = AiErrorKind::Network;             // connect/DNS failure (e.g. Ollama down)
    e.message     = detail;
    e.providerRaw = detail;
    if (cb_.onError) cb_.onError(e);
}

} // namespace ai
