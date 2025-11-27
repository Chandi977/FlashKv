#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sstream>
#include <mutex>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#endif

// ====================================================
// RESP Helper
// ====================================================
std::string buildRESP(const std::vector<std::string> &parts)
{
    std::ostringstream oss;
    oss << "*" << parts.size() << "\r\n";
    for (auto &p : parts)
    {
        oss << "$" << p.size() << "\r\n"
            << p << "\r\n";
    }
    return oss.str();
}

// ====================================================
// Single Client Worker
// ====================================================
struct Stats
{
    std::atomic<long long> totalOps = 0;
    std::atomic<long long> totalLatencyMicro = 0;
    std::atomic<int> timeouts = 0;
    std::atomic<int> failures = 0;
};

void clientWorker(int id, const std::string &host, int port,
                  int ops, const std::string &mode, Stats *stats)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        stats->failures++;
        return;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(host.c_str());

    if (connect(sock, (sockaddr *)&server, sizeof(server)) < 0)
    {
        stats->failures++;
        CLOSE_SOCKET(sock);
        return;
    }

    char buffer[2048];

    for (int i = 0; i < ops; i++)
    {
        std::string cmd;
        if (mode == "ping")
        {
            cmd = buildRESP({"PING"});
        }
        else if (mode == "sets")
        {
            cmd = buildRESP({"SET", "key_" + std::to_string(id) + "_" + std::to_string(i), "value123"});
        }
        else if (mode == "gets")
        {
            cmd = buildRESP({"GET", "key_" + std::to_string(id) + "_" + std::to_string(i)});
        }
        else
        { // mixed set/get
            bool isSet = (i % 2 == 0);
            std::string key = "mkey_" + std::to_string(id) + "_" + std::to_string(i % 200);

            if (isSet)
                cmd = buildRESP({"SET", key, "valueXYZ"});
            else
                cmd = buildRESP({"GET", key});
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        send(sock, cmd.c_str(), cmd.size(), 0);

        int n = recv(sock, buffer, sizeof(buffer), 0);
        auto t1 = std::chrono::high_resolution_clock::now();

        if (n <= 0)
        {
            stats->timeouts++;
            continue;
        }

        long long us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        stats->totalLatencyMicro += us;
        stats->totalOps++;
    }

    CLOSE_SOCKET(sock);

#ifdef _WIN32
    WSACleanup();
#endif
}

// ====================================================
// MAIN – Stress Test Controller
// ====================================================
int main(int argc, char *argv[])
{
    // Config
    std::string host = "127.0.0.1";
    int port = 6379;
    int clients = 50;
    int ops = 200;
    std::string mode = "setget";

    if (argc >= 2)
        clients = std::stoi(argv[1]);
    if (argc >= 3)
        ops = std::stoi(argv[2]);
    if (argc >= 4)
        mode = argv[3];

    std::cout << "=== C++ Redis Stress Tester ===\n";
    std::cout << "Clients: " << clients << "\n";
    std::cout << "Ops/client: " << ops << "\n";
    std::cout << "Mode: " << mode << "\n\n";

    Stats stats;
    std::vector<std::thread> threads;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < clients; i++)
    {
        threads.emplace_back(clientWorker, i + 1, host, port, ops, mode, &stats);
    }

    for (auto &t : threads)
        t.join();

    auto end = std::chrono::high_resolution_clock::now();

    double sec = std::chrono::duration<double>(end - start).count();
    long long totalOps = stats.totalOps.load();

    std::cout << "Total ops: " << totalOps << "\n";
    std::cout << "Time: " << sec << " sec\n";
    std::cout << "OPS/sec: " << (totalOps / sec) << "\n";

    if (totalOps > 0)
    {
        double avgLatencyMs = (stats.totalLatencyMicro / 1000.0) / totalOps;
        std::cout << "Avg Latency: " << avgLatencyMs << " ms\n";
    }

    std::cout << "Timeouts: " << stats.timeouts << "\n";
    std::cout << "Failures: " << stats.failures << "\n";

    std::cout << "=====================================\n";
    return 0;
}
