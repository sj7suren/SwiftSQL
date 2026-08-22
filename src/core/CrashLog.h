// CrashLog.h — process-wide crash safety net.
//
// SwiftSQL runs DB work on ~11 worker threads. An uncaught exception on any of
// them would call std::terminate() and make the app vanish silently; a null
// dereference / access violation is worse still — a hard fault that C++ try/catch
// can never see. This module is the single sink all three protection layers write
// to:
//   1. worker-thread catch blocks           → LogException()
//   2. wxApp::OnExceptionInMainLoop / etc.   → LogException()
//   3. Windows top-level SEH filter (below)  → hard-fault record + minidump
//
// One human-readable log file (…/SwiftSQL/crash.log) plus a minidump per hard
// fault (…/SwiftSQL/crash_<ts>.dmp) for post-mortem debugging.
#pragma once

#include <wx/string.h>

#include <functional>
#include <thread>
#include <utility>

namespace core {

class CrashLog {
public:
    // Install the Windows top-level SEH filter and arm the log. Call once, as
    // early as possible in wxApp::OnInit (before any worker thread can start).
    static void Init();

    // Append a timestamped record. Thread-safe; safe from any worker-thread catch
    // block or the wxApp exception overrides (NOT from inside the SEH filter).
    static void LogException(const wxString& context, const wxString& what);
    static void LogMessage(const wxString& message);

    // Run `fn`; if it throws, log (context + message) and swallow so a worker
    // thread can never call std::terminate() and take the whole app down. If a
    // notifier is installed it is also invoked (for a friendly dialog). Use this
    // to wrap EVERY std::thread worker body. Dependency-injected notifier keeps
    // core UI-agnostic (the ui layer can't be a dependency of core/db/net).
    static void Guard(const wxString& context, const std::function<void()>& fn);

    // The app installs this (from main.cpp) to surface caught background-thread
    // exceptions as a friendly dialog. Optional — logging happens regardless.
    static void SetNotifier(std::function<void(const wxString& context,
                                               const wxString& what)> notifier);

    // Drop-in replacement for `std::thread(fn)` that runs the worker body inside
    // Guard(): an exception on the thread is logged (+ notified) instead of
    // calling std::terminate() and killing the app. Usage — turn
    //   worker_ = std::thread([caps]{ ... });
    // into
    //   worker_ = core::CrashLog::GuardedThread(L"context", [caps]{ ... });
    // (only the opening changes; the closing ")" is unchanged). fn may be a
    // move-only lambda (move-captures are forwarded, not copied).
    template <class Fn>
    static std::thread GuardedThread(const wxString& context, Fn&& fn)
    {
        return std::thread(
            [context, f = std::forward<Fn>(fn)]() mutable {
                Guard(context, [&]() { f(); });
            });
    }

    // …/SwiftSQL/crash.log — surfaced in error dialogs so users can find it.
    static wxString FilePath();
};

} // namespace core
