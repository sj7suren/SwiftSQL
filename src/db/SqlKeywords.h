// SqlKeywords.h — dialect-specific SQL keyword and function lists used for
// editor syntax highlighting and autocompletion. SQL-language knowledge belongs
// in the data-access layer (db), keyed by Dialect.
#pragma once

#include <wx/string.h>
#include <vector>
#include "db/DbDriver.h"   // db::Dialect

namespace db {

// Reserved words (SELECT, JOIN, dialect types like AUTO_INCREMENT / SERIAL…).
const std::vector<wxString>& Keywords(Dialect d);

// Built-in functions (GROUP_CONCAT / string_agg / to_char…).
const std::vector<wxString>& Functions(Dialect d);

// Signature hint for a function (upper-cased name), for editor call tips.
// Empty if unknown. e.g. FunctionSignature(_, "DATE_FORMAT") → "DATE_FORMAT(date, format)".
wxString FunctionSignature(Dialect d, const wxString& nameUpper);

} // namespace db
