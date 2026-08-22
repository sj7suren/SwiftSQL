// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// StmtGuard.h — RAII closer for the raw ODBC statement handle held across a
// fetch/dump loop. Moving the release into a destructor makes those loops
// exception-safe: if a row's SQLGetData / push_back / emit / sink throws
// mid-loop, the SQLHSTMT is still freed (and the connection's in-flight
// statement pointer cleared under the same mutex Cancel() uses) instead of
// leaking. Normal-path release semantics are unchanged — the guard simply runs
// at scope exit where the manual ClearStmt()/free used to.
//
// Only the ODBC (SQLHSTMT) guard lives here because it is shared verbatim by the
// SQL Server and DM (Oracle-compatible) ODBC drivers and needs the Cancel()-mutex
// dance. The single-vendor handle guards (MYSQL_RES / PGresult / sqlite3_stmt /
// OCIStmt) stay local to their own driver .cpp so this header never drags a
// vendor SDK header into anything else.
#pragma once

#include <mutex>

#include <windows.h>
#include <sql.h>

namespace db {

// Frees a SQLHSTMT at scope exit, first clearing the connection's in-flight
// statement pointer under the same mutex Cancel() takes — so Cancel() can never
// SQLCancel a handle this guard is about to SQLFreeHandle.
class OdbcStmtGuard {
public:
    OdbcStmtGuard(SQLHSTMT stmt, std::mutex& mx, SQLHSTMT& current)
        : stmt_(stmt), mx_(mx), current_(current) {}
    ~OdbcStmtGuard()
    {
        { std::lock_guard<std::mutex> lk(mx_); current_ = nullptr; }
        if (stmt_) SQLFreeHandle(SQL_HANDLE_STMT, stmt_);
    }
    OdbcStmtGuard(const OdbcStmtGuard&) = delete;
    OdbcStmtGuard& operator=(const OdbcStmtGuard&) = delete;

private:
    SQLHSTMT    stmt_;
    std::mutex& mx_;
    SQLHSTMT&   current_;
};

} // namespace db
