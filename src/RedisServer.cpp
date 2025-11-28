// src/RedisServer.cpp
#include "../include/RedisServer.h"
#include "../include/ThreadPool.h"
#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"
#include "../include/Logger.h"

#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <atomic>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cerrno>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
using socket_errno_t = int;
#else
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netinet/tcp.h>
#define CLOSE_SOCKET close
using socket_errno_t = int;
#endif

/* ============================================================
   GLOBAL POINTER FOR SIGNAL HANDLER
   ============================================================ */
static RedisServer *globalServer = nullptr;

#ifndef _WIN32
static void signalHandler(int)
{
    if (globalServer)
        globalServer->shutdown();
}
#endif

/* ============================================================
   HEX PREVIEW FOR LOGS
   ============================================================ */
static std::string buffer_preview_hex(const std::string &buf, size_t max_bytes = 64)
{
    std::ostringstream ss;
    size_t n = std::min(buf.size(), max_bytes);
    ss << std::hex << std::setfill('0');

    for (size_t i = 0; i < n; ++i)
    {
        ss << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(buf[i]));
        if (i + 1 < n)
            ss << ' ';
    }
    if (buf.size() > n)
        ss << " ...";
    return ss.str();
}

/* ============================================================
   SIGNAL HANDLER SETUP
   ============================================================ */
void RedisServer::setupSignalHandler()
{
#ifndef _WIN32
    signal(SIGINT, signalHandler);
    signal(SIGPIPE, SIG_IGN);
#endif
}

/* ============================================================
   CONSTRUCTOR (MERGED)
   ============================================================ */
RedisServer::RedisServer(int port)
    : port(port),
      server_socket(INVALID_SOCKET_T),
      running(true),
      thread_pool(std::make_unique<ThreadPool>(std::thread::hardware_concurrency()))
{
    globalServer = this;
    setupSignalHandler();

    Logger::getInstance().info(
        "Thread pool initialized with " +
        std::to_string(std::thread::hardware_concurrency()) +
        " threads");
}

/* ============================================================
   DESTRUCTOR
   ============================================================ */
RedisServer::~RedisServer()
{
    shutdown();
}

/* ============================================================
   SERVER SHUTDOWN
   ============================================================ */
void RedisServer::shutdown()
{
    running.store(false, std::memory_order_release);

    if (server_socket != INVALID_SOCKET_T)
    {
        CLOSE_SOCKET(server_socket);
        server_socket = INVALID_SOCKET_T;
    }

    if (thread_pool)
    {
        Logger::getInstance().info("Shutting down thread pool...");
        thread_pool->shutdown();
        Logger::getInstance().info("Thread pool shut down complete");
    }

    RedisDatabase::getInstance().dump("dump.my_rdb");
    Logger::getInstance().info("Server shut down gracefully");
}

/* ============================================================
   MAIN SERVER LOOP (MERGED WITH THREADPOOL)
   ============================================================ */
void RedisServer::run()
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        Logger::getInstance().error("WSAStartup failed");
        return;
    }
