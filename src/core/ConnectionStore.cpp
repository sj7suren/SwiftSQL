// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "core/ConnectionStore.h"

#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <memory>

#include "core/Secret.h"

namespace core {
namespace {

wxString StorePath()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir();  // …/SwiftSQL
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir + wxFileName::GetPathSeparator() + L"connections.ini";
}

const wchar_t* TypeToStr(db::DbType t)
{
    switch (t) {
    case db::DbType::PostgreSQL: return L"postgresql";
    case db::DbType::OceanBase:  return L"oceanbase";
    case db::DbType::KingBase:   return L"kingbase";
    case db::DbType::DM:         return L"dm";
    case db::DbType::Sqlite:     return L"sqlite";
    case db::DbType::MariaDB:    return L"mariadb";
    case db::DbType::SqlServer:  return L"sqlserver";
    case db::DbType::Oracle:     return L"oracle";
    default:                     return L"mysql";
    }
}

db::DbType StrToType(const wxString& s)
{
    if (s == L"postgresql") return db::DbType::PostgreSQL;
    if (s == L"oceanbase")  return db::DbType::OceanBase;
    if (s == L"kingbase")   return db::DbType::KingBase;
    if (s == L"dm")         return db::DbType::DM;
    if (s == L"sqlite")     return db::DbType::Sqlite;
    if (s == L"mariadb")    return db::DbType::MariaDB;
    if (s == L"sqlserver")  return db::DbType::SqlServer;
    if (s == L"oracle")     return db::DbType::Oracle;
    return db::DbType::MySQL;
}

// A profile name may contain characters that aren't valid ini group names;
// group by index and store the name as a value instead.
std::unique_ptr<wxFileConfig> Open()
{
    return std::make_unique<wxFileConfig>(
        wxEmptyString, wxEmptyString, StorePath(), wxEmptyString,
        wxCONFIG_USE_LOCAL_FILE);
}

} // namespace

wxString ConnectionStore::FilePath() { return StorePath(); }

