// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AiTypes.h — the AI layer's internal domain model (the "anti-corruption layer"
// vocabulary). Every provider adapter translates an external API's shapes to/from
// these types, so the rest of SwiftSQL only ever sees AiRequest / AiChunk / AiError
// regardless of which vendor is configured. Pure value objects, no I/O, no wx GUI.
//
// See docs/design/ai-integration.md §2 for the architecture these types anchor.
#pragma once

#include <wx/string.h>

#include <functional>
#include <vector>

namespace ai {

// Chat roles, normalised across vendors (Gemini's "model" ⇄ Assistant is mapped
// inside GeminiProvider; Anthropic's top-level system ⇄ Role::System likewise).
enum class Role { System, User, Assistant };

struct AiMessage {
    Role     role;
    wxString content;
};

// One chat turn. `temperature < 0` means "omit the field" (use the server default),
// which keeps long-tail providers that reject the parameter happy.
struct AiRequest {
    wxString               model;
    std::vector<AiMessage> messages;
    double                 temperature = 0.7;
    int                    maxTokens   = 2048;   // Anthropic requires this; others optional
    bool                   stream      = true;
};

// Normalised error taxonomy — every vendor's error maps to one of these so the UI
// only handles a fixed, translatable set (see docs/design/ai-integration.md §5 table).
enum class AiErrorKind {
    None = 0,          // success sentinel
    Auth,              // 401/403, bad key / missing version header
    RateLimit,         // 429 (honour Retry-After)
    Timeout,           // connect / idle timeout
    ContextOverflow,   // request exceeded the model's context window
    BadRequest,        // 400 other (bad model id, unsupported param)
    ServerError,       // 5xx
    StreamInterrupted, // SSE/NDJSON cut mid-stream (terminator never arrived)
    Network,           // DNS/connect failure (e.g. local Ollama not running)
    Unknown
};

struct AiError {
    AiErrorKind kind       = AiErrorKind::None;
    int         httpStatus = 0;
    wxString    message;      // human-readable, already de-vendored where possible
    wxString    providerRaw;  // original body/snippet for diagnostics (never shown raw)

    bool ok() const { return kind == AiErrorKind::None; }
};

// Streaming sinks. When a request is driven by wxWebRequest (Storage_None), these
// fire on the GUI thread already, so implementations need no extra CallAfter.
//   onDelta : incremental text as it arrives (may be called many times)
//   onDone  : stream finished cleanly; token counts are -1 when the vendor omits them
//   onError : terminal failure; any text already delivered via onDelta is kept
struct AiCallbacks {
    std::function<void(const wxString& deltaText)>                     onDelta;
    std::function<void(int promptTokens, int completionTokens)>        onDone;
    std::function<void(const AiError&)>                                onError;
};

} // namespace ai
