// SshTunnel.h — an SSH local port-forward. When a connection profile has SSH
// enabled, Open() dials the SSH host, authenticates, and starts forwarding a
// local 127.0.0.1:<port> to the real database host:port through the SSH channel.
// The DB driver then connects to that local endpoint. pImpl keeps libssh2 out of
// the public header. swiftsql::net is a leaf module (net → core only).
#pragma once

#include <wx/string.h>
#include <memory>
#include "core/ConnectionProfile.h"

namespace net {

class SshTunnel {
public:
    SshTunnel();
    ~SshTunnel();
    SshTunnel(const SshTunnel&) = delete;
    SshTunnel& operator=(const SshTunnel&) = delete;

    // Establish the SSH session and start forwarding to p.host:p.port. On success
    // the DB should connect to 127.0.0.1:localPort(). Returns false + err on
    // failure.
    bool Open(const core::ConnectionProfile& p, wxString& err);
    void Close();

    bool IsOpen() const;
    int  localPort() const;   // 0 until Open() succeeds

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace net
