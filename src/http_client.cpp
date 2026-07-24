#include "http_client.hpp"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace browservice {

namespace {

// Parse an HTTP URL of the form http://host:port/path into host, port, and path.
// Defaults: port=80, path="/". Returns false if the URL is not a valid http:// URL.
bool parseHttpUrl(const string& url, string& host, int& port, string& path) {
    const string prefix = "http://";
    if(url.size() <= prefix.size() || url.substr(0, prefix.size()) != prefix) {
        return false;
    }

    size_t hostStart = prefix.size();
    size_t pathPos = url.find('/', hostStart);

    string hostPort;
    if(pathPos == string::npos) {
        hostPort = url.substr(hostStart);
        path = "/";
    } else {
        hostPort = url.substr(hostStart, pathPos - hostStart);
        path = url.substr(pathPos);
        if(path.empty()) {
            path = "/";
        }
    }

    // Split hostPort into host and port at the last ':' (IPv6 not supported —
    // our use case is LAN IPs and hostnames).
    size_t colonPos = hostPort.find(':');
    if(colonPos == string::npos) {
        host = hostPort;
        port = 80;
    } else {
        host = hostPort.substr(0, colonPos);
        optional<int> parsedPort = parseString<int>(hostPort.substr(colonPos + 1));
        if(!parsedPort.has_value()) {
            return false;
        }
        port = parsedPort.value();
    }

    if(host.empty()) {
        return false;
    }

    return true;
}

void sendHttpGetSync(string url) {
    string host;
    int port;
    string path;
    if(!parseHttpUrl(url, host, port, path)) {
        WARNING_LOG("VLC helper request failed: invalid URL: ", url);
        return;
    }

    // Resolve hostname
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* addrInfo = nullptr;
    int gaiResult = getaddrinfo(host.c_str(), nullptr, &hints, &addrInfo);
    if(gaiResult != 0 || addrInfo == nullptr) {
        WARNING_LOG(
            "VLC helper request failed: failed to resolve host '",
            host, "': ", gai_strerror(gaiResult)
        );
        if(addrInfo != nullptr) {
            freeaddrinfo(addrInfo);
        }
        return;
    }

    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) {
        WARNING_LOG("VLC helper request failed: socket() failed: ", strerror(errno));
        freeaddrinfo(addrInfo);
        return;
    }

    // Set 5-second send timeout so connect/send don't hang indefinitely
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Build sockaddr_in from resolved address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    struct sockaddr_in* resolved = (struct sockaddr_in*)addrInfo->ai_addr;
    addr.sin_family = AF_INET;
    addr.sin_addr = resolved->sin_addr;
    addr.sin_port = htons(port);

    // Connect
    if(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        WARNING_LOG(
            "VLC helper request failed: connect() to ",
            host, ":", port, " failed: ", strerror(errno)
        );
        close(sock);
        freeaddrinfo(addrInfo);
        return;
    }

    freeaddrinfo(addrInfo);

    // Send HTTP GET request (HTTP/1.0 for simplicity — no keep-alive)
    string request =
        "GET " + path + " HTTP/1.0\r\n"
        "Host: " + host + "\r\n"
        "Connection: close\r\n"
        "\r\n";

    ssize_t sentTotal = 0;
    while(sentTotal < (ssize_t)request.size()) {
        ssize_t sent = send(sock, request.data() + sentTotal, request.size() - sentTotal, 0);
        if(sent <= 0) {
            WARNING_LOG(
                "VLC helper request failed: send() failed: ",
                strerror(errno)
            );
            close(sock);
            return;
        }
        sentTotal += sent;
    }

    // Read and discard response; check status line for non-200
    string response;
    char buf[4096];
    ssize_t received;
    bool statusChecked = false;
    while((received = recv(sock, buf, sizeof(buf), 0)) > 0) {
        if(!statusChecked) {
            response.append(buf, received);
            if(response.find("\r\n") != string::npos) {
                // Parse the status line: "HTTP/1.0 200 OK"
                size_t spacePos = response.find(' ');
                if(spacePos != string::npos) {
                    size_t space2Pos = response.find(' ', spacePos + 1);
                    string statusCode = response.substr(
                        spacePos + 1,
                        (space2Pos == string::npos ? string::npos : space2Pos) - spacePos - 1
                    );
                    optional<int> codeNum = parseString<int>(statusCode);
                    if(codeNum.has_value() && codeNum.value() != 200) {
                        WARNING_LOG(
                            "VLC helper returned non-200 status: ",
                            codeNum.value()
                        );
                    }
                }
                statusChecked = true;
            }
        }
    }

    close(sock);
}

} // namespace

void sendHttpGetAsync(string url) {
    // Fire-and-forget in a detached thread so the CEF UI thread is not blocked.
    // The URL is captured by value.
    std::thread([url = std::move(url)]() {
        sendHttpGetSync(url);
    }).detach();
}

} // namespace browservice
