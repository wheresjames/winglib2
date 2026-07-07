// Drives the wl2:http server end-to-end as an out-of-process client. The server
// runs in a wl2 subprocess (its JS thread stays free); this fixture is the HTTP
// client over a raw loopback socket. An in-process client is not usable because
// the wl2:curl client blocks the JS thread the server also needs to run handlers.
//
// Steps: pick a free loopback port, export it, spawn `wl2 run <server script>`
// with listen granted, wait for the port to accept, issue HTTP/1.1 requests,
// assert routing / params / body / 404, then terminate the subprocess.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

extern char** environ;

namespace {

int fail(const std::string& message) {
    std::cerr << "wl2_restinio fixture failed: " << message << '\n';
    return 1;
}

// Bind a loopback socket to port 0 to discover a free port, then release it.
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

// Open a loopback TCP connection, retrying until the server accepts or we give up.
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
        usleep(100 * 1000); // 100ms
    }
    return -1;
}

// Send one HTTP/1.1 request with Connection: close and return the whole raw
// response (headers + body). `extraHeaders` is raw header lines (each ending in
// CRLF); when it sets Content-Type, the default is not added. Returns false on
// socket failure.
bool http_request(uint16_t port, const std::string& method, const std::string& target,
    const std::string& body, std::string& response, const std::string& extraHeaders = "") {
    int fd = connect_with_retry(port, 1);
    if (fd < 0) {
        return false;
    }
    std::string req = method + " " + target + " HTTP/1.1\r\n";
    req += "Host: 127.0.0.1\r\n";
    req += "Connection: close\r\n";
    req += extraHeaders;
    if (!body.empty() && extraHeaders.find("Content-Type") == std::string::npos) {
        req += "Content-Type: text/plain\r\n";
    }
    if (!body.empty()) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t w = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (w <= 0) {
            ::close(fd);
            return false;
        }
        sent += static_cast<size_t>(w);
    }

    response.clear();
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return true;
}

int status_of(const std::string& response) {
    // "HTTP/1.1 200 OK\r\n..."
    size_t sp = response.find(' ');
    if (sp == std::string::npos) {
        return -1;
    }
    return std::atoi(response.c_str() + sp + 1);
}

std::string body_of(const std::string& response) {
    size_t pos = response.find("\r\n\r\n");
    return pos == std::string::npos ? std::string{} : response.substr(pos + 4);
}

// Case-insensitive lookup of a response header value (headers section only).
std::string header_of(const std::string& response, const std::string& name) {
    size_t end = response.find("\r\n\r\n");
    std::string head = end == std::string::npos ? response : response.substr(0, end);
    std::string lowerHead = head;
    std::string lowerName = name;
    for (char& c : lowerHead) c = static_cast<char>(::tolower(c));
    for (char& c : lowerName) c = static_cast<char>(::tolower(c));
    size_t pos = lowerHead.find("\r\n" + lowerName + ":");
    if (pos == std::string::npos) {
        return {};
    }
    size_t valStart = pos + 2 + lowerName.size() + 1;
    size_t valEnd = head.find("\r\n", valStart);
    std::string value = head.substr(valStart, valEnd - valStart);
    size_t s = value.find_first_not_of(" \t");
    return s == std::string::npos ? std::string{} : value.substr(s);
}

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t w = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (w <= 0) {
            return false;
        }
        sent += static_cast<size_t>(w);
    }
    return true;
}

bool recv_exact(int fd, char* out, size_t size) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = ::recv(fd, out + got, size - got, 0);
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

int websocket_connect(uint16_t port) {
    int fd = connect_with_retry(port, 1);
    if (fd < 0) {
        return -1;
    }
    std::string req;
    req += "GET /socket HTTP/1.1\r\n";
    req += "Host: 127.0.0.1\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "\r\n";
    if (!send_all(fd, req)) {
        ::close(fd);
        return -1;
    }
    std::string head;
    char c = 0;
    while (head.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0) {
            ::close(fd);
            return -1;
        }
        head.push_back(c);
        if (head.size() > 4096) {
            ::close(fd);
            return -1;
        }
    }
    if (head.find(" 101 ") == std::string::npos) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string websocket_client_frame(uint8_t opcode, const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80u | opcode));
    const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80u | static_cast<uint8_t>(payload.size())));
    } else {
        frame.push_back(static_cast<char>(0x80u | 126u));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }
    return frame;
}

