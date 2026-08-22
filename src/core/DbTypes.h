// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// DbTypes.h — supported database engines and their display metadata.
// The UI (connection dialog, sidebar, icons) is driven entirely by this table:
// adding a new engine here is the single step that lights it up everywhere.
#pragma once

#include <wx/colour.h>
#include <wx/string.h>
#include <array>

namespace db {

enum class DbType {
    MySQL = 0,
    PostgreSQL,
    OceanBase,
    KingBase,     // 人大金仓 — PostgreSQL wire protocol
    DM,           // 达梦 — proprietary protocol (driver pending)
    Sqlite,       // SQLite — embedded, file-based (no server)
    MariaDB,      // MariaDB — MySQL wire protocol (libmariadb)
    SqlServer,    // Microsoft SQL Server — TDS via ODBC
    Oracle,       // Oracle Database — via ODBC (Oracle Instant Client / ODBC driver)
    _Count
};

struct DbTypeInfo {
    DbType       type;
    const wchar_t* name;        // display name
    const wchar_t* shortMark;   // 1-2 letter mark drawn on the brand icon
    int          defaultPort;
    wxColour     brandColor;    // icon / accent color
    const wchar_t* defaultUser;
};

inline const std::array<DbTypeInfo, static_cast<size_t>(DbType::_Count)>& AllDbTypes()
{
    static const std::array<DbTypeInfo, static_cast<size_t>(DbType::_Count)> kTypes = {{
        // MySQL — official palette leans teal/orange; orange reads best at 16px
        { DbType::MySQL,      L"MySQL",      L"My", 3306, wxColour(0xE9, 0x7B, 0x00), L"root"     },
        // PostgreSQL — the classic slate blue
        { DbType::PostgreSQL, L"PostgreSQL", L"Pg", 5432, wxColour(0x33, 0x67, 0x91), L"postgres" },
        // OceanBase — bright azure; MySQL-compatible protocol, port 2881
        { DbType::OceanBase,  L"OceanBase",  L"Ob", 2881, wxColour(0x00, 0x7F, 0xFF), L"root"     },
        // 人大金仓 KingBase — PostgreSQL-compatible; default port 54321
        { DbType::KingBase,   L"KingBase",   L"KES", 54321, wxColour(0xD7, 0x00, 0x0F), L"system"  },
        // 达梦 DM — proprietary protocol; default port 5236
        { DbType::DM,         L"达梦 DM",     L"DM",  5236,  wxColour(0xC8, 0x16, 0x1E), L"SYSDBA"  },
        // SQLite — embedded/file-based; no host/port/user, so port 0 & no default user
        { DbType::Sqlite,     L"SQLite",     L"SL",  0,     wxColour(0x0F, 0x80, 0xCC), L""        },
        // MariaDB — MySQL-compatible; libmariadb driver; default port 3306
        { DbType::MariaDB,    L"MariaDB",    L"Ma",  3306,  wxColour(0x1F, 0x30, 0x5A), L"root"     },
        // Microsoft SQL Server — TDS via ODBC; default port 1433
        { DbType::SqlServer,  L"SQL Server", L"MS",  1433,  wxColour(0xA9, 0x1D, 0x22), L"sa"       },
        // Oracle Database — via ODBC; default listener port 1521; Oracle brand red
        { DbType::Oracle,     L"Oracle",     L"Or",  1521,  wxColour(0xC7, 0x46, 0x34), L"system"   },
    }};
    return kTypes;
}

inline const DbTypeInfo& InfoOf(DbType t)
{
    return AllDbTypes()[static_cast<size_t>(t)];
}

} // namespace db
