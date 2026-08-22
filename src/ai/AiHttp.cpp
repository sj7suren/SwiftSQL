// AiHttp.cpp — see AiHttp.h. The whole wxWebRequest lifecycle lives here, behind a
// pimpl so no other translation unit ever sees wx::net types.
#include "ai/AiHttp.h"

#include "ai/IAiProvider.h"   // HttpRequestSpec

#include <wx/event.h>
#include <wx/timer.h>
#include <wx/webrequest.h>

#include <string>
#include <utility>

namespace ai {

namespace {

// Timer id for the idle-timeout timer (scoped to the Impl's own event table).
constexpr int kTimeoutTimerId = wxID_HIGHEST + 4201;

// Length of the largest prefix of `bytes` that ends on a UTF-8 code-point boundary.
// A read boundary can leave a lead byte without its continuation bytes; we return the
// offset up to (but excluding) that dangling last character so the caller can hold the
// remainder for the next read. ASCII and well-formed multibyte sequences pass through.
size_t Utf8CompleteLen(const std::string& bytes)
{
    if (bytes.empty())
        return 0;

    // Walk back from the end over continuation bytes (10xxxxxx) to the last lead byte.
    size_t start = bytes.size();
    while (start > 0) {
        const unsigned char c = static_cast<unsigned char>(bytes[start - 1]);
        --start;
        if ((c & 0xC0) != 0x80)   // ASCII (0xxxxxxx) or a lead byte (11xxxxxx)
            break;
    }

    const unsigned char lead = static_cast<unsigned char>(bytes[start]);
    size_t need;
    if ((lead & 0x80) == 0x00)      need = 1;   // 0xxxxxxx
    else if ((lead & 0xE0) == 0xC0) need = 2;   // 110xxxxx
    else if ((lead & 0xF0) == 0xE0) need = 3;   // 1110xxxx
    else if ((lead & 0xF8) == 0xF0) need = 4;   // 11110xxx
    else                            need = 1;   // stray continuation / invalid: don't stall

    const size_t have = bytes.size() - start;
    return (have >= need) ? bytes.size()   // last char is complete → all bytes usable
                          : start;         // hold back the dangling partial char
}

} // namespace

// ── Impl: the wxEvtHandler that owns the request + timer ─────────────────────────────
class AiHttp::Impl : public wxEvtHandler {
public:
    Impl()
        : timer_(this, kTimeoutTimerId)
    {
        // Events for the request are routed to this handler (see CreateRequest below).
        Bind(wxEVT_WEBREQUEST_STATE, &Impl::OnState,   this);
        Bind(wxEVT_WEBREQUEST_DATA,  &Impl::OnData,    this);
        Bind(wxEVT_TIMER,            &Impl::OnTimeout, this, kTimeoutTimerId);
    }

    ~Impl() override
    {
        timer_.Stop();
        if (request_.IsOk() && request_.GetState() == wxWebRequest::State_Active)
            request_.Cancel();
    }

    // ===================================================================
    // THE CRASH THIS EXISTS FOR
    // ===================================================================
    // This handler used to document "callers are expected to outlive their
    // in-flight requests" and leave it at that. A caller did not, and the
    // result was a hard 0xC0000005 EXECUTE fault: control jumped into .rdata,
    // i.e. a virtual call through the vtable of a destroyed object.
    //
    // WHY THE DESTRUCTOR'S Cancel() IS NOT ENOUGH. wxWebRequest::Cancel() is
    // ASYNCHRONOUS. It requests cancellation; the State_Cancelled event is
    // delivered later — to the wxEvtHandler that was handed to
    // CreateRequest(this, url), which by then has been freed. wx holds that
    // pointer raw and has no way to learn the handler died.
    //
    // The offending caller was the SQL editor's AI completer, which built a
    // fresh AiClient per keystroke burst; a fast typist superseded requests
    // faster than they completed, and each supersede destroyed a handler with
    // an event still on its way. It crashed after ~45 seconds of ordinary
    // typing, every time.
    //
    // Orphan() is the fix at the layer that owns the contract: instead of
    // dying while wx still points at it, the handler is DETACHED — it stops
    // calling upward (its callbacks are cleared, so the owner that is going
    // away can never be touched) and deletes itself once the transport
    // delivers the terminal event it is still waiting for.
    void Orphan()
    {
        orphaned_ = true;
        // Cleared FIRST. Whatever owned these callbacks is being destroyed
        // right now; a late event must not reach into it.
        onData_ = nullptr;
        onDone_ = nullptr;
        onErr_  = nullptr;
        timer_.Stop();

        if (active_ && request_.IsOk()) {
            request_.Cancel();   // → State_Cancelled → `delete this` in OnState
            return;
        }
        delete this;             // nothing in flight; nothing will ever arrive
    }