#endif

    /* ------------------------------
       Create Socket
       ------------------------------ */
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET_T)
    {
        Logger::getInstance().error("Socket creation failed");
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));
#else
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        Logger::getInstance().error("Bind failed");
        CLOSE_SOCKET(server_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    if (listen(server_socket, 128) < 0)
    {
        Logger::getInstance().error("Listen failed");
        CLOSE_SOCKET(server_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    std::cout << "Redis Server running on port " << port << "\n";
    Logger::getInstance().info("Server accepting connections");

    /* ------------------------------
       ACCEPT LOOP
       ------------------------------ */
    while (running.load(std::memory_order_acquire))
    {
        socket_t client_fd = accept(server_socket, nullptr, nullptr);

#ifdef _WIN32
        if (client_fd == INVALID_SOCKET)
#else
        if (client_fd < 0)
#endif
        {
            if (!running.load())
                break;

#ifdef _WIN32
            int wsa = WSAGetLastError();
            Logger::getInstance().warn("Accept failed WSA=" + std::to_string(wsa));
#else
            Logger::getInstance().warn("Accept failed errno=" + std::to_string(errno));
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        /* Submit client to Thread Pool (capture by value) */
        try
        {
            thread_pool->enqueue([this, client_fd]()
                                 { handleClient(client_fd); });
        }
        catch (const std::exception &ex)
        {
            Logger::getInstance().error(std::string("ThreadPool enqueue failed: ") + ex.what());
            CLOSE_SOCKET(client_fd);
        }
        catch (...)
        {
            Logger::getInstance().error("ThreadPool enqueue unknown failure");
            CLOSE_SOCKET(client_fd);
        }
    }

    Logger::getInstance().info("Accept loop exited");

#ifdef _WIN32
    WSACleanup();
#endif
}

/* ============================================================
   CLIENT HANDLER (MERGED)
   ============================================================ */
void RedisServer::handleClient(socket_t client_fd)
{
    RedisCommandHandler handler;

    std::string buffer;
    buffer.reserve(4096);
    const size_t MAX_BUFFER = 4 * 1024 * 1024;

    std::vector<char> recvbuf(8192);

    /* ------------------------------ TCP_NODELAY ------------------------------ */
#ifdef _WIN32
    {
        char flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    }
#else
    {
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    }
#endif

    /* -------------------------------------------------------------------------
       SET SOCKET TIMEOUTS (prevents hung/stalled clients)
       ------------------------------------------------------------------------- */
#ifdef _WIN32
    {
        DWORD timeout = 30000; // 30 seconds
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&timeout, sizeof(timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&timeout, sizeof(timeout));
    }
#else
    {
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#endif

    Logger::getInstance().debug("Socket timeouts configured (30s)");

    /* -------------------------------------------------------------------------
       SET KEEPALIVE (detect dead TCP connections)
       ------------------------------------------------------------------------- */
#ifdef _WIN32
    {
        char keepalive = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE,
                   &keepalive, sizeof(keepalive));
    }
#else
    {
        int keepalive = 1;
        int keepidle = 60;  // Start keepalive after 60s idle
        int keepintvl = 10; // Send keepalive probe every 10s
        int keepcnt = 3;    // 3 failed probes → dead

        setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE,
                   &keepalive, sizeof(keepalive));
        setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE,
                   &keepidle, sizeof(keepidle));
        setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL,
                   &keepintvl, sizeof(keepintvl));
        setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT,
                   &keepcnt, sizeof(keepcnt));
    }
