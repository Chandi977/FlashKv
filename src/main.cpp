// main.cpp - Final merged version (production safe)

#include "../include/RedisServer.h"
#include "../include/RedisDatabase.h"
#include "../include/Logger.h"

#include <thread>
#include <iostream>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <string>
#include <mutex>

// --------------------------------------------------------------
// GLOBAL FLAGS
// --------------------------------------------------------------
static std::atomic<bool> persistRunning(true);
static std::condition_variable persistCv;
static std::mutex persistMtx;

// ---- Dump protection ----
static std::atomic<bool> dumpInProgress(false);
static std::mutex dumpMutex;

// On POSIX the server must be notified
#ifndef _WIN32
static std::atomic<RedisServer *> g_server_ptr(nullptr);
#endif

// --------------------------------------------------------------
// SAFE DUMP WRAPPER (prevents concurrent dump corruption)
// --------------------------------------------------------------
bool safeDump(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(dumpMutex);

    if (dumpInProgress.exchange(true))
    {
        Logger::getInstance().warn("Dump skipped (another dump in progress)");
        return false;
    }

    bool ok = RedisDatabase::getInstance().dump(filename);

    dumpInProgress.store(false);

    if (!ok)
        Logger::getInstance().error("Error dumping database to " + filename);
    else
        Logger::getInstance().info("Database successfully dumped to " + filename);

    return ok;
}

// --------------------------------------------------------------
// Shutdown helper
// --------------------------------------------------------------
static void requestShutdown(RedisServer *server)
{
    if (server)
        server->shutdown();

    persistRunning.store(false, std::memory_order_relaxed);
    persistCv.notify_all();
}

// --------------------------------------------------------------
// Signal handler (POSIX only)
// --------------------------------------------------------------
#ifndef _WIN32
static void sigintHandler(int)
{
    RedisServer *srv = g_server_ptr.load(std::memory_order_acquire);
    requestShutdown(srv);
}
#endif

// --------------------------------------------------------------
// MAIN ENTRY POINT
// --------------------------------------------------------------
int main(int argc, char *argv[])
{
    try
    {
        Logger::getInstance().info("Redis-like server starting...");

        int port = 6379;
        if (argc >= 2)
        {
            try
            {
                port = std::stoi(argv[1]);
            }
            catch (...)
            {
                Logger::getInstance().warn("Invalid port argument, using default 6379");
                port = 6379;
            }
        }

        // ----------------------------------------------------------
        // Load DB on startup
        // ----------------------------------------------------------
        if (RedisDatabase::getInstance().load("dump.my_rdb"))
        {
            std::cout << "Database loaded from dump.my_rdb\n";
            Logger::getInstance().info("Database loaded successfully");
        }
        else
        {
            std::cout << "No dump found or failed; starting empty DB\n";
            Logger::getInstance().info("Empty DB initialized");
        }

        // ----------------------------------------------------------
        // Start server
        // ----------------------------------------------------------
        RedisServer server(port);
        Logger::getInstance().info("Server initialized on port " + std::to_string(port));

#ifndef _WIN32
        g_server_ptr.store(&server, std::memory_order_release);
        std::signal(SIGINT, sigintHandler);
        std::signal(SIGTERM, sigintHandler);
#endif

        // ----------------------------------------------------------
        // Persistence Thread (every 300s)
        // ----------------------------------------------------------
        constexpr std::chrono::seconds INTERVAL(300);

        std::thread persistenceThread([&]()
                                      {
            Logger::getInstance().info("Persistence worker started (300s interval)");

            while (persistRunning.load(std::memory_order_relaxed))
            {
                std::unique_lock<std::mutex> lk(persistMtx);

                persistCv.wait_for(
                    lk,
                    INTERVAL,
                    []() { return !persistRunning.load(std::memory_order_relaxed); }
                );

                if (!persistRunning.load(std::memory_order_relaxed))
                    break;

                safeDump("dump.my_rdb");
            }

            Logger::getInstance().info("Persistence worker exiting"); });

        Logger::getInstance().info("Server fully running");

        // ----------------------------------------------------------
        // Run server (blocking)
        // ----------------------------------------------------------
        server.run();

        // ----------------------------------------------------------
        // Shutdown sequence
        // ----------------------------------------------------------
        Logger::getInstance().info("Main: server.run() returned, shutting down");

        persistRunning.store(false, std::memory_order_relaxed);
        persistCv.notify_all();

        if (persistenceThread.joinable())
            persistenceThread.join();

        Logger::getInstance().info("Performing final DB dump...");
        safeDump("dump.my_rdb");

        Logger::getInstance().info("Shutdown complete");
        Logger::getInstance().shutdown();
    }
    catch (const std::exception &ex)
    {
        Logger::getInstance().error(std::string("Fatal exception: ") + ex.what());
        return EXIT_FAILURE;
    }
    catch (...)
    {
        Logger::getInstance().error("Unknown fatal exception");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
