// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// main.cpp — SwiftSQL application entry point.
#include <wx/wx.h>
#include <wx/image.h>
#include <wx/snglinst.h>   // wxSingleInstanceChecker
#include <wx/ipc.h>        // wxServer / wxClient / wxConnection (DDE on Windows)
#include <wx/utils.h>      // wxGetUserId

#include <exception>
#include <stdexcept>
#include <thread>

#include "core/CrashLog.h"
#include "core/Settings.h"
#include "db/OciConfig.h"
#include "ui/CrashDialog.h"
#include "ui/MainFrame.h"
#include "ui/SplashScreen.h"

// ---------------------------------------------------------------------------
// Single-instance IPC. Kept file-scope (no port, uses DDE on Windows → no
// firewall prompt). A second launch connects to the first and asks it to bring
// its window to the front, then exits without opening another window.
namespace {
const wxString kIpcService = wxT("SwiftSQL.singleinstance");
const wxString kIpcTopic   = wxT("raise");
}  // namespace

class SwiftSqlApp : public wxApp {
public:
    bool OnInit() override;
    int  OnExit() override;

    // Bring the already-running instance's main window to the foreground.
    void RaiseMainWindow();

    // Called when an exception escapes an event handler on the main loop. Log it
    // and keep the app alive (return true) so one bad handler doesn't take down
    // the whole program — the user sees an error instead of a vanished window.
    bool OnExceptionInMainLoop() override
    {
        wxString what = L"unknown exception";
        try { throw; }
        catch (const std::exception& e) { what = wxString::FromUTF8(e.what()); }
        catch (...) {}
        core::CrashLog::LogException(L"MainLoop", what);
        ui::ShowCrashDialog(false, L"主循环事件处理", what);
        return true;   // continue the main loop
    }

    // Last C++-level net for an exception that escaped even OnExceptionInMainLoop
    // (e.g. thrown during shutdown). Log before the runtime unwinds.
    void OnUnhandledException() override
    {
        wxString what = L"unknown exception";
        try { throw; }
        catch (const std::exception& e) { what = wxString::FromUTF8(e.what()); }
        catch (...) {}
        core::CrashLog::LogException(L"Unhandled", what);
    }

private:
    wxSingleInstanceChecker* checker_   = nullptr;
    wxServer*                ipcServer_ = nullptr;
    ui::MainFrame*           frame_     = nullptr;
};

namespace {

// Diagnostic hook (mirrors SWIFTSQL_AUTOSTOP / SWIFTSQL_AUTOFORMAT). Set the env
// var SWIFTSQL_TESTCRASH to one of the modes below to deliberately trigger a crash
// path and watch the friendly dialog + crash.log in action. No effect when unset —
// a verification aid, safe to leave in.
//   mainloop  → exception from a main-loop event handler → OnExceptionInMainLoop (recoverable)
//   worker    → exception on a guarded worker thread     → CrashLog::Guard → friendly dialog (recoverable)
//   segv      → null-pointer write (hard fault)          → SEH filter → log + minidump + native notice (fatal)
//   terminate → exception on an UNguarded thread         → std::set_terminate backstop (fatal)
void InstallTestCrashHook()
{
    wxString mode;
    if (!wxGetEnv(L"SWIFTSQL_TESTCRASH", &mode) || mode.IsEmpty())
        return;
    mode.MakeLower();
    core::CrashLog::LogMessage(L"SWIFTSQL_TESTCRASH armed: " + mode);

    if (mode == L"mainloop") {
        wxTheApp->CallAfter([] {
            throw std::runtime_error("SWIFTSQL_TESTCRASH=mainloop (deliberate test exception)");
        });
    } else if (mode == L"worker") {
        core::CrashLog::GuardedThread(L"测试后台任务", [] {
            throw std::runtime_error("SWIFTSQL_TESTCRASH=worker (deliberate test exception)");
        }).detach();
    } else if (mode == L"segv") {
        wxTheApp->CallAfter([] {
            volatile int* p = nullptr;
            *p = 42;   // deliberate null-pointer write → hard fault
        });
    } else if (mode == L"terminate") {
        std::thread([] {
            throw std::runtime_error("SWIFTSQL_TESTCRASH=terminate (deliberate test exception)");
        }).detach();
    }
}

// Server side of the single-instance IPC: any Execute() from a second launch
// bounces (on the GUI thread) to raising the existing main window.
class RaiseConnection : public wxConnection {
public:
    bool OnExecute(const wxString&, const void*, size_t, wxIPCFormat) override
    {
        wxTheApp->CallAfter([] {
            if (auto* app = static_cast<SwiftSqlApp*>(wxTheApp))
                app->RaiseMainWindow();
        });
        return true;
    }
};

class RaiseServer : public wxServer {
public:
    wxConnectionBase* OnAcceptConnection(const wxString& topic) override
    {
        return topic == kIpcTopic ? new RaiseConnection() : nullptr;
    }
};

}  // namespace

