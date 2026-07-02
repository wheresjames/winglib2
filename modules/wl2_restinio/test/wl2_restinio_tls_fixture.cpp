// HTTPS end-to-end test for wl2:http. Runs the TLS server in a wl2 subprocess and
// drives it as an out-of-process TLS client (OpenSSL). The self-signed cert in
// test/data is trusted implicitly (the client does not verify), which is
// sufficient to prove the TLS handshake and request/response path work.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

extern char** environ;

namespace {

int fail(const std::string& message) {
    std::cerr << "wl2_restinio tls fixture failed: " << message << '\n';
    return 1;
}

uint16_t pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    uint16_t port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }
    ::close(fd);
    return port;
}

int connect_with_retry(uint16_t port, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);
        usleep(100 * 1000);
    }
    return -1;
}

// TLS GET over a fresh connection; returns the raw response (no cert verification).
bool https_get(uint16_t port, const std::string& target, std::string& response) {
    int fd = connect_with_retry(port, 50);
    if (fd < 0) {
        return false;
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ::close(fd);
        return false;
    }
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, "127.0.0.1");
    bool ok = false;
    if (SSL_connect(ssl) == 1) {
        std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (SSL_write(ssl, req.data(), static_cast<int>(req.size())) > 0) {
            response.clear();
            char buf[4096];
            for (;;) {
                int n = SSL_read(ssl, buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                response.append(buf, static_cast<size_t>(n));
            }
            ok = true;
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    return ok;
}

int status_of(const std::string& response) {
    size_t sp = response.find(' ');
    return sp == std::string::npos ? -1 : std::atoi(response.c_str() + sp + 1);
}

std::string body_of(const std::string& response) {
    size_t pos = response.find("\r\n\r\n");
    return pos == std::string::npos ? std::string{} : response.substr(pos + 4);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: wl2_restinio_tls_fixture <wl2> <server-script> <cert.pem> <key.pem>\n";
        return 2;
    }
    const char* wl2 = argv[1];
    const char* script = argv[2];
    const char* cert = argv[3];
    const char* key = argv[4];

    uint16_t port = pick_free_port();
    if (port == 0) {
        return fail("could not pick a free port");
    }
    const std::string portStr = std::to_string(port);

    pid_t child = ::fork();
    if (child < 0) {
        return fail(std::string("fork: ") + std::strerror(errno));
    }
    if (child == 0) {
        char* const childArgv[] = {
            const_cast<char*>(wl2), const_cast<char*>("run"),
            const_cast<char*>("--allow-listen"), const_cast<char*>("--listen-allow"),
            const_cast<char*>("127.0.0.1"), const_cast<char*>(script),
            const_cast<char*>(portStr.c_str()), const_cast<char*>(cert), const_cast<char*>(key),
            nullptr,
        };
        ::execve(wl2, childArgv, environ);
        std::cerr << "execve: " << std::strerror(errno) << '\n';
        _exit(127);
    }

    int rc = 0;
    std::string response;
    if (!https_get(port, "/secure", response)) {
        rc = fail("TLS request failed (handshake or I/O)");
    } else if (status_of(response) != 200) {
        rc = fail("HTTPS status: " + std::to_string(status_of(response)));
    } else if (body_of(response) != "secure-hello") {
        rc = fail("HTTPS body: [" + body_of(response) + "]");
    }

    ::kill(child, SIGTERM);
    int status = 0;
    ::waitpid(child, &status, 0);

    if (rc == 0) {
        std::cout << "wl2_restinio tls fixture ok\n";
    }
    return rc;
}
