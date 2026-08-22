// ConnectionProfile.h — a database connection's full configuration, covering
// General / Advanced / Databases / SSL / SSH / HTTP (Navicat/DBeaver-class
// coverage). Passwords/passphrases are encrypted at rest via core::Secret.
#pragma once

#include <wx/string.h>
#include "core/DbTypes.h"

namespace core {

// SSL client authentication method (the user picks how the client presents).
enum class SslAuth {
    None = 0,          // no client cert/key
    KeyOnly,           // client key only
    KeyPassphrase,     // client key + passphrase
    CertKey,           // client cert + key
    CertKeyPassphrase, // client cert + key + passphrase
};

// SSH authentication method.
enum class SshAuth {
    Password = 0,
    PublicKey,
    PasswordAndKey,
    Agent,
    KeyboardInteractive,
};

// HTTP tunnel authentication.
enum class HttpAuth {
    None = 0,
    Basic,
    Digest,
};

// How the "Databases" tab filters the database list.
enum class DbListMode {
    All = 0,
    Include,   // only the listed databases
    Exclude,   // all except the listed databases
};

struct ConnectionProfile {
    // ================= 常规 (General) =================
    wxString   name;
    db::DbType type = db::DbType::MySQL;
    wxString   host = L"127.0.0.1";
    int        port = 3306;
    wxString   user;
    wxString   password;          // encrypted at rest
    wxString   database;          // initial database
    bool       savePassword = true;
    wxString   color;             // tree label colour "#RRGGBB" ("" = default)

    // ================= 高级 (Advanced) =================
    wxString   charset;           // "" = driver default
    int        connectTimeout = 8;   // seconds
    int        readWriteTimeout = 0; // seconds, 0 = none
    bool       keepalive = true;
    int        keepaliveInterval = 30;   // seconds
    bool       autoReconnect = false;
    bool       compression = false;      // protocol compression
    wxString   timezone;          // session time zone ("" = server default)
    wxString   appName;           // application_name / program name
    wxString   initCommands;      // SQL executed on connect (';'-separated)
    wxString   socketFile;        // unix socket / named pipe (local)
    wxString   defaultSchema;     // PG search_path / default schema
    bool       readOnly = false;
    wxString   isolationLevel;    // "", "READ COMMITTED", "SERIALIZABLE"…

    // ================= 数据库 (Databases) =================
    wxString   dbFilter;          // comma-separated names
    DbListMode dbListMode = DbListMode::All;
    bool       showSystemDbs = false;
    bool       autoConnectOnStartup = false;

    // ================= SSL =================
    bool       sslEnabled = false;
    wxString   sslMode;           // disable/allow/prefer/require/verify-ca/verify-full
    SslAuth    sslAuth = SslAuth::None;
    wxString   sslCaCert;
    wxString   sslClientCert;
    wxString   sslClientKey;
    wxString   sslClientKeyPassword;  // encrypted at rest
    bool       sslVerifyServerCert = false;
    bool       sslVerifyHostname = false;
    wxString   sslCipher;
    wxString   sslMinVersion;     // "", "TLSv1.2", "TLSv1.3"

    // ================= SSH tunnel (config persisted; forwarding is next milestone) =================
    bool       sshEnabled = false;
    wxString   sshHost;
    int        sshPort = 22;
    wxString   sshUser;
    SshAuth    sshAuth = SshAuth::Password;
    wxString   sshPassword;       // encrypted at rest
    wxString   sshKeyFile;        // private key path
    wxString   sshKeyPassword;    // key passphrase, encrypted at rest
    int        sshLocalPort = 0;  // 0 = auto-assign
    int        sshKeepalive = 30; // seconds
    bool       sshCompression = false;

    // ================= HTTP tunnel (config persisted; forwarding is next milestone) =================
    bool       httpEnabled = false;
    wxString   httpUrl;
    HttpAuth   httpAuth = HttpAuth::None;
    wxString   httpUser;
    wxString   httpPassword;      // encrypted at rest
    bool       httpVerifyCert = true;

    wxString Caption() const
    {
        return wxString(db::InfoOf(type).name) + L" · " + host;
    }
};

} // namespace core