std::vector<ConnectionProfile> ConnectionStore::LoadAll()
{
    std::vector<ConnectionProfile> out;
    auto cfg = Open();

    wxString group;
    long idx = 0;
    cfg->SetPath(L"/");
    bool more = cfg->GetFirstGroup(group, idx);
    while (more) {
        cfg->SetPath(L"/" + group);
        ConnectionProfile p;
        p.name = cfg->Read(L"name", group);
        p.type = StrToType(cfg->Read(L"type", L"mysql"));
        p.host = cfg->Read(L"host", L"127.0.0.1");
        long port = 3306; cfg->Read(L"port", &port, 3306); p.port = static_cast<int>(port);
        p.user = cfg->Read(L"user", wxEmptyString);
        p.password = DecryptSecret(cfg->Read(L"password", wxEmptyString));
        p.database = cfg->Read(L"database", wxEmptyString);
        p.savePassword = cfg->ReadBool(L"savePassword", true);
        p.color = cfg->Read(L"color", wxEmptyString);
        // advanced
        p.charset = cfg->Read(L"charset", wxEmptyString);
        long to = 8; cfg->Read(L"connectTimeout", &to, 8); p.connectTimeout = static_cast<int>(to);
        long rw = 0; cfg->Read(L"readWriteTimeout", &rw, 0); p.readWriteTimeout = static_cast<int>(rw);
        p.keepalive = cfg->ReadBool(L"keepalive", true);
        long ki = 30; cfg->Read(L"keepaliveInterval", &ki, 30); p.keepaliveInterval = static_cast<int>(ki);
        p.autoReconnect = cfg->ReadBool(L"autoReconnect", false);
        p.compression = cfg->ReadBool(L"compression", false);
        p.timezone = cfg->Read(L"timezone", wxEmptyString);
        p.appName = cfg->Read(L"appName", wxEmptyString);
        p.initCommands = cfg->Read(L"initCommands", wxEmptyString);
        p.socketFile = cfg->Read(L"socketFile", wxEmptyString);
        p.defaultSchema = cfg->Read(L"defaultSchema", wxEmptyString);
        p.readOnly = cfg->ReadBool(L"readOnly", false);
        p.isolationLevel = cfg->Read(L"isolationLevel", wxEmptyString);
        // databases
        p.dbFilter = cfg->Read(L"dbFilter", wxEmptyString);
        long dlm = 0; cfg->Read(L"dbListMode", &dlm, 0);
        p.dbListMode = static_cast<DbListMode>(dlm);
        p.showSystemDbs = cfg->ReadBool(L"showSystemDbs", false);
        p.autoConnectOnStartup = cfg->ReadBool(L"autoConnect", false);
        // ssl
        p.sslEnabled = cfg->ReadBool(L"sslEnabled", false);
        p.sslMode = cfg->Read(L"sslMode", wxEmptyString);
        long sa = 0; cfg->Read(L"sslAuth", &sa, 0); p.sslAuth = static_cast<SslAuth>(sa);
        p.sslCaCert = cfg->Read(L"sslCa", wxEmptyString);
        p.sslClientCert = cfg->Read(L"sslCert", wxEmptyString);
        p.sslClientKey = cfg->Read(L"sslKey", wxEmptyString);
        p.sslClientKeyPassword = DecryptSecret(cfg->Read(L"sslKeyPass", wxEmptyString));
        p.sslVerifyServerCert = cfg->ReadBool(L"sslVerify", false);
        p.sslVerifyHostname = cfg->ReadBool(L"sslVerifyHost", false);
        p.sslCipher = cfg->Read(L"sslCipher", wxEmptyString);
        p.sslMinVersion = cfg->Read(L"sslMinVersion", wxEmptyString);
        // ssh
        p.sshEnabled = cfg->ReadBool(L"sshEnabled", false);
        p.sshHost = cfg->Read(L"sshHost", wxEmptyString);
        long sp = 22; cfg->Read(L"sshPort", &sp, 22); p.sshPort = static_cast<int>(sp);
        p.sshUser = cfg->Read(L"sshUser", wxEmptyString);
        long ssa = 0; cfg->Read(L"sshAuth", &ssa, 0); p.sshAuth = static_cast<SshAuth>(ssa);
        p.sshPassword = DecryptSecret(cfg->Read(L"sshPassword", wxEmptyString));
        p.sshKeyFile = cfg->Read(L"sshKeyFile", wxEmptyString);
        p.sshKeyPassword = DecryptSecret(cfg->Read(L"sshKeyPass", wxEmptyString));
        long slp = 0; cfg->Read(L"sshLocalPort", &slp, 0); p.sshLocalPort = static_cast<int>(slp);
        long ska = 30; cfg->Read(L"sshKeepalive", &ska, 30); p.sshKeepalive = static_cast<int>(ska);
        p.sshCompression = cfg->ReadBool(L"sshCompression", false);
        // http
        p.httpEnabled = cfg->ReadBool(L"httpEnabled", false);
        p.httpUrl = cfg->Read(L"httpUrl", wxEmptyString);
        long ha = 0; cfg->Read(L"httpAuth", &ha, 0); p.httpAuth = static_cast<HttpAuth>(ha);
        p.httpUser = cfg->Read(L"httpUser", wxEmptyString);
        p.httpPassword = DecryptSecret(cfg->Read(L"httpPassword", wxEmptyString));
        p.httpVerifyCert = cfg->ReadBool(L"httpVerifyCert", true);
        cfg->SetPath(L"/");
        out.push_back(std::move(p));
        more = cfg->GetNextGroup(group, idx);
    }
    return out;
}

