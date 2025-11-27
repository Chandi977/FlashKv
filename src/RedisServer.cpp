#include "../include/RedisServer.h"
#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"
#include "../include/Logger.h"

#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#define CLOSE_SOCKET close
#include <signal.h>
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
   INSTALL SIGNAL HANDLER
============================================================ */
void RedisServer::setupSignalHandler()
{
#ifndef _WIN32
    signal(SIGINT, signalHandler);
#endif
}

/* ============================================================
   CONSTRUCTOR
============================================================ */
RedisServer::RedisServer(int port)
    : port(port), server_socket(-1), running(true)
{
    globalServer = this;
    setupSignalHandler();
}

/* ============================================================
   SHUTDOWN
============================================================ */
void RedisServer::shutdown()
{
    running.store(false, std::memory_order_release);

    if (server_socket != -1)
        CLOSE_SOCKET(server_socket);

    RedisDatabase::getInstance().dump("dump.my_rdb");

    std::cout << "Server shut down gracefully.\n";
}

/* ============================================================
   MAIN RUN LOOP
============================================================ */
void RedisServer::run()
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        Logger::getInstance().error("WSAStartup failed");
        return;
    }
#endif

    /* ------------------------------
       Create Socket
    ------------------------------ */
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        Logger::getInstance().error("Socket creation failed");
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

    if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        Logger::getInstance().error("Bind failed");
        return;
    }

    if (listen(server_socket, 128) < 0)
    {
        Logger::getInstance().error("Listen failed");
        return;
    }

    std::cout << "Redis Server running on port " << port << "\n";

    /* =====================================================================
       Use ONE command handler per thread (avoid shared data issues)
    ====================================================================== */
    while (running.load(std::memory_order_acquire))
    {
        int client_fd;

#ifdef _WIN32
        client_fd = accept(server_socket, nullptr, nullptr);
        if (client_fd == INVALID_SOCKET)
#else
        client_fd = accept(server_socket, nullptr, nullptr);
        if (client_fd < 0)
#endif
        {
            if (!running.load())
                break;
            Logger::getInstance().error("Accept failed");
            continue;
        }

        /* =====================================================================
           CLIENT THREAD
        ====================================================================== */
        std::thread([client_fd]()
                    {
                        RedisCommandHandler handler; // each thread gets its own handler
                        std::string buffer;
                        buffer.reserve(4096);

                        char recvbuf[2048];

                        while (true)
                        {
#ifdef _WIN32
                            int bytes = recv(client_fd, recvbuf, sizeof(recvbuf), 0);
#else
                            int bytes = read(client_fd, recvbuf, sizeof(recvbuf));
#endif
                            if (bytes <= 0)
                                break;

                            buffer.append(recvbuf, bytes);

                            /* -------------------------
                               Support pipelining
                            -------------------------- */
                            auto frames = handler.splitFrames(buffer);
                            for (auto &frame : frames)
                            {
                                std::string response = handler.processCommand(frame);

#ifdef _WIN32
                                send(client_fd, response.c_str(), (int)response.size(), 0);
#else
                                send(client_fd, response.c_str(), response.size(), 0);
#endif
                            }
                        }

                        CLOSE_SOCKET(client_fd);
                    })
            .detach();
    }

    /* ============================================================
       FINAL DB DUMP
    ============================================================ */
    RedisDatabase::getInstance().dump("dump.my_rdb");

#ifdef _WIN32
    WSACleanup();
#endif
}
