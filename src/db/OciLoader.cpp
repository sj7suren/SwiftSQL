// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// OciLoader.cpp — see OciLoader.h. Late-binds oci.dll at first use.
//
// Resolution order (first that loads wins):
//   1. the explicit path set via SetOciLibraryPath (Preferences) — a full
//      oci.dll path or the directory containing it;
//   2. oci.dll sitting directly next to SwiftSQL.exe (Instant Client dropped in
//      the program directory);
//   3. bare LoadLibraryW(L"oci.dll") — lets the OS PATH find a system install.
//
// Full-path candidates use LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH) so
// oci.dll's siblings (oraociei*.dll, oci*.dll, …) resolve from the same folder.
//
// HONEST GATE: the *loading* code path (LoadLibrary/GetProcAddress) and any real
// Oracle connection are unverified on this build host — there is no oci.dll here.
// Only compilation + the "no oci.lib link" gate are verified.
// <windows.h> must precede oci.h (pulled in by OciLoader.h): both define the
// `boolean`/`BOOLEAN` types, and Windows' rpcndr.h wins only if seen first.
#include <windows.h>

#include "db/OciLoader.h"

#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <mutex>

namespace db {
namespace {

std::mutex g_mx;
wxString   g_configPath;          // explicit path/dir from Preferences (may be empty)
HMODULE    g_dll     = nullptr;   // loaded oci.dll (kept for process lifetime)
OciApi     g_api;                 // resolved entry points (valid iff g_loaded)
bool       g_loaded  = false;     // true once g_api is fully populated

// Directory that holds SwiftSQL.exe (no trailing separator).
wxString ExeDir()
{
    wxFileName fn(wxStandardPaths::Get().GetExecutablePath());
    return fn.GetPath();
}

// Turn a configured path/dir into a concrete oci.dll path. If it already names a
// .dll we take it verbatim; otherwise we treat it as a directory and append.
wxString ConfigDllPath()
{
    if (g_configPath.IsEmpty()) return wxString();
    wxFileName fn(g_configPath);
    if (g_configPath.Lower().EndsWith(L".dll"))
        return fn.GetFullPath();
    fn.AssignDir(g_configPath);
    fn.SetFullName(L"oci.dll");
    return fn.GetFullPath();
}

// Try to load one absolute candidate with altered search path (siblings resolve).
HMODULE TryLoadFull(const wxString& fullPath)
{
    if (fullPath.IsEmpty() || !wxFileName::FileExists(fullPath)) return nullptr;
    return ::LoadLibraryExW(fullPath.wc_str(), nullptr,
                            LOAD_WITH_ALTERED_SEARCH_PATH);
}

// Populate every OciApi pointer from `h`; on any miss set `missing` and fail.
bool ResolveAll(HMODULE h, OciApi& api, wxString& missing)
{
#define OCI_BIND(name)                                                          \
    api.name = reinterpret_cast<name##_t>(                                      \
        reinterpret_cast<void*>(::GetProcAddress(h, #name)));                   \
    if (!api.name) { missing = L#name; return false; }

    OCI_BIND(OCIEnvNlsCreate)
    OCI_BIND(OCIHandleAlloc)
    OCI_BIND(OCIHandleFree)
    OCI_BIND(OCILogon2)
    OCI_BIND(OCILogoff)
    OCI_BIND(OCIBreak)
    OCI_BIND(OCIAttrGet)
    OCI_BIND(OCIStmtPrepare2)
    OCI_BIND(OCIStmtRelease)
    OCI_BIND(OCIStmtExecute)
    OCI_BIND(OCIParamGet)
    OCI_BIND(OCIDefineByPos)
    OCI_BIND(OCIStmtFetch2)
    OCI_BIND(OCIErrorGet)
#undef OCI_BIND
    return true;
}

// Walk the resolution order and return the first module that loads.
HMODULE LoadOciDll()
{
    if (HMODULE h = TryLoadFull(ConfigDllPath())) return h;

    wxFileName exeOci(ExeDir(), L"oci.dll");
    if (HMODULE h = TryLoadFull(exeOci.GetFullPath())) return h;

    return ::LoadLibraryW(L"oci.dll");   // PATH fallback
}

const wchar_t kNotFound[] =
    L"未找到 Oracle 客户端库(oci.dll)。请在 偏好设置 中配置 OCI 动态库路径,"
    L"或将 Oracle Instant Client 放到程序目录。";

} // namespace

const OciApi* GetOciApi(wxString& err)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_loaded) return &g_api;

    HMODULE h = LoadOciDll();
    if (!h) { err = kNotFound; return nullptr; }

    OciApi api;
    wxString missing;
    if (!ResolveAll(h, api, missing)) {
        ::FreeLibrary(h);
        err = wxString::Format(
            L"Oracle 客户端库(oci.dll)缺少必要的函数: %s。请检查 Instant Client 版本。",
            missing);
        return nullptr;
    }

    g_dll    = h;
    g_api    = api;
    g_loaded = true;
    return &g_api;
}

void SetOciLibraryPath(const wxString& pathOrDir)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_configPath == pathOrDir) return;
    g_configPath = pathOrDir;
    // Allow the next connection to re-resolve. We deliberately do NOT FreeLibrary
    // the previously loaded oci.dll here.
    //
    // KNOWN, ACCEPTED TRADE-OFF (leak by design): switching the OCI path drops our
    // only handle to the old module (g_dll = nullptr) without releasing its
    // refcount, so that module stays mapped for the rest of the process — one
    // leaked HMODULE per path change. This is intentional: a live
    // OracleOciConnection may still be executing against the old oci.dll on another
    // thread, and there is no safe, cheap way from here to prove no connection
    // still references it (OCI hands us no such refcount, and connections don't
    // register with this loader). FreeLibrary while a connection is mid-call would
    // unmap code under it → crash. We trade a bounded handle leak (path changes are
    // rare, user-driven Preferences actions) for guaranteed no use-after-unload.
    // If this ever needs fixing, add a connection refcount here and FreeLibrary
    // only when it reaches zero — do not blind-FreeLibrary.
    g_loaded = false;
    g_dll    = nullptr;
    g_api    = OciApi{};
}

wxString OciLibraryPath()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_configPath;
}

bool OciAvailable(wxString& resolvedPath)
{
    wxString err;
    if (!GetOciApi(err)) { resolvedPath.clear(); return false; }

    std::lock_guard<std::mutex> lk(g_mx);
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = g_dll ? ::GetModuleFileNameW(g_dll, buf,
                                           sizeof(buf) / sizeof(buf[0]))
                    : 0;
    resolvedPath = (n > 0) ? wxString(buf, n) : wxString(L"oci.dll");
    return true;
}

} // namespace db