void ConnectionStore::Save(const ConnectionProfile& p)
{
    if (p.name.IsEmpty()) return;
    auto cfg = Open();

    // find an existing group whose name matches, else allocate a new index
    wxString targetGroup;
    {
        wxString group; long idx = 0;
        cfg->SetPath(L"/");
        bool more = cfg->GetFirstGroup(group, idx);
        long maxN = -1;
        while (more) {
            cfg->SetPath(L"/" + group);
            if (cfg->Read(L"name", wxEmptyString) == p.name) targetGroup = group;
            cfg->SetPath(L"/");
            long n = 0;
            if (group.StartsWith(L"conn") && group.Mid(4).ToLong(&n)) maxN = std::max(maxN, n);
            more = cfg->GetNextGroup(group, idx);
        }
        if (targetGroup.IsEmpty())
            targetGroup = wxString::Format(L"conn%ld", maxN + 1);
    }

    cfg->SetPath(L"/" + targetGroup);
    cfg->Write(L"name", p.name);
    cfg->Write(L"type", wxString(TypeToStr(p.type)));
    cfg->Write(L"host", p.host);
    cfg->Write(L"port", static_cast<long>(p.port));
    cfg->Write(L"user", p.user);
    cfg->Write(L"password", EncryptSecret(p.savePassword ? p.password : wxString()));
    cfg->Write(L"database", p.database);
    cfg->Write(L"savePassword", p.savePassword);
    cfg->Write(L"color", p.color);
    // advanced
    cfg->Write(L"charset", p.charset);
    cfg->Write(L"connectTimeout", static_cast<long>(p.connectTimeout));
    cfg->Write(L"readWriteTimeout", static_cast<long>(p.readWriteTimeout));
    cfg->Write(L"keepalive", p.keepalive);
    cfg->Write(L"keepaliveInterval", static_cast<long>(p.keepaliveInterval));
    cfg->Write(L"autoReconnect", p.autoReconnect);
    cfg->Write(L"compression", p.compression);
    cfg->Write(L"timezone", p.timezone);
    cfg->Write(L"appName", p.appName);
    cfg->Write(L"initCommands", p.initCommands);
    cfg->Write(L"socketFile", p.socketFile);
    cfg->Write(L"defaultSchema", p.defaultSchema);
    cfg->Write(L"readOnly", p.readOnly);
    cfg->Write(L"isolationLevel", p.isolationLevel);
    // databases
    cfg->Write(L"dbFilter", p.dbFilter);
    cfg->Write(L"dbListMode", static_cast<long>(p.dbListMode));
    cfg->Write(L"showSystemDbs", p.showSystemDbs);
    cfg->Write(L"autoConnect", p.autoConnectOnStartup);
    // ssl
    cfg->Write(L"sslEnabled", p.sslEnabled);
    cfg->Write(L"sslMode", p.sslMode);
    cfg->Write(L"sslAuth", static_cast<long>(p.sslAuth));
    cfg->Write(L"sslCa", p.sslCaCert);
    cfg->Write(L"sslCert", p.sslClientCert);
    cfg->Write(L"sslKey", p.sslClientKey);
    cfg->Write(L"sslKeyPass", EncryptSecret(p.sslClientKeyPassword));
    cfg->Write(L"sslVerify", p.sslVerifyServerCert);
    cfg->Write(L"sslVerifyHost", p.sslVerifyHostname);
    cfg->Write(L"sslCipher", p.sslCipher);
    cfg->Write(L"sslMinVersion", p.sslMinVersion);
    // ssh
    cfg->Write(L"sshEnabled", p.sshEnabled);
    cfg->Write(L"sshHost", p.sshHost);
    cfg->Write(L"sshPort", static_cast<long>(p.sshPort));
    cfg->Write(L"sshUser", p.sshUser);
    cfg->Write(L"sshAuth", static_cast<long>(p.sshAuth));
    cfg->Write(L"sshPassword", EncryptSecret(p.sshPassword));
    cfg->Write(L"sshKeyFile", p.sshKeyFile);
    cfg->Write(L"sshKeyPass", EncryptSecret(p.sshKeyPassword));
    cfg->Write(L"sshLocalPort", static_cast<long>(p.sshLocalPort));
    cfg->Write(L"sshKeepalive", static_cast<long>(p.sshKeepalive));
    cfg->Write(L"sshCompression", p.sshCompression);
    // http
    cfg->Write(L"httpEnabled", p.httpEnabled);
    cfg->Write(L"httpUrl", p.httpUrl);
    cfg->Write(L"httpAuth", static_cast<long>(p.httpAuth));
    cfg->Write(L"httpUser", p.httpUser);
    cfg->Write(L"httpPassword", EncryptSecret(p.httpPassword));
    cfg->Write(L"httpVerifyCert", p.httpVerifyCert);
    cfg->Flush();
}

void ConnectionStore::Remove(const wxString& name)
{
    auto cfg = Open();
    wxString group; long idx = 0;
    cfg->SetPath(L"/");
    bool more = cfg->GetFirstGroup(group, idx);
    while (more) {
        cfg->SetPath(L"/" + group);
        const bool match = (cfg->Read(L"name", wxEmptyString) == name);
        cfg->SetPath(L"/");
        if (match) { cfg->DeleteGroup(group); break; }
        more = cfg->GetNextGroup(group, idx);
    }
    cfg->Flush();
}

} // namespace core
