
// ThreadPool.h - Add this new header
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

class ThreadPool
{
public:
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency())
        : stopping(false)
    {
        workers.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i)
        {
            workers.emplace_back([this]
                                 { workerThread(); });
        }
    }

    ~ThreadPool()
    {
        shutdown();
    }

    void enqueue(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (stopping.load())
                return;
            tasks.push(std::move(task));
        }
        condition.notify_one();
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (stopping.load())
                return;
            stopping.store(true);
        }
        condition.notify_all();

        for (auto &worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
    }

    size_t activeThreads() const
    {
        return active_count.load();
    }

private:
    void workerThread()
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                condition.wait(lock, [this]
                               { return stopping.load() || !tasks.empty(); });

                if (stopping.load() && tasks.empty())
                {
                    return;
                }

                if (!tasks.empty())
                {
                    task = std::move(tasks.front());
                    tasks.pop();
                }
            }

            if (task)
            {
                active_count.fetch_add(1);
                try
                {
                    task();
                }
                catch (...)
                {
                    // Log exception but don't crash worker
                }
                active_count.fetch_sub(1);
            }
        }
    }

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stopping;
    std::atomic<size_t> active_count{0};
};

#endif // THREAD_POOL_H

// ============================================
// RedisServer.h - Updated
// ============================================
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
    ~RedisServer(); // Add destructor

    void run();
    void shutdown();

private:
    int port = 0;
    socket_t server_socket = INVALID_SOCKET_T;
    std::atomic<bool> running{false};
    std::unique_ptr<ThreadPool> thread_pool; // Use thread pool

    void setupSignalHandler();
    void handleClient(socket_t client_fd);
};

#endif