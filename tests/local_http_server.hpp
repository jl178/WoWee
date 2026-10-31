#pragma once

/// A canned HTTP server on 127.0.0.1 for tests that exercise http_fetch
/// without a network: every path maps to a fixed reply. Runs its accept
/// loop on a thread and answers one request per connection, which is all
/// the client ever sends.

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "network/net_platform.hpp"

class LocalHttpServer {
public:
    struct Reply {
        int status = 200;
        std::string body;
        bool chunked = false;
        /// Extra header lines, each ending in CRLF.
        std::string headers;
        /// Lie about the length: send this many fewer bytes than declared.
        std::size_t truncate = 0;
    };

    LocalHttpServer() {
        wowee::net::ensureInit();
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listener_, 8);
        socklen_t len = sizeof(addr);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { loop(); });
    }

    ~LocalHttpServer() {
        stop_.store(true);
        // Connecting to ourselves wakes accept() so the loop can see the flag.
        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port_);
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        wowee::net::closeSocket(s);
        if (thread_.joinable()) thread_.join();
        wowee::net::closeSocket(listener_);
    }

    void set(const std::string& path, Reply reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[path] = std::move(reply);
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    int requests() const { return requests_.load(); }

private:
    void loop() {
        while (!stop_.load()) {
            socket_t c = ::accept(listener_, nullptr, nullptr);
            if (c == INVALID_SOCK) continue;
            if (stop_.load()) {
                wowee::net::closeSocket(c);
                break;
            }
            serve(c);
            wowee::net::closeSocket(c);
        }
    }

    void serve(socket_t c) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const int n = static_cast<int>(::recv(c, buf, sizeof(buf), 0));
            if (n <= 0) return;
            req.append(buf, static_cast<std::size_t>(n));
        }
        requests_.fetch_add(1);
        const std::size_t s1 = req.find(' ');
        const std::size_t s2 = req.find(' ', s1 + 1);
        const std::string path = req.substr(s1 + 1, s2 - s1 - 1);
        Reply reply;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = replies_.find(path);
            if (it == replies_.end()) {
                reply.status = 404;
                reply.body = "not here";
            } else {
                reply = it->second;
            }
        }
        std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\n" + reply.headers;
        if (reply.chunked) {
            out += "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
            // Two chunks when there is enough body, so the decoder has to
            // join across a boundary.
            const std::size_t half = reply.body.size() / 2;
            for (const std::string& piece : {reply.body.substr(0, half), reply.body.substr(half)}) {
                if (piece.empty()) continue;
                char hex[32];
                std::snprintf(hex, sizeof(hex), "%zx\r\n", piece.size());
                out += hex + piece + "\r\n";
            }
            out += "0\r\n\r\n";
        } else {
            out += "Content-Length: " + std::to_string(reply.body.size()) + "\r\nConnection: close\r\n\r\n";
            out += reply.body.substr(0, reply.body.size() - reply.truncate);
        }
        std::size_t sent = 0;
        while (sent < out.size()) {
            const int n = static_cast<int>(::send(c, out.data() + sent, static_cast<int>(out.size() - sent), 0));
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
    }

    socket_t listener_ = INVALID_SOCK;
    unsigned short port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> requests_{0};
    std::mutex mutex_;
    std::map<std::string, Reply> replies_;
};
