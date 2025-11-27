#ifndef REDIS_SERVER_H
#define REDIS_SERVER_H

#include <string>
#include <atomic>

/*
  RedisServer
  --------------------------------------
  - Clean and minimal public interface
  - Atomic running flag for thread-safe shutdown
  - Prepped for Phase-6: epoll/select optimization
  - No unnecessary includes
*/

class RedisServer
{
public:
    explicit RedisServer(int port);

    // Starts blocking accept() loop (multi-threaded clients)
    void run();

    // Graceful shutdown (thread-safe)
    void shutdown();

private:
    int port = 0;
    int server_socket = -1;
    std::atomic<bool> running{false};

    // For Ctrl+C, SIGINT, etc.
    void setupSignalHandler();
};

#endif
