#include "../include/RedisServer.h"
#include "../include/RedisDatabase.h"
#include "../include/Logger.h"

#include <thread>
#include <iostream>
#include <chrono>
#include <atomic>

// Global shutdown flag for persistence worker
static std::atomic<bool> persistRunning(true);

int main(int argc, char *argv[])
{
    Logger::getInstance().info("Redis server starting...");

    int port = 6379;
    if (argc >= 2)
        port = std::stoi(argv[1]);

    // ---------------------------------------------------------------------
    // Database Load
    // ---------------------------------------------------------------------
    if (RedisDatabase::getInstance().load("dump.my_rdb"))
    {
        std::cout << "Database Loaded From dump.my_rdb\n";
        Logger::getInstance().info("Database loaded from dump.my_rdb");
    }
    else
    {
        std::cout << "No dump found or load failed; starting with an empty database.\n";
        Logger::getInstance().info("No dump found or load failed. New empty DB.");
    }

    // ---------------------------------------------------------------------
    // Start Server
    // ---------------------------------------------------------------------
    RedisServer server(port);
    Logger::getInstance().info("Server initialized on port " + std::to_string(port));

    // ---------------------------------------------------------------------
    // Persistence Thread (Phase-5 optimized)
    // ---------------------------------------------------------------------
    std::thread persistenceThread([]()
                                  {
        Logger::getInstance().info("Persistence worker started");

        while (persistRunning.load(std::memory_order_relaxed))
        {
            for (int i = 0; i < 300; ++i)
            {
                if (!persistRunning.load(std::memory_order_relaxed))
                    break;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!persistRunning.load(std::memory_order_relaxed))
                break;

            if (!RedisDatabase::getInstance().dump("dump.my_rdb"))
                Logger::getInstance().error("Error dumping database");
            else
                Logger::getInstance().info("Database dumped to dump.my_rdb");
        }

        Logger::getInstance().info("Persistence worker stopped"); });

    persistenceThread.detach();
    Logger::getInstance().info("Persistence thread detached");

    // ---------------------------------------------------------------------
    // Run Main Server (blocking)
    // ---------------------------------------------------------------------
    Logger::getInstance().info("Server is now running...");
    server.run();

    // ---------------------------------------------------------------------
    // Shutdown sequence
    // ---------------------------------------------------------------------
    persistRunning.store(false, std::memory_order_relaxed);

    Logger::getInstance().info("Server shutting down.");
    return 0;
}
