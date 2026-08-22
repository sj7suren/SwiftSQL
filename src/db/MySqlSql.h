// MySqlSql.h — pure SQL-rendering helpers for the MySQL driver. Dialect-specific
// but connection-free: they turn normalized schema value objects (SchemaModel.h)
// into MySQL DDL text and vice-versa. Split out of MySqlDriver.cpp so that file
// stays within the 1000-line charter limit; RenderSchemaChange / GetTableSchema
// call into here. Nothing in this unit touches a MYSQL* handle.
#pragma once

#include <wx/string.h>
#include <vector>

#include "db/DbDriver.h"   // Dialect / QuoteIdent (+ transitively SchemaModel.h)

namespace db {
namespace detail {

// MySQL data_type / column_type → normalized ColKind. Unmapped → Other (diff then
// falls back to rawType). column_type carries the display width so tinyint(1) can
// be read as boolean.
ColKind MapColKind(const wxString& dataType, const wxString& columnType);

// Comma-joined column list → trimmed vector (GROUP_CONCAT / GetIndexes text).
std::vector<wxString> SplitCsv(const wxString& s);

// Backtick-quote + comma-join a column-name list.
wxString JoinIdents(const std::vector<wxString>& cols);

// One column's DEFAULT clause value. Bare for numerics / NULL / function-like
// expressions (CURRENT_TIMESTAMP, now()); quoted+escaped otherwise.
wxString RenderDefault(const wxString& v);

// `col` <rawType> [NOT NULL] [AUTO_INCREMENT] [DEFAULT …] — rawType is the
// engine-native column_type captured by GetTableSchema, so it round-trips.
wxString ColumnDef(const NormColumn& c);

// [UNIQUE] INDEX `name` (`c1`, `c2`) — inline index definition.
wxString IndexDef(const NormIndex& idx);

// [CONSTRAINT `name`] FOREIGN KEY (…) REFERENCES `t` (…) [ON DELETE/UPDATE …].
wxString FkDef(const NormForeignKey& fk);

// Whole-table CREATE from a normalized source schema (createTable path).
wxString RenderCreateTable(const TableSchema& s);

} // namespace detail
} // namespace db
