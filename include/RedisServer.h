#ifndef REDIS_SERVER_H
#define REDIS_SERVER_H

#include <string>
#include <atomic>
#include <memory>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t INVALID_SOCKET_T = INVALID_SOCKET;
#else
using socket_t = int;
static constexpr socket_t INVALID_SOCKET_T = -1;
#endif

class ThreadPool; // Forward declaration

class RedisServer
{
public:
  explicit RedisServer(int port);
  ~RedisServer(); // Ensures proper cleanup

  // Start the server: blocking accept() loop
  void run();

  // Graceful shutdown: thread-safe
  void shutdown();

private:
  int port = 0;
  socket_t server_socket = INVALID_SOCKET_T;
  std::atomic<bool> running{false};

  std::unique_ptr<ThreadPool> thread_pool; // Multi-threading support

  void setupSignalHandler();
  void handleClient(socket_t client_fd);
};

#endif // REDIS_SERVER_H
