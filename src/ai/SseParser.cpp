// SseParser.cpp — implementation of the incremental stream framer declared in SseParser.h.
//
// The transport decodes bytes to a wxString before calling Feed(), so at this layer a
// "chunk" is well-formed text — our job is purely to reassemble LINES and EVENTS that
// HTTP split across chunk boundaries. We buffer any tail that does not yet end in '\n'
// and only emit COMPLETE payloads.
//
// Field parsing follows the SSE spec closely enough for LLM providers: a "field: value"
// line (one optional space after the colon is stripped); a bare ":" comment line is
// ignored; only "data" fields are collected. A blank line terminates the event and
// flushes the accumulated data as one payload. Non-data fields (event/id/retry) are
// intentionally dropped — the providers convey everything we need in data payloads.
#include "ai/SseParser.h"

namespace ai {

namespace {

// Process one complete (newline-stripped) SSE line, mutating the current event's data
// accumulator and appending a payload to `out` on a blank-line flush.
void ProcessSseLine(const wxString& line, wxString& eventData, bool& haveData,
                    std::vector<wxString>& out)
{
    if (line.empty()) {
        // Blank line => dispatch the current event (if any data was collected).
        if (haveData) {
            out.push_back(eventData);
            eventData.clear();
            haveData = false;
        }
        return;
    }

    int colon = line.Find(wxUniChar(':'));
    wxString field, value;
    if (colon == wxNOT_FOUND) {
        field = line;          // "field" with an implicit empty value
    } else if (colon == 0) {
        return;                // leading colon => comment line, ignore entirely
    } else {
        field = line.Left(colon);
        value = line.Mid(colon + 1);
        if (!value.empty() && value[0] == ' ')
            value = value.Mid(1);   // strip a single leading space after the colon
    }

    if (field == wxT("data")) {
        // Multiple data lines in one event are joined with '\n' (per the SSE spec). The
        // literal "[DONE]" is just data here — it flushes as a payload on the blank line,
        // and the provider recognises it.
        if (haveData) eventData += '\n';
        eventData += value;
        haveData = true;
    }
    // event:/id:/retry: and any other field are ignored on purpose.
}

// Process one complete line in NDJSON mode: each non-empty line is a standalone payload.
void ProcessNdjsonLine(const wxString& line, std::vector<wxString>& out)
{
    if (!line.empty())
        out.push_back(line);
}

} // namespace

std::vector<wxString> SseParser::Feed(const wxString& chunk)
{
    std::vector<wxString> out;
    buf_ += chunk;

    // Peel off every complete line (terminated by '\n'); tolerate CRLF by dropping a
    // trailing '\r'. Whatever remains after the last '\n' stays buffered for next Feed.
    for (;;) {
        size_t nl = buf_.find(wxUniChar('\n'));
        if (nl == wxString::npos) break;
        wxString line = buf_.Mid(0, nl);
        buf_ = buf_.Mid(nl + 1);
        if (!line.empty() && line.Last() == '\r') line.RemoveLast();

        if (mode_ == Mode::Sse) ProcessSseLine(line, eventData_, haveData_, out);
        else                    ProcessNdjsonLine(line, out);
    }
    return out;
}

std::vector<wxString> SseParser::Finish()
{
    std::vector<wxString> out;

    // A final line with no trailing newline is still a complete payload at end-of-stream.
    if (!buf_.empty()) {
        wxString line = buf_;
        if (!line.empty() && line.Last() == '\r') line.RemoveLast();
        buf_.clear();
        if (mode_ == Mode::Sse) ProcessSseLine(line, eventData_, haveData_, out);
        else                    ProcessNdjsonLine(line, out);
    }

    // SSE: an event whose terminating blank line never arrived (stream just ended) still
    // needs to be flushed.
    if (mode_ == Mode::Sse && haveData_) {
        out.push_back(eventData_);
        eventData_.clear();
        haveData_ = false;
    }
    return out;
}

} // namespace ai
