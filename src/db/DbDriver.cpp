// DbDriver.cpp — protocol factory.
#include "db/DbDriver.h"

namespace db {

std::unique_ptr<IConnection> CreateMySqlConnection();
std::unique_ptr<IConnection> CreatePgConnection();
std::unique_ptr<IConnection> CreateSqliteConnection();
std::unique_ptr<IConnection> CreateSqlServerConnection();
std::unique_ptr<IConnection> CreateOracleConnection();
std::unique_ptr<IConnection> CreateDmConnection();

namespace {

// Placeholder connection for engines whose native driver isn't wired yet
// (e.g. 达梦 DM needs the proprietary DPI client library). Connect() fails with
// a clear message; everything else is a safe no-op so the UI stays stable.
class StubConnection : public IConnection {
public:
    explicit StubConnection(wxString why, Dialect d)
        : why_(std::move(why)), dialect_(d) {}

    Dialect GetDialect() const override { return dialect_; }
    bool Connect(const core::ConnectionProfile&, wxString& err) override
    { err = why_; return false; }
    void Disconnect() override {}
    bool IsConnected() const override { return false; }
    bool Execute(const wxString&, QueryResult&, wxString& err) override
    { err = why_; return false; }
    bool ListDatabases(std::vector<wxString>&, wxString& err) override
    { err = why_; return false; }
    bool ListTables(const wxString&, std::vector<TableInfo>&, wxString& err) override
    { err = why_; return false; }
    bool GetColumns(const wxString&, const wxString&, std::vector<ColumnInfo>&,
                    wxString& err) override { err = why_; return false; }
    bool GetForeignKeys(const wxString&, const wxString&, std::vector<ForeignKey>&,
                        wxString& err) override { err = why_; return false; }
    bool GetIndexes(const wxString&, const wxString&, std::vector<IndexInfo>&,
                    wxString& err) override { err = why_; return false; }
    bool GetCreateDdl(const wxString&, const wxString&, wxString&,
                      wxString& err) override { err = why_; return false; }
    wxString ServerVersion() const override { return wxString(); }

private:
    wxString why_;
    Dialect  dialect_;
};

} // namespace

std::unique_ptr<IConnection> CreateConnection(DbType type)
{
    switch (type) {
    case DbType::MySQL:
    case DbType::OceanBase:   // MySQL wire protocol (OceanBase MySQL mode)
    case DbType::MariaDB:     // MySQL wire protocol (libmariadb is MariaDB's own)
        return CreateMySqlConnection();
    case DbType::PostgreSQL:
    case DbType::KingBase:    // PostgreSQL wire protocol (KingBase / 人大金仓)
        return CreatePgConnection();
    case DbType::Sqlite:      // embedded, file-based
        return CreateSqliteConnection();
    case DbType::SqlServer:   // TDS via ODBC — driver landing in this change
        return CreateSqlServerConnection();
    case DbType::Oracle:      // Oracle Database via native OCI (Instant Client)
        return CreateOracleConnection();
    case DbType::DM:          // 达梦 — Oracle-compatible; served via ODBC (DM8 driver)
        return CreateDmConnection();
    default:
        return nullptr;
    }
}

} // namespace db
