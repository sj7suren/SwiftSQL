// DialectRegistry.cpp — the GetDialectProfile(DbType) factory (ADR-014 T6). Maps
// each DbType to a per-DbType-family singleton profile, mirroring
// CreateConnection(DbType). All five families now have real profiles (T3/T4/T5):
// MySQL (Inline family), PG / SQLite / SQL Server / Oracle (Separate family). The
// default arm keeps the factory total for any DbType added ahead of its profile.
#include "db/DialectProfile.h"

namespace db {

// Concrete family singletons — defined in their respective *Profile.cpp files.
const DialectProfile& MySqlDialectProfile();     // MySqlProfile.cpp     (MySQL/MariaDB/OceanBase)
const DialectProfile& PgDialectProfile();        // PgProfile.cpp        (PostgreSQL/KingBase)
const DialectProfile& SqliteDialectProfile();    // SqliteProfile.cpp    (SQLite)
const DialectProfile& SqlServerDialectProfile(); // SqlServerProfile.cpp (SQL Server)
const DialectProfile& OracleDialectProfile();    // OracleProfile.cpp    (Oracle/DM)

const DialectProfile& GetDialectProfile(DbType type)
{
    switch (type) {
    case DbType::MySQL:
    case DbType::MariaDB:
    case DbType::OceanBase:
        return MySqlDialectProfile();

    case DbType::PostgreSQL:
    case DbType::KingBase:
        return PgDialectProfile();

    case DbType::Sqlite:
        return SqliteDialectProfile();

    case DbType::SqlServer:
        return SqlServerDialectProfile();

    // Oracle + 达梦 DM share the Oracle DDL surface (parenthesized MODIFY, RENAME
    // COLUMN, COMMENT ON COLUMN). They get their own profiles when they diverge.
    case DbType::Oracle:
    case DbType::DM:
        return OracleDialectProfile();

    default:
        return PgDialectProfile();
    }
}

// Same family grouping as GetDialectProfile(DbType), keyed by the coarser
// Dialect enum instead — see the header comment for why this overload exists.
const DialectProfile& GetDialectProfile(Dialect d)
{
    switch (d) {
    case Dialect::MySQL:     return MySqlDialectProfile();
    case Dialect::Postgres:  return PgDialectProfile();
    case Dialect::Sqlite:    return SqliteDialectProfile();
    case Dialect::SqlServer: return SqlServerDialectProfile();
    case Dialect::Oracle:    return OracleDialectProfile();
    default:                 return PgDialectProfile();
    }
}

} // namespace db
