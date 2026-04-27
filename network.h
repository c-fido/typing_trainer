// POSIX BSD socket networking for head-to-head multiplayer.
// Protocol: uint32_t per message.
//   bit 31 = 0  →  position update (bits 0-30 = char index)
//   bit 31 = 1  →  done signal    (bits 0-30 = WPM * 10)

#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <string>

class Network {
public:
    int fd = -1;

    // Encoding helpers
    static uint32_t mkDone(float wpm) {
        return 0x80000000u | ((uint32_t)(wpm * 10.0f) & 0x7FFFFFFFu);
    }
    static bool isDone(uint32_t v) { return (v >> 31) != 0; }
    static float doneWPM(uint32_t v) { return (float)(v & 0x7FFFFFFFu) / 10.0f; }
    static uint32_t mkPos(uint32_t pos) { return pos & 0x7FFFFFFFu; }

    // Host: bind port, wait for one client connection.
    bool hostAndWait(int port = 7777) {
        int srv = ::socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) return false;
        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons((uint16_t)port);
        if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) { ::close(srv); return false; }
        if (listen(srv, 1) < 0)                            { ::close(srv); return false; }
        fd = accept(srv, nullptr, nullptr);
        ::close(srv);
        return fd >= 0;
    }

    // Client: connect to host.
    bool connectTo(const std::string& ip, int port = 7777) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((uint16_t)port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) { close(); return false; }
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0)     { close(); return false; }
        return true;
    }

    // Send the race line (host → client).
    bool sendLine(const std::string& line) {
        uint32_t len = (uint32_t)line.size();
        return sendAll(&len, sizeof(len)) && sendAll(line.data(), len);
    }

    // Receive the race line (client ← host).
    std::string recvLine() {
        uint32_t len = 0;
        if (!recvAll(&len, sizeof(len)) || len == 0 || len > 4096) return "";
        std::string s(len, '\0');
        if (!recvAll(s.data(), len)) return "";
        return s;
    }

    bool sendU32(uint32_t v) { return sendAll(&v, sizeof(v)); }

    // Blocking receive.
    bool recvU32(uint32_t& v) { return recvAll(&v, sizeof(v)); }

    void close() { if (fd >= 0) { ::close(fd); fd = -1; } }
    ~Network() { close(); }

private:
    bool sendAll(const void* data, size_t len) {
        const char* p = static_cast<const char*>(data);
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd, p + sent, len - sent, 0);
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    bool recvAll(void* data, size_t len) {
        char* p = static_cast<char*>(data);
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::recv(fd, p + got, len - got, 0);
            if (n <= 0) return false;
            got += (size_t)n;
        }
        return true;
    }
};