bool websocket_read_frame(int fd, uint8_t& opcode, std::string& payload) {
    unsigned char hdr[2];
    if (!recv_exact(fd, reinterpret_cast<char*>(hdr), 2)) {
        return false;
    }
    opcode = hdr[0] & 0x0f;
    uint64_t len = hdr[1] & 0x7f;
    if (len == 126) {
        unsigned char ext[2];
        if (!recv_exact(fd, reinterpret_cast<char*>(ext), 2)) {
            return false;
        }
        len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (!recv_exact(fd, reinterpret_cast<char*>(ext), 8)) {
            return false;
        }
        len = 0;
        for (unsigned char b : ext) {
            len = (len << 8) | b;
        }
    }
    payload.assign(static_cast<size_t>(len), '\0');
    return len == 0 || recv_exact(fd, payload.data(), static_cast<size_t>(len));
}

uint16_t websocket_close_code(const std::string& payload) {
    if (payload.size() < 2) {
        return 0;
    }
    return (static_cast<uint16_t>(static_cast<unsigned char>(payload[0])) << 8)
        | static_cast<uint16_t>(static_cast<unsigned char>(payload[1]));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: wl2_restinio_fixture <wl2-executable> <server-script> [static-root]\n";
        return 2;
    }
    const char* wl2 = argv[1];
    const char* script = argv[2];
    const std::string staticRoot = argc == 4 ? argv[3] : "";

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
        std::vector<char*> childArgv = {
            const_cast<char*>(wl2),
            const_cast<char*>("run"),
            const_cast<char*>("--allow-listen"),
            const_cast<char*>("--listen-allow"),
            const_cast<char*>("127.0.0.1"),
        };
        if (!staticRoot.empty()) {
            childArgv.push_back(const_cast<char*>("--allow-filesystem-reads"));
            childArgv.push_back(const_cast<char*>("--filesystem-read-root"));
            childArgv.push_back(const_cast<char*>(staticRoot.c_str()));
        }
        childArgv.push_back(const_cast<char*>(script));
        childArgv.push_back(const_cast<char*>(portStr.c_str()));
        if (!staticRoot.empty()) {
            childArgv.push_back(const_cast<char*>(staticRoot.c_str()));
        }
        childArgv.push_back(nullptr);
        ::execve(wl2, childArgv.data(), environ);
        std::cerr << "execve: " << std::strerror(errno) << '\n';
        _exit(127);
    }

    int rc = 0;
    // Give the server time to bind (connect retries cover startup).
    if (connect_with_retry(port, 50) < 0) {
        rc = fail("server did not start listening");
    }

    std::string response;
    if (rc == 0) {
        if (!http_request(port, "GET", "/hello/world?x=1", "", response)) {
            rc = fail("GET request failed");
        } else if (status_of(response) != 200) {
            rc = fail("GET status: " + std::to_string(status_of(response)));
        } else if (body_of(response) != "hi world q=x=1") {
            rc = fail("GET body: [" + body_of(response) + "]");
        }
    }
    if (rc == 0) {
        if (!http_request(port, "GET", "/plain", "", response) || body_of(response) != "plain-ok") {
            rc = fail("sync string handler body: [" + body_of(response) + "]");
        }
    }
    if (rc == 0) {
        if (!http_request(port, "POST", "/echo", "ping-pong", response)) {
            rc = fail("POST request failed");
        } else if (status_of(response) != 201) {
            rc = fail("POST status: " + std::to_string(status_of(response)));
        } else if (body_of(response) != "ping-pong") {
            rc = fail("POST echo body: [" + body_of(response) + "]");
        }
    }
    if (rc == 0) {
        if (!http_request(port, "GET", "/does-not-exist", "", response) || status_of(response) != 404) {
            rc = fail("expected 404, got " + std::to_string(status_of(response)));
        }
    }
    // Cookie parsing.
    if (rc == 0) {
        if (!http_request(port, "GET", "/cookie", "", response, "Cookie: sid=abc123; theme=dark\r\n")
            || body_of(response) != "sid=abc123") {
            rc = fail("cookie body: [" + body_of(response) + "]");
        }
    }
    // Multipart upload.
    if (rc == 0) {
        const std::string boundary = "----wl2boundary";
        std::string form;
        form += "--" + boundary + "\r\n";
        form += "Content-Disposition: form-data; name=\"doc\"; filename=\"note.txt\"\r\n";
        form += "Content-Type: text/plain\r\n\r\n";
        form += "hello-upload\r\n";
        form += "--" + boundary + "--\r\n";
        const std::string ct = "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
        if (!http_request(port, "POST", "/upload", form, response, ct)
            || body_of(response) != "doc:note.txt:hello-upload") {
            rc = fail("multipart body: [" + body_of(response) + "]");
        }
    }
    // gzip: without Accept-Encoding the body is uncompressed plaintext.
    std::string plainBig;
    if (rc == 0) {
        if (!http_request(port, "GET", "/big", "", response) || status_of(response) != 200) {
            rc = fail("gzip baseline request failed");
        } else if (!header_of(response, "content-encoding").empty()) {
            rc = fail("unexpected content-encoding without Accept-Encoding");
        } else {
            plainBig = body_of(response);
            if (plainBig.size() != 500) {
                rc = fail("gzip baseline body size: " + std::to_string(plainBig.size()));
            }
        }
    }
    // gzip: with Accept-Encoding: gzip the response is gzip-encoded and smaller.
    if (rc == 0) {
        if (!http_request(port, "GET", "/big", "", response, "Accept-Encoding: gzip\r\n")) {
            rc = fail("gzip request failed");
        } else if (header_of(response, "content-encoding") != "gzip") {
            rc = fail("expected content-encoding gzip, got [" + header_of(response, "content-encoding") + "]");
        } else if (body_of(response).size() >= plainBig.size()) {
            rc = fail("gzip body not smaller: " + std::to_string(body_of(response).size()));
        }
    }
    // Static file serving (only when a root was provided).
    if (rc == 0 && !staticRoot.empty()) {
        if (!http_request(port, "GET", "/assets/hello.txt", "", response)) {
            rc = fail("static request failed");
        } else if (status_of(response) != 200) {
            rc = fail("static status: " + std::to_string(status_of(response)));
        } else if (body_of(response) != "winglib2 static asset\n") {
            rc = fail("static body: [" + body_of(response) + "]");
        } else if (header_of(response, "content-type").find("text/plain") == std::string::npos) {
            rc = fail("static content-type: [" + header_of(response, "content-type") + "]");
        } else if (header_of(response, "cache-control") != "no-store") {
            rc = fail("static cache-control: [" + header_of(response, "cache-control") + "]");
        }
    }
    // Static MIME overrides support media playlist mounts used by the streamer examples.
    if (rc == 0 && !staticRoot.empty()) {
        if (!http_request(port, "GET", "/assets/playlist.m3u8", "", response)) {
            rc = fail("static playlist request failed");
        } else if (status_of(response) != 200) {
            rc = fail("static playlist status: " + std::to_string(status_of(response)));
        } else if (header_of(response, "content-type").find("application/vnd.apple.mpegurl") == std::string::npos) {
            rc = fail("static playlist content-type: [" + header_of(response, "content-type") + "]");
        }
    }
    // Static traversal must be rejected.
    if (rc == 0 && !staticRoot.empty()) {
        if (!http_request(port, "GET", "/assets/../secret", "", response) || status_of(response) != 403) {
            rc = fail("traversal not blocked, status " + std::to_string(status_of(response)));
        }
    }
    // WebSocket welcome + echo.
    if (rc == 0) {
        int ws = websocket_connect(port);
        if (ws < 0) {
            rc = fail("websocket handshake failed");
        } else {
            uint8_t opcode = 0;
            std::string payload;
            if (!websocket_read_frame(ws, opcode, payload) || opcode != 1 || payload != "welcome") {
                rc = fail("websocket welcome: opcode=" + std::to_string(opcode) + " payload=[" + payload + "]");
            } else if (!send_all(ws, websocket_client_frame(1, "ping"))) {
                rc = fail("websocket send failed");
            } else if (!websocket_read_frame(ws, opcode, payload) || opcode != 1 || payload != "echo:ping") {
                rc = fail("websocket echo: opcode=" + std::to_string(opcode) + " payload=[" + payload + "]");
            }
            ::close(ws);
        }
    }
    // WebSocket maxMessageBytes overflow closes with 1009.
    if (rc == 0) {
        int ws = websocket_connect(port);
        if (ws < 0) {
            rc = fail("websocket overflow handshake failed");
        } else {
            uint8_t opcode = 0;
            std::string payload;
            (void)websocket_read_frame(ws, opcode, payload); // welcome
            if (!send_all(ws, websocket_client_frame(1, std::string(80, 'x')))) {
                rc = fail("websocket overflow send failed");
            } else if (!websocket_read_frame(ws, opcode, payload) || opcode != 8
                || websocket_close_code(payload) != 1009) {
                rc = fail("websocket overflow close: opcode=" + std::to_string(opcode)
                    + " code=" + std::to_string(websocket_close_code(payload)));
            }
            ::close(ws);
        }
    }

    // Streaming route: one long-lived chunked response carrying several writes,
    // finished automatically when the JS handler returns.
    if (rc == 0) {
        if (!http_request(port, "GET", "/events", "", response)) {
            rc = fail("stream request failed");
        } else if (status_of(response) != 200) {
            rc = fail("stream status: " + std::to_string(status_of(response)));
        } else if (header_of(response, "transfer-encoding").find("chunked") == std::string::npos) {
            rc = fail("stream transfer-encoding: [" + header_of(response, "transfer-encoding") + "]");
        } else if (header_of(response, "content-type").find("text/event-stream") == std::string::npos) {
            rc = fail("stream content-type: [" + header_of(response, "content-type") + "]");
        } else {
            const std::string body = body_of(response); // raw chunked framing
            if (body.find("data: one") == std::string::npos || body.find("data: two") == std::string::npos
                || body.find("data: three") == std::string::npos) {
                rc = fail("stream body missing chunks: [" + body + "]");
            }
        }
    }
    // respond() twice rejects with a stable code (reported through the stream).
    if (rc == 0) {
        if (!http_request(port, "GET", "/respond-twice", "", response)
            || body_of(response).find("code=http_invalid_argument") == std::string::npos) {
            rc = fail("respond-twice body: [" + body_of(response) + "]");
        }
    }
    // Client disconnect mid-stream is observed by the producer: open the endless
    // /drip stream, read a little, hang up, then poll /drip-status until the
    // server reports the stream closed.
    if (rc == 0) {
        int fd = connect_with_retry(port, 1);
        if (fd < 0) {
            rc = fail("drip connect failed");
        } else {
            std::string req = "GET /drip HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
            if (!send_all(fd, req)) {
                rc = fail("drip send failed");
            } else {
                char buf[512];
                ssize_t n = ::recv(fd, buf, sizeof(buf), 0); // headers + first ticks
                if (n <= 0) {
                    rc = fail("drip read failed");
                }
            }
            ::close(fd);
        }
        if (rc == 0) {
            bool closedSeen = false;
            for (int i = 0; i < 50 && !closedSeen; ++i) {
                if (http_request(port, "GET", "/drip-status", "", response)
                    && body_of(response) == "closed") {
                    closedSeen = true;
                } else {
                    usleep(100 * 1000);
                }
            }
            if (!closedSeen) {
                rc = fail("drip stream did not observe the client disconnect");
            }
        }
    }

    ::kill(child, SIGTERM);
    int status = 0;
    ::waitpid(child, &status, 0);

    if (rc == 0) {
        std::cout << "wl2_restinio fixture ok\n";
    }
    return rc;
}
