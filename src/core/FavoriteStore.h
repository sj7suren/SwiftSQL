// FavoriteStore.h — persisted favourite (saved) SQL snippets. Stored in
// %APPDATA%/SwiftSQL/favorites.ini; the SQL body is base64-encoded so multi-line
// queries survive the INI format.
#pragma once

#include <wx/string.h>
#include <vector>

namespace core {

struct Favorite {
    wxString name;
    wxString sql;
};

class FavoriteStore {
public:
    static std::vector<Favorite> LoadAll();
    static void Save(const Favorite& f);      // dedupe by name (update in place)
    static void Remove(const wxString& name);
};

} // namespace core
