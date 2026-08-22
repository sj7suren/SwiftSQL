// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SseParser.h — incremental framing for streaming LLM responses. HTTP chunks do NOT
// arrive on line/event boundaries (a "data:" line can be split across two reads, a
// multi-byte UTF-8 char can be halved), so we buffer and emit only COMPLETE payloads.
//
// Two modes:
//   Sse    — Server-Sent Events (OpenAI/Anthropic/Gemini): an event ends at a blank
//            line; we concatenate its "data:" lines and emit that payload. The literal
//            "[DONE]" is emitted as-is (the provider recognises it).
//   Ndjson — newline-delimited JSON (Ollama native): each complete line is one payload.
//
// Pure & unit-tested (tests/sse_parser_test.cpp): half-line splits, multiple events in
// one chunk, CRLF vs LF, UTF-8 byte splits, trailing partial with no newline.
#pragma once

#include <wx/string.h>

#include <vector>

namespace ai {

class SseParser {
public:
    enum class Mode { Sse, Ndjson };
    explicit SseParser(Mode mode = Mode::Sse) : mode_(mode) {}

    // Feed a raw chunk (already decoded to a wxString by the transport). Returns any
    // payloads completed by this chunk (SSE: the text after "data: "; NDJSON: the line).
    // Incomplete tails are retained for the next Feed.
    std::vector<wxString> Feed(const wxString& chunk);

    // Flush any buffered final payload that had no trailing newline (call on stream end).
    std::vector<wxString> Finish();

private:
    Mode     mode_;
    wxString buf_;        // bytes not yet forming a complete line
    wxString eventData_;  // SSE: accumulated data across lines of the current event
    bool     haveData_ = false;
};

} // namespace ai
