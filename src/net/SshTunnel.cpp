#include "net/SshTunnel.h"
#include "core/CrashLog.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <libssh2.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace net {

struct SshTunnel::Impl {
    LIBSSH2_SESSION* session = nullptr;
    SOCKET sshSock = INVALID_SOCKET;
    SOCKET listenSock = INVALID_SOCKET;
    std::atomic<SOCKET> client{ INVALID_SOCKET };   // current forwarded client
    int    localPort = 0;
    std::string remoteHost;
    int    remotePort = 0;
    std::atomic<bool> running{ false };
    std::thread accept;
    std::mutex sess;                  // guards libssh2 session calls
    bool wsa = false;
    bool sshInit = false;             // libssh2_init succeeded; pair with libssh2_exit

    void acceptLoop();
    void forward(SOCKET c);
};

namespace {

SOCKET DialTcp(const std::string& host, int port, wxString& err)
{
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        err = L"无法解析主机 " + wxString::FromUTF8(host);
        return INVALID_SOCKET;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); err = L"创建套接字失败"; return s; }
    if (connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        closesocket(s); freeaddrinfo(res);
        err = L"连接 " + wxString::FromUTF8(host) + L" 失败";
        return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
SshTunnel::SshTunnel() : impl_(std::make_unique<Impl>()) {}
SshTunnel::~SshTunnel() { Close(); }

bool SshTunnel::IsOpen() const { return impl_->running.load(); }
int  SshTunnel::localPort() const { return impl_->localPort; }

bool SshTunnel::Open(const core::ConnectionProfile& p, wxString& err)
{
    Impl& d = *impl_;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = L"WSAStartup 失败"; return false; }
    d.wsa = true;

    const std::string sshHost = p.sshHost.utf8_string();
    d.sshSock = DialTcp(sshHost, p.sshPort, err);
    if (d.sshSock == INVALID_SOCKET) { Close(); return false; }

    if (libssh2_init(0) != 0) { err = L"libssh2 初始化失败"; Close(); return false; }
    d.sshInit = true;
    d.session = libssh2_session_init();
    if (!d.session) { err = L"创建 SSH 会话失败"; Close(); return false; }

    libssh2_session_set_blocking(d.session, 1);
    if (libssh2_session_handshake(d.session, static_cast<libssh2_socket_t>(d.sshSock))) {
        err = L"SSH 握手失败"; Close(); return false;
    }

    // ---- authenticate ----
    const std::string user = p.sshUser.utf8_string();
    std::string pass = p.sshPassword.utf8_string();   // non-const so we can scrub
    const std::string key  = p.sshKeyFile.utf8_string();
    std::string kpw  = p.sshKeyPassword.utf8_string(); // non-const so we can scrub
    int rc = LIBSSH2_ERROR_AUTHENTICATION_FAILED;
    switch (p.sshAuth) {
    case core::SshAuth::PublicKey:
        rc = libssh2_userauth_publickey_fromfile(d.session, user.c_str(), nullptr,
                                                 key.c_str(), kpw.c_str());
        break;
    case core::SshAuth::PasswordAndKey:
        rc = libssh2_userauth_publickey_fromfile(d.session, user.c_str(), nullptr,
                                                 key.c_str(), kpw.c_str());
        if (rc) rc = libssh2_userauth_password(d.session, user.c_str(), pass.c_str());
        break;
    case core::SshAuth::Agent:
        err = L"SSH Agent 认证暂未支持,请改用密码或密钥"; Close(); return false;
    case core::SshAuth::Password:
    case core::SshAuth::KeyboardInteractive:
    default:
        rc = libssh2_userauth_password(d.session, user.c_str(), pass.c_str());
        break;
    }
    // Scrub the plaintext SSH secrets from our local buffers now that libssh2
    // has consumed them. Best effort: the ConnectionProfile source (wxString)
    // holds its own copies we cannot reliably wipe from here — this at least
    // keeps the decoded UTF-8 out of the heap for the rest of the tunnel's life.
    if (!pass.empty()) SecureZeroMemory(&pass[0], pass.size());
    if (!kpw.empty())  SecureZeroMemory(&kpw[0], kpw.size());

    if (rc) { err = L"SSH 认证失败(检查用户名/密码/密钥)"; Close(); return false; }

    // ---- local listener on 127.0.0.1:0 (auto port) ----
    d.listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    if (bind(d.listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(d.listenSock, 4) != 0) {
        err = L"创建本地转发端口失败"; Close(); return false;
    }
    int alen = sizeof addr;
    getsockname(d.listenSock, reinterpret_cast<sockaddr*>(&addr), &alen);
    d.localPort = ntohs(addr.sin_port);

    d.remoteHost = p.host.utf8_string();
    d.remotePort = p.port;
    d.running = true;
    d.accept = core::CrashLog::GuardedThread(L"SSH 隧道", [&d] { d.acceptLoop(); });
    return true;
}

void SshTunnel::Impl::acceptLoop()
{
    while (running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(listenSock, &fds);
        timeval tv{ 1, 0 };
        const int r = select(0, &fds, nullptr, nullptr, &tv);
        if (!running) break;
        if (r > 0 && FD_ISSET(listenSock, &fds)) {
            SOCKET c = ::accept(listenSock, nullptr, nullptr);
            if (c != INVALID_SOCKET) forward(c);   // one DB connection at a time
        }
    }
}

void SshTunnel::Impl::forward(SOCKET c)
{
    client.store(c);
    LIBSSH2_CHANNEL* ch = nullptr;
    {
        std::lock_guard<std::mutex> lk(sess);
        libssh2_session_set_blocking(session, 1);
        ch = libssh2_channel_direct_tcpip_ex(session, remoteHost.c_str(), remotePort,
                                             "127.0.0.1", localPort);
        libssh2_session_set_blocking(session, 0);
    }
    if (!ch) { if (client.exchange(INVALID_SOCKET) != INVALID_SOCKET) closesocket(c); return; }

    char buf[16384];
    bool done = false;
    while (running && !done) {
        fd_set fds; FD_ZERO(&fds); FD_SET(c, &fds); FD_SET(sshSock, &fds);
        timeval tv{ 0, 200000 };   // 200ms
        select(0, &fds, nullptr, nullptr, &tv);
        if (!running) break;

        if (FD_ISSET(c, &fds)) {                 // client → channel
            const int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            int off = 0;
            while (off < n) {
                std::lock_guard<std::mutex> lk(sess);
                const int w = libssh2_channel_write(ch, buf + off, n - off);
                if (w == LIBSSH2_ERROR_EAGAIN) continue;
                if (w < 0) { done = true; break; }
                off += w;
            }
        }
        for (;;) {                                // channel → client
            int rd;
            { std::lock_guard<std::mutex> lk(sess); rd = libssh2_channel_read(ch, buf, sizeof buf); }
            if (rd == LIBSSH2_ERROR_EAGAIN) break;
            if (rd < 0) { done = true; break; }
            if (rd == 0) break;
            int off = 0;
            while (off < rd) {
                const int s = send(c, buf + off, rd - off, 0);
                if (s <= 0) { done = true; break; }
                off += s;
            }
        }
        { std::lock_guard<std::mutex> lk(sess); if (libssh2_channel_eof(ch)) done = true; }
    }

    { std::lock_guard<std::mutex> lk(sess); libssh2_channel_free(ch); }
    if (client.exchange(INVALID_SOCKET) != INVALID_SOCKET) closesocket(c);
}

void SshTunnel::Close()
{
    Impl& d = *impl_;
    d.running = false;
    { SOCKET oldc = d.client.exchange(INVALID_SOCKET); if (oldc != INVALID_SOCKET) closesocket(oldc); }
    if (d.listenSock != INVALID_SOCKET) { closesocket(d.listenSock); d.listenSock = INVALID_SOCKET; }
    if (d.accept.joinable()) d.accept.join();
    if (d.session) {
        libssh2_session_disconnect(d.session, "bye");
        libssh2_session_free(d.session);
        d.session = nullptr;
    }
    if (d.sshInit) { libssh2_exit(); d.sshInit = false; }
    if (d.sshSock != INVALID_SOCKET) { closesocket(d.sshSock); d.sshSock = INVALID_SOCKET; }
    if (d.wsa) { WSACleanup(); d.wsa = false; }
    d.localPort = 0;
}

} // namespace net
