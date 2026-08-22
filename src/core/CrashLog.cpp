// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "core/CrashLog.h"

#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/datetime.h>

#include <mutex>
#include <exception>
#include <cstdlib>

#if defined(__WXMSW__)
#include <windows.h>
#include <dbghelp.h>
#endif

namespace core {
namespace {

std::mutex g_logMx;   // serialize appends across worker threads

// Installed once at startup (before any worker starts) → read-only from workers.
std::function<void(const wxString&, const wxString&)> g_notifier;

wxString DataDir()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir();   // …/SwiftSQL
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

wxString LogPathStr()
{
    return DataDir() + wxFileName::GetPathSeparator() + L"crash.log";
}

// Normal (non-fault) append path: free to use wx / heap. Used by worker catch
// blocks and the wxApp exception overrides.
void Append(const wxString& record)
{
    std::lock_guard<std::mutex> lk(g_logMx);
    wxFFile f(LogPathStr(), L"a");
    if (f.IsOpened())
        f.Write(wxDateTime::Now().FormatISOCombined(' ') + L"  " + record + L"\n",
                wxConvUTF8);
}

#if defined(__WXMSW__)
// Paths precomputed at Init() so the SEH filter never touches wx / the heap while
// the process is in a faulted state — it only calls raw Win32.
wchar_t g_sehLogPath[MAX_PATH * 2] = { 0 };
wchar_t g_sehDumpDir[MAX_PATH * 2] = { 0 };

void SehAppendLine(const char* text, int len)
{
    HANDLE h = CreateFileW(g_sehLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, text, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
}

void SehWriteMinidump(EXCEPTION_POINTERS* ep)
{
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t path[MAX_PATH * 2];
    wsprintfW(path, L"%scrash_%04d%02d%02d_%02d%02d%02d.dmp", g_sehDumpDir,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                      MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(hFile);
}

// Last-resort catcher for hard faults on ANY thread (access violation, stack
// overflow, illegal instruction …). C++ try/catch cannot see these; without this
// the process just disappears. Logs a line + a minidump, then lets the process
// terminate cleanly (EXECUTE_HANDLER) instead of popping the Windows WER dialog.
LONG WINAPI SehTopLevelFilter(EXCEPTION_POINTERS* ep)
{
    SYSTEMTIME st; GetLocalTime(&st);
    const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;

    // NB: wsprintf supports neither %p nor floating point — the fault address is
    // captured precisely in the minidump, so we keep only wsprintf-safe specifiers.
    char line[256];
    int n = wsprintfA(line,
        "%04d-%02d-%02d %02d:%02d:%02d  FATAL (hard fault)  code=0x%08X"
        "  thread=%lu  (see minidump for fault address + stack)\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        code, GetCurrentThreadId());
    SehAppendLine(line, n);
    SehWriteMinidump(ep);

    // Friendly notice for the unrecoverable case. wx is off-limits here (the
    // process is faulted), so use the raw Win32 message box — no heap, no CRT.
    wchar_t msg[MAX_PATH * 2 + 256];
    wsprintfW(msg,
        L"SwiftSQL 遇到严重错误，需要关闭。\n\n"
        L"你当前的操作可能未完成，但诊断信息已经保存下来，方便定位问题：\n%s\n\n"
        L"如果问题反复出现，请把该文件夹里的 crash.log 和 .dmp 文件发给我们。",
        g_sehLogPath);
    MessageBoxW(nullptr, msg, L"SwiftSQL",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif  // __WXMSW__

// Universal backstop for any uncaught C++ exception that escapes a thread we did
// NOT wrap in Guard(). std::terminate() would otherwise kill the app with no
// trace; here we recover the message, log it, show a friendly native notice, then
// exit. (Guard() is still preferred — it keeps the app running; this only catches
// what slips past.)
void OnTerminate()
{
    wxString what = L"std::terminate";
    if (std::exception_ptr ep = std::current_exception()) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { what = wxString::FromUTF8(e.what()); }
        catch (...) { what = L"unknown exception"; }
    }
    Append(L"TERMINATE (uncaught on some thread)  " + what);
#if defined(__WXMSW__)
    wchar_t msg[MAX_PATH * 2 + 512];
    wsprintfW(msg,
        L"SwiftSQL 遇到未预期的错误，需要关闭。\n\n"
        L"诊断信息已保存到：\n%s\n\n错误：%s",
        g_sehLogPath, what.wc_str());
    MessageBoxW(nullptr, msg, L"SwiftSQL",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
#endif
    std::abort();
}

} // namespace

void CrashLog::Init()
{
#if defined(__WXMSW__)
    lstrcpynW(g_sehLogPath, LogPathStr().wc_str(), MAX_PATH * 2);
    lstrcpynW(g_sehDumpDir,
              (DataDir() + wxFileName::GetPathSeparator()).wc_str(), MAX_PATH * 2);
    SetUnhandledExceptionFilter(&SehTopLevelFilter);
#endif
    std::set_terminate(&OnTerminate);
    Append(L"===== SwiftSQL session start (crash log armed) =====");
}

void CrashLog::LogException(const wxString& context, const wxString& what)
{
    Append(L"EXCEPTION [" + context + L"] " + what);
}

void CrashLog::LogMessage(const wxString& message)
{
    Append(message);
}

void CrashLog::SetNotifier(std::function<void(const wxString&, const wxString&)> notifier)
{
    g_notifier = std::move(notifier);
}

void CrashLog::Guard(const wxString& context, const std::function<void()>& fn)
{
    try {
        fn();
    }
    catch (const std::exception& e) {
        const wxString what = wxString::FromUTF8(e.what());
        LogException(context, what);
        if (g_notifier) g_notifier(context, what);
    }
    catch (...) {
        LogException(context, L"unknown exception");
        if (g_notifier) g_notifier(context, L"未知异常");
    }
}

wxString CrashLog::FilePath() { return LogPathStr(); }

} // namespace core