    void Start(const HttpRequestSpec& spec, bool stream, int timeoutSec,
               DataCb onData, DoneCb onDone, ErrCb onErr)
    {
        Cancel();      // supersede any in-flight request
        Reset();

        onData_    = std::move(onData);
        onDone_    = std::move(onDone);
        onErr_     = std::move(onErr);
        stream_    = stream;
        timeoutMs_ = (timeoutSec > 0 ? timeoutSec : 60) * 1000;

        wxWebSession& session = wxWebSession::GetDefault();
        request_ = session.CreateRequest(this, spec.url);
        if (!request_.IsOk()) {
            if (onErr_) onErr_(Fail::Network, wxS("could not create web request"));
            return;
        }

        request_.SetMethod(spec.method.empty() ? wxString(wxS("POST")) : spec.method);
        for (const auto& h : spec.headers)
            request_.SetHeader(h.first, h.second);
        request_.SetData(spec.body, wxS("application/json"));   // sets Content-Type + UTF-8 body

        if (stream_)
            request_.SetStorage(wxWebRequest::Storage_None);    // else default Storage_Memory

        active_ = true;
        ArmTimer();
        request_.Start();
    }

    void Cancel()
    {
        timer_.Stop();
        if (active_ && request_.IsOk())
            request_.Cancel();     // fires State_Cancelled; swallowed because timedOut_ is false
        active_ = false;
    }

    bool Active() const { return active_; }

private:
    void Reset()
    {
        rawTail_.clear();
        body_.clear();
        stream_   = false;
        active_   = false;
        timedOut_ = false;
    }

    void ArmTimer()  { if (timeoutMs_ > 0) timer_.StartOnce(timeoutMs_); }

    // Append raw response bytes, return only the well-formed UTF-8 prefix as a wxString;
    // any dangling partial code point is retained in rawTail_ for the next read.
    wxString DrainUtf8(const void* buf, size_t n)
    {
        rawTail_.append(static_cast<const char*>(buf), n);
        const size_t complete = Utf8CompleteLen(rawTail_);
        wxString out = wxString::FromUTF8(rawTail_.data(), complete);
        rawTail_.erase(0, complete);
        return out;
    }

    void OnData(wxWebRequestEvent& e)
    {
        if (!active_)
            return;
        ArmTimer();     // progress → reset the idle-timeout clock

        wxString chunk = DrainUtf8(e.GetDataBuffer(), e.GetDataSize());
        if (chunk.empty())
            return;
        body_ += chunk;                 // retained so a non-2xx stream body is available to onDone
        if (onData_) onData_(chunk);
    }