bool SwiftSqlApp::OnInit()
{
    // Arm the crash safety net before anything else can start a worker thread
    // or touch a driver: installs the top-level SEH filter + opens the log.
    core::CrashLog::Init();

    // Let background-thread exceptions caught by CrashLog::Guard surface as a
    // friendly dialog on the GUI thread (kept UI-agnostic in core via this hook).
    core::CrashLog::SetNotifier([](const wxString& ctx, const wxString& what) {
        if (wxTheApp)
            wxTheApp->CallAfter([ctx, what] { ui::ShowCrashDialog(false, ctx, what); });
    });

    try {
        if (!wxApp::OnInit())
            return false;

        SetAppName(L"SwiftSQL");

        // ---- single instance ---------------------------------------------
        // If SwiftSQL is already running, ask that instance to show its window
        // and exit this one — no duplicate windows. Per-user name so different
        // users on one machine each get their own instance.
        checker_ = new wxSingleInstanceChecker(wxString(L"SwiftSQL-") + wxGetUserId());
        if (checker_->IsAnotherRunning()) {
            wxLogNull noLog;   // suppress "can't connect" noise if the server is mid-setup
            wxClient client;
            wxConnectionBase* conn =
                client.MakeConnection(L"localhost", kIpcService, kIpcTopic);
            if (conn) {
                conn->Execute(L"show");
                conn->Disconnect();
                delete conn;
            }
            return false;      // do NOT open a second main window
        }
        // First instance: stand up the IPC server so later launches can reach us.
        ipcServer_ = new RaiseServer();
        ipcServer_->Create(kIpcService);

        // Register the PNG image handler once, before any window is built.
        // The About/Donate dialog loads the embedded Alipay QR from an RC PNG
        // resource (wxBITMAP_TYPE_PNG_RESOURCE); without this handler the
        // bitmap would silently come back not-Ok. Guard against a double add.
        if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
            wxImage::AddHandler(new wxPNGHandler);

        // Re-hydrate the persisted Oracle OCI library path before any Oracle
        // connection so the loader resolves the user-configured oci.dll. Empty
        // (unconfigured) => the loader falls back to exe dir / PATH.
        db::SetOciLibraryPath(
            core::Settings::ReadString(core::keys::kOciLibraryPath));

        // Show the animated splash first; build MainFrame during the splash
        // animation but keep it hidden until the splash finishes, so the user
        // always sees the launch screen before the main window appears.
        auto* splash = new ui::SplashScreen();
        splash->Show(true);
        splash->Raise();
        splash->Update();

        frame_ = new ui::MainFrame();   // constructed now, revealed later
        auto* f = frame_;
        splash->SetOnFinished([f] {
            f->Show(true);
            f->Raise();
        });

        InstallTestCrashHook();   // no-op unless SWIFTSQL_TESTCRASH is set
        return true;
    }
    catch (const std::exception& e) {
        const wxString what = wxString::FromUTF8(e.what());
        core::CrashLog::LogException(L"OnInit", what);
        ui::ShowCrashDialog(true, L"OnInit（启动）", what);
        return false;
    }
    catch (...) {
        core::CrashLog::LogException(L"OnInit", L"unknown exception");
        ui::ShowCrashDialog(true, L"OnInit（启动）", L"未知异常");
        return false;
    }
}

void SwiftSqlApp::RaiseMainWindow()
{
    if (!frame_)
        return;
    if (frame_->IsIconized())
        frame_->Iconize(false);   // restore if minimized
    frame_->Show(true);
    frame_->Raise();
    frame_->RequestUserAttention();
}

int SwiftSqlApp::OnExit()
{
    delete ipcServer_;
    ipcServer_ = nullptr;
    delete checker_;
    checker_ = nullptr;
    return wxApp::OnExit();
}

wxIMPLEMENT_APP(SwiftSqlApp);