#endif

    Logger::getInstance().debug("TCP keepalive configured");

    /* ------------------------------ SEND ALL ------------------------------ */
    auto sendAll = [&](socket_t fd, const std::string &data) -> bool
    {
        size_t sent_total = 0;
        size_t remaining = data.size();
        const char *ptr = data.data();

        while (remaining > 0)
        {
            // clamp write size
            size_t chunk_sz = remaining;
            if (chunk_sz > static_cast<size_t>(INT_MAX))
                chunk_sz = static_cast<size_t>(INT_MAX);

#ifdef _WIN32
            int sent = ::send(fd, ptr + sent_total, static_cast<int>(chunk_sz), 0);
            if (sent == SOCKET_ERROR)
            {
                int we = WSAGetLastError();
                if (we == WSAEWOULDBLOCK || we == WSAEINTR || we == WSAEINPROGRESS)
                {
                    // transient, retry a few times
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                Logger::getInstance().warn("send failed WSA=" + std::to_string(we));
                return false;
            }
            sent_total += static_cast<size_t>(sent);
#else
            ssize_t sent = ::send(fd, ptr + sent_total, chunk_sz, 0);
            if (sent < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // transient, retry
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                Logger::getInstance().warn("send failed errno=" + std::to_string(errno));
                return false;
            }
            if (sent == 0)
            {
                Logger::getInstance().warn("send returned 0 (peer closed?)");
                return false;
            }
            sent_total += static_cast<size_t>(sent);
#endif
            remaining = data.size() - sent_total;
        }
        return true;
    };

    /* ------------------------------ READ LOOP ------------------------------ */
    while (true)
    {
#ifdef _WIN32
        int bytes = ::recv(client_fd, recvbuf.data(), static_cast<int>(recvbuf.size()), 0);
        if (bytes == 0)
        {
            Logger::getInstance().info("client closed connection");
            break;
        }
        if (bytes == SOCKET_ERROR)
        {
            int we = WSAGetLastError();
            // treat timeouts / would-block / interrupts as transient: continue to next loop iteration
            if (we == WSAEWOULDBLOCK || we == WSAEINTR || we == WSAETIMEDOUT)
            {
                Logger::getInstance().debug("recv transient WSA=" + std::to_string(we));
                // allow server to keep connection alive a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            Logger::getInstance().warn("recv failed WSA=" + std::to_string(we));
            break;
        }
#else
        ssize_t bytes = ::recv(client_fd, recvbuf.data(), recvbuf.size(), 0);
        if (bytes == 0)
        {
            Logger::getInstance().info("client closed connection");
            break;
        }
        if (bytes < 0)
        {
            if (errno == EINTR)
            {
                // signal - retry
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                Logger::getInstance().debug("recv would block (EAGAIN/EWOULDBLOCK), continuing");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            Logger::getInstance().warn("recv failed errno=" + std::to_string(errno));
            break;
        }
#endif

        // append bytes read to input buffer
#ifdef _WIN32
        buffer.append(recvbuf.data(), static_cast<size_t>(bytes));
#else
        buffer.append(recvbuf.data(), static_cast<size_t>(bytes));
#endif

        if (buffer.size() > MAX_BUFFER)
        {
            Logger::getInstance().warn("payload too large (" + std::to_string(buffer.size()) + ")");
            sendAll(client_fd, "-ERR payload too large\r\n");
            break;
        }

        std::vector<std::string> frames;
        try
        {
            frames = handler.splitFrames(buffer);
        }
        catch (const std::exception &ex)
        {
            // <<-- CHANGED: do not close connection on parser exception that might be caused
            // by partial input. Log and continue to receive more data.
            Logger::getInstance().warn("Parse error (will wait for more data): " + std::string(ex.what()) +
                                       " hex=" + buffer_preview_hex(buffer));
            // Wait for more data instead of closing the socket immediately.
            // This is important for normal TCP fragmentation: do not treat parser exceptions as fatal.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        catch (...)
        {
            // <<-- CHANGED: do not close connection on unknown parse error either.
            Logger::getInstance().warn("Unknown parse error (will wait for more data) hex=" + buffer_preview_hex(buffer));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // If no complete frames were returned, this is normal: wait for more recv data.
        // <<-- CHANGED: explicitly treat empty frames as non-fatal
        if (frames.empty())
        {
            // nothing complete to process yet
            continue;
        }

        // Process all complete frames (if any)
        for (auto &frame : frames)
        {
            std::string resp;
            try
            {
                resp = handler.processCommand(frame);
            }
            catch (const std::exception &ex)
            {
                Logger::getInstance().warn("processCommand threw: " + std::string(ex.what()));
                resp = "-ERR internal error\r\n";
            }
            catch (...)
            {
                Logger::getInstance().warn("processCommand threw unknown exception");
                resp = "-ERR internal error\r\n";
            }

            if (!sendAll(client_fd, resp))
                goto close_conn;
        }
    }

close_conn:
#ifdef _WIN32
    ::shutdown(client_fd, SD_BOTH);
#else
    ::shutdown(client_fd, SHUT_RDWR);
#endif
    CLOSE_SOCKET(client_fd);
}