    void OnState(wxWebRequestEvent& e)
    {
        // AN ORPHANED HANDLER IS WAITING FOR EXACTLY THIS. Its owner is gone
        // and its callbacks are already cleared, so the only thing left to do
        // on a terminal state is stop existing. `delete this` inside one's own
        // event handler is safe as long as nothing touches the object
        // afterwards — hence the immediate return, and hence this check
        // sitting ABOVE the switch rather than inside each arm.
        if (orphaned_) {
            switch (e.GetState()) {
            case wxWebRequest::State_Completed:
            case wxWebRequest::State_Unauthorized:
            case wxWebRequest::State_Failed:
            case wxWebRequest::State_Cancelled:
                timer_.Stop();
                active_ = false;
                delete this;
                return;
            default:
                return;   // a non-terminal state on an orphan: ignore, keep waiting
            }
        }

        switch (e.GetState()) {
        case wxWebRequest::State_Completed: {
            timer_.Stop();
            active_ = false;
            const wxWebResponse& resp = e.GetResponse();
            const int status = resp.IsOk() ? resp.GetStatus() : 0;
            wxString body;
            if (stream_) {
                FlushTail();            // emit any final bytes with no trailing boundary
                body = body_;           // full accumulated text (used only on non-2xx)
            } else {
                body = resp.IsOk() ? resp.AsString() : wxString();
            }
            if (onDone_) onDone_(status, body);
            break;
        }
        case wxWebRequest::State_Unauthorized: {
            // Server demanded credentials we don't negotiate (bearer/API-key auth):
            // treat the 401 like a completed error response so AiClient::MapHttpError
            // can classify it as an Auth failure from the status + body.
            timer_.Stop();
            active_ = false;
            const wxWebResponse& resp = e.GetResponse();
            const int status = resp.IsOk() ? resp.GetStatus() : 401;
            const wxString body = resp.IsOk() ? resp.AsString() : wxString();
            if (onDone_) onDone_(status, body);
            break;
        }
        case wxWebRequest::State_Failed: {
            timer_.Stop();
            active_ = false;
            if (onErr_) onErr_(Fail::Network, e.GetErrorDescription());
            break;
        }
        case wxWebRequest::State_Cancelled: {
            timer_.Stop();
            active_ = false;
            if (timedOut_ && onErr_)
                onErr_(Fail::Timeout, wxS("request timed out"));
            // otherwise a user-initiated Cancel() → stay silent
            break;
        }
        default:
            break;
        }
    }

    void OnTimeout(wxTimerEvent&)
    {
        if (!active_)
            return;
        timedOut_ = true;
        if (request_.IsOk())
            request_.Cancel();   // → State_Cancelled → onErr(Timeout)
    }

    // Flush a trailing partial UTF-8 sequence at true end-of-stream, best effort.
    void FlushTail()
    {
        if (rawTail_.empty())
            return;
        wxString t = wxString::FromUTF8(rawTail_.data(), rawTail_.size());
        rawTail_.clear();
        if (t.empty())
            return;
        body_ += t;
        if (onData_) onData_(t);
    }

    wxWebRequest request_;
    wxTimer      timer_;

    DataCb onData_;
    DoneCb onDone_;
    ErrCb  onErr_;

    bool        stream_   = false;
    bool        active_   = false;
    bool        timedOut_ = false;
    bool        orphaned_ = false;   // owner gone; self-delete on the terminal event
    int         timeoutMs_ = 0;
    std::string rawTail_;    // incomplete trailing UTF-8 bytes awaiting continuation
    wxString    body_;       // accumulated decoded stream text (for a non-2xx body)
};

// ── AiHttp: thin forwarding shell ────────────────────────────────────────────────────
AiHttp::AiHttp() : impl_(std::make_unique<Impl>()) {}
AiHttp::~AiHttp()
{
    // Hand the handler over to itself rather than destroying it under a
    // request wx is still holding a raw pointer to. See Impl::Orphan() for the
    // crash this prevents. The release() is deliberate: ownership moves to the
    // orphan, which deletes itself when the transport is finally done with it.
    if (impl_ && impl_->Active()) {
        impl_.release()->Orphan();
        return;
    }
    // Nothing in flight — ordinary destruction, no zombie, no leak.
}

void AiHttp::Start(const HttpRequestSpec& spec, bool stream, int timeoutSec,
                   DataCb onData, DoneCb onDone, ErrCb onErr)
{
    impl_->Start(spec, stream, timeoutSec,
                 std::move(onData), std::move(onDone), std::move(onErr));
}

void AiHttp::Cancel()      { impl_->Cancel(); }
bool AiHttp::Active() const { return impl_->Active(); }

} // namespace ai
