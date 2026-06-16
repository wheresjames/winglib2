#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

extern char** environ;

namespace {

class HttpFixture {
public:
    HttpFixture() {
        serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ < 0) {
            fail("socket");
        }

        int one = 1;
        if (::setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
            fail("setsockopt");
        }

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

    ~HttpFixture() {
        stop();
        if (serverFd_ >= 0) {
            ::close(serverFd_);
        }
    }

    HttpFixture(const HttpFixture&) = delete;
    HttpFixture& operator=(const HttpFixture&) = delete;

    void start() {
        thread_ = std::thread([this] { serve(); });
    }

    void stop() {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::string baseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

private:
    [[noreturn]] void fail(const char* what) const {
        std::cerr << what << ": " << std::strerror(errno) << '\n';
        std::exit(2);
    }

    void serve() {
        while (!stopping_.load()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(serverFd_, &set);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
            int ready = ::select(serverFd_ + 1, &set, nullptr, nullptr, &timeout);
            if (ready <= 0) {
                continue;
            }

            int client = ::accept(serverFd_, nullptr, nullptr);
            if (client >= 0) {
                handle(client);
                ::close(client);
            }
        }
    }

    static bool sendAll(int fd, std::string_view data) {
        const char* cursor = data.data();
        size_t remaining = data.size();
        while (remaining > 0) {
            ssize_t sent = ::send(fd, cursor, remaining, MSG_NOSIGNAL);
            if (sent <= 0) {
                return false;
            }
            cursor += sent;
            remaining -= static_cast<size_t>(sent);
        }
        return true;
    }

    static std::string readRequest(int fd) {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return request;
            }
            request.append(buffer, static_cast<size_t>(n));
            if (request.size() > 65536) {
                return request;
            }
        }

        auto headerEnd = request.find("\r\n\r\n");
        size_t contentLength = 0;
        std::istringstream headers(request.substr(0, headerEnd));
        std::string line;
        while (std::getline(headers, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            constexpr std::string_view prefix = "Content-Length:";
            if (line.size() >= prefix.size()
                    && std::equal(prefix.begin(), prefix.end(), line.begin(),
                        [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
                contentLength = static_cast<size_t>(std::stoul(line.substr(prefix.size())));
            }
        }

        size_t bodyStart = headerEnd + 4;
        while (request.size() < bodyStart + contentLength) {
            ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                break;
            }
            request.append(buffer, static_cast<size_t>(n));
        }
        return request;
    }

    void handle(int client) const {
        std::string request = readRequest(client);
        std::istringstream lines(request);
        std::string method;
        std::string path;
        lines >> method >> path;

        if (method == "GET" && path == "/text") {
            sendResponse(client, 200, "OK", "text/plain; charset=utf-8",
                "winglib2 wl2_curl smoke response\n",
                "X-WL2-Fixture: text\r\n");
        } else if (method == "GET" && path == "/headers") {
            sendResponse(client, 200, "OK", "application/json",
                "{\"ok\":true}\n",
                "X-WL2-Fixture: headers\r\n");
        } else if (method == "GET" && path == "/redirect") {
            sendAll(client,
                "HTTP/1.1 302 Found\r\n"
                "Location: /text\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n");
        } else if (method == "POST" && path == "/echo") {
            auto bodyStart = request.find("\r\n\r\n");
            std::string body = bodyStart == std::string::npos ? std::string{} : request.substr(bodyStart + 4);
            sendResponse(client, 200, "OK", "text/plain; charset=utf-8",
                "posted:" + body,
                "X-WL2-Fixture: echo\r\n");
        } else if (method == "GET" && path == "/slow") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            sendResponse(client, 200, "OK", "text/plain; charset=utf-8", "slow\n", {});
        } else {
            sendResponse(client, 404, "Not Found", "text/plain; charset=utf-8", "not found\n", {});
        }
    }

    static void sendResponse(
            int client,
            int status,
            std::string_view reason,
            std::string_view contentType,
            std::string_view body,
            std::string_view extraHeaders) {
        std::ostringstream out;
        out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
            << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << extraHeaders
            << "Connection: close\r\n\r\n"
            << body;
        sendAll(client, out.str());
    }

    int serverFd_ = -1;
    uint16_t port_ = 0;
    std::atomic_bool stopping_{false};
    std::thread thread_;
};

int runChild(const char* wl2, const char* script, const std::string& baseUrl) {
    std::string postUrl = baseUrl + "/echo";
    ::setenv("WL2_CURL_TEST_URL", baseUrl.c_str(), 1);
    ::setenv("WL2_CURL_TEST_POST_URL", postUrl.c_str(), 1);

    pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "fork: " << std::strerror(errno) << '\n';
        return 2;
    }
    if (child == 0) {
        char* const argv[] = {
            const_cast<char*>(wl2),
            const_cast<char*>("run"),
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

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: wl2_curl_fixture <wl2-executable> <smoke-script>\n";
        return 2;
    }

    HttpFixture fixture;
    fixture.start();
    int result = runChild(argv[1], argv[2], fixture.baseUrl());
    fixture.stop();
    return result;
}
