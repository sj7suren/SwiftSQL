// OciLoader.h — runtime dynamic loader for the Oracle Call Interface (OCI).
//
// SwiftSQL no longer links oci.lib at build time. Instead the OCI functions the
// Oracle driver needs are resolved from oci.dll at first use via LoadLibrary +
// GetProcAddress and cached in a single OciApi table. This lets the whole app
// build and run on a host with *no* Oracle client installed; Oracle simply
// reports "client library not found" until the user drops an Instant Client next
// to the exe or configures a path in Preferences.
//
// We still #include <oci.h> for the OCI types/structs/constants (OCIEnv, sword,
// ub4, SQLT_STR, …) — only the *functions* are late-bound. Never call an OCI
// function directly (that reintroduces an oci.lib symbol dependency); always go
// through the OciApi pointers returned by GetOciApi().
#pragma once

#include <oci.h>
#include <wx/string.h>

// Vendor-free config surface (SetOciLibraryPath / OciLibraryPath / OciAvailable)
// so UI & startup code can configure the loader without pulling in <oci.h>.
#include "db/OciConfig.h"

namespace db {

// ---- function-pointer typedefs (signatures copied from ociap.h) -------------
typedef sword (*OCIEnvNlsCreate_t)(OCIEnv**, ub4, void*,
                                   void* (*)(void*, size_t),
                                   void* (*)(void*, void*, size_t),
                                   void (*)(void*, void*),
                                   size_t, void**, ub2, ub2);
typedef sword (*OCIHandleAlloc_t)(const void*, void**, const ub4, const size_t,
                                  void**);
typedef sword (*OCIHandleFree_t)(void*, const ub4);
typedef sword (*OCILogon2_t)(OCIEnv*, OCIError*, OCISvcCtx**, const OraText*,
                             ub4, const OraText*, ub4, const OraText*, ub4, ub4);
typedef sword (*OCILogoff_t)(OCISvcCtx*, OCIError*);
typedef sword (*OCIBreak_t)(void*, OCIError*);
typedef sword (*OCIAttrGet_t)(const void*, ub4, void*, ub4*, ub4, OCIError*);
typedef sword (*OCIStmtPrepare2_t)(OCISvcCtx*, OCIStmt**, OCIError*,
                                   const OraText*, ub4, const OraText*, ub4,
                                   ub4, ub4);
typedef sword (*OCIStmtRelease_t)(OCIStmt*, OCIError*, const OraText*, ub4, ub4);
typedef sword (*OCIStmtExecute_t)(OCISvcCtx*, OCIStmt*, OCIError*, ub4, ub4,
                                  const OCISnapshot*, OCISnapshot*, ub4);
typedef sword (*OCIParamGet_t)(const void*, ub4, OCIError*, void**, ub4);
typedef sword (*OCIDefineByPos_t)(OCIStmt*, OCIDefine**, OCIError*, ub4, void*,
                                  sb4, ub2, void*, ub2*, ub2*, ub4);
typedef sword (*OCIStmtFetch2_t)(OCIStmt*, OCIError*, ub4, ub2, sb4, ub4);
typedef sword (*OCIErrorGet_t)(void*, ub4, OraText*, sb4*, OraText*, ub4, ub4);

// Resolved OCI entry points. Every pointer is non-null once GetOciApi() returns
// a valid table (partial loads are rejected — see OciLoader.cpp).
struct OciApi {
    OCIEnvNlsCreate_t  OCIEnvNlsCreate  = nullptr;
    OCIHandleAlloc_t   OCIHandleAlloc   = nullptr;
    OCIHandleFree_t    OCIHandleFree    = nullptr;
    OCILogon2_t        OCILogon2        = nullptr;
    OCILogoff_t        OCILogoff        = nullptr;
    OCIBreak_t         OCIBreak         = nullptr;
    OCIAttrGet_t       OCIAttrGet       = nullptr;
    OCIStmtPrepare2_t  OCIStmtPrepare2  = nullptr;
    OCIStmtRelease_t   OCIStmtRelease   = nullptr;
    OCIStmtExecute_t   OCIStmtExecute   = nullptr;
    OCIParamGet_t      OCIParamGet      = nullptr;
    OCIDefineByPos_t   OCIDefineByPos   = nullptr;
    OCIStmtFetch2_t    OCIStmtFetch2    = nullptr;
    OCIErrorGet_t      OCIErrorGet      = nullptr;
};

// Resolve (once, cached) and return the OCI entry-point table. Thread-safe.
// On failure returns nullptr and sets `err` to a user-facing Chinese message.
const OciApi* GetOciApi(wxString& err);

// ---- configuration -----------------------------------------------------------
// SetOciLibraryPath / OciLibraryPath / OciAvailable are declared in the
// vendor-free header db/OciConfig.h (included above) so UI code can use them
// without <oci.h>.

} // namespace db
