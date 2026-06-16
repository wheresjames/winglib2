// Spawns a loopback TCP echo server on an ephemeral port, then runs the wl2
// runner against a JavaScript test that connects to it through wl2:asio. Network
// access is granted only for the fixture endpoint, so the test exercises the
// real capability gate. No public network is used.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

class EchoFixture {
public:
    EchoFixture() {
        serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ < 0) {
            fail("socket");
        }
        int one = 1;
        ::setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(serverFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            fail("bind");
        }
        if (::listen(serverFd_, 16) != 0) {
            fail("listen");
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(serverFd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            fail("getsockname");
        }
        port_ = ntohs(addr.sin_port);
    }

    ~EchoFixture() {
        stop();
        if (serverFd_ >= 0) {
            ::close(serverFd_);
        }
    }

    EchoFixture(const EchoFixture&) = delete;
    EchoFixture& operator=(const EchoFixture&) = delete;

    void start() {
        thread_ = std::thread([this] { serve(); });
    }

    void stop() {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    uint16_t port() const {
        return port_;
    }

private:
    [[noreturn]] void fail(const char* what) const {
        std::cerr << "echo fixture " << what << ": " << std::strerror(errno) << '\n';
        std::exit(2);
    }

    void serve() {
        while (!stopping_.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(serverFd_, &readSet);
            timeval timeout{0, 100000};  // 100ms poll so stopping_ is observed
            int ready = ::select(serverFd_ + 1, &readSet, nullptr, nullptr, &timeout);
            if (ready <= 0) {
                continue;
            }
            int client = ::accept(serverFd_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            echo(client);
            ::close(client);
        }
    }

    static void echo(int client) {
        char buffer[4096];
        for (;;) {
            ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return;
            }
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t w = ::send(client, buffer + sent, static_cast<size_t>(n - sent), 0);
                if (w <= 0) {
                    return;
                }
                sent += w;
            }
        }
    }

    int serverFd_ = -1;
    uint16_t port_ = 0;
    std::atomic_bool stopping_{false};
    std::thread thread_;
};

int runChild(const char* wl2, const char* script, uint16_t port) {
    const std::string host = "127.0.0.1";
    const std::string endpoint = host + ":" + std::to_string(port);
    ::setenv("WL2_ASIO_TEST_HOST", host.c_str(), 1);
    ::setenv("WL2_ASIO_TEST_PORT", std::to_string(port).c_str(), 1);

    pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "fork: " << std::strerror(errno) << '\n';
        return 2;
    }
    if (child == 0) {
        char* const argv[] = {
            const_cast<char*>(wl2),
            const_cast<char*>("run"),
            const_cast<char*>("--network-allow"),
            const_cast<char*>(endpoint.c_str()),
            const_cast<char*>(script),
            nullptr,
        };
        ::execve(wl2, argv, environ);
        std::cerr << "execve: " << std::strerror(errno) << '\n';
        _exit(127);
    }

    int status = 0;
    if (::waitpid(child, &status, 0) < 0) {
        std::cerr << "waitpid: " << std::strerror(errno) << '\n';
        return 2;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        std::cerr << "wl2 terminated by signal " << WTERMSIG(status) << '\n';
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: wl2_asio_fixture <wl2-executable> <test-script>\n";
        return 2;
    }

    EchoFixture fixture;
    fixture.start();
    int result = runChild(argv[1], argv[2], fixture.port());
    fixture.stop();
    return result;
}
