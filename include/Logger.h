#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

class Logger
{
public:
    enum class Level : int
    {
        DEBUG = 0,
        INFO = 1,
        WARN = 2,
        ERROR = 3,
        OFF = 4
    };

    // Get singleton
    static Logger &getInstance()
    {
        static Logger instance;
        return instance;
    }

    // Configure log level (thread-safe)
    void setLevel(Level lvl)
    {
        level.store(static_cast<int>(lvl), std::memory_order_relaxed);
    }

    Level getLevel() const
    {
        return static_cast<Level>(level.load(std::memory_order_relaxed));
    }

    // Public API - very cheap on hot path
    void debug(const std::string &msg) { log(Level::DEBUG, "", msg); }
    void info(const std::string &msg) { log(Level::INFO, "", msg); }
    void warn(const std::string &msg) { log(Level::WARN, "", msg); }
    void error(const std::string &msg) { log(Level::ERROR, "", msg); }

    // Existing names kept for compatibility
    void request(const std::string &client, const std::string &msg) { log(Level::DEBUG, client, msg, "REQUEST"); }
    void response(const std::string &client, const std::string &msg) { log(Level::DEBUG, client, msg, "RESPONSE"); }

    // Clean shutdown & flush (safe to call repeatedly)
    void shutdown()
    {
        bool expected = false;
        if (!stopping.compare_exchange_strong(expected, true))
            return; // already stopping

        {
            std::lock_guard<std::mutex> lk(queueMutex);
            queueCond.notify_one();
        }

        if (worker.joinable())
            worker.join();
    }

    // Non-copyable/movable
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

private:
    struct Item
    {
        std::chrono::system_clock::time_point tp;
        Level level;
        std::string client; // optional client tag
        std::string tag;    // optional tag like "REQUEST"/"RESPONSE"
        std::string msg;
    };

    // configurable parameters
    static constexpr size_t MAX_BATCH = 256;
    static constexpr std::chrono::milliseconds FLUSH_INTERVAL = std::chrono::milliseconds(200);
    static constexpr size_t MAX_QUEUE = 64 * 1024; // safety cap

    std::atomic<int> level{static_cast<int>(Level::INFO)};

    // queue + sync
    std::mutex queueMutex;
    std::condition_variable queueCond;
    std::deque<Item> queue;

    // worker & lifecycle
    std::thread worker;
    std::atomic<bool> stopping{false};

    // file rotation state (protected by fileMutex inside worker only)
    std::ofstream outFile;
    int currentYear{-1}, currentMonth{-1}, currentDay{-1}, currentHour{-1};

    std::mutex fileMutex; // held only by worker when rotating/writing

    // ctor/dtor private
    Logger()
    {
        // start worker thread
        worker = std::thread([this]()
                             { this->workerLoop(); });
    }

    ~Logger()
    {
        shutdown();
        // close file if open
        std::lock_guard<std::mutex> lk(fileMutex);
        if (outFile.is_open())
            outFile.close();
    }

    // Hot-path enqueue (very short lock)
    void log(Level lvl, const std::string &client, const std::string &msg, const std::string &tag = "")
    {
        // Fast path filter by level
        if (static_cast<int>(lvl) < level.load(std::memory_order_relaxed))
            return;

        // Create a small item
        Item it;
        it.tp = std::chrono::system_clock::now();
        it.level = lvl;
        it.client = client;
        it.tag = tag;
        it.msg = msg;

        std::unique_lock<std::mutex> lk(queueMutex);

        // If queue too large, drop oldest (avoids unbounded growth)
        if (queue.size() >= MAX_QUEUE)
        {
            queue.pop_front();
        }

        queue.emplace_back(std::move(it));

        // wake worker (use notify_one to minimize wakeups)
        lk.unlock();
        queueCond.notify_one();
    }

    // Worker thread: batch, format, rotate, write
    void workerLoop()
    {
        std::vector<Item> batch;
        batch.reserve(MAX_BATCH);

        while (!stopping.load(std::memory_order_relaxed))
        {
            // Wait for either items or timeout
            {
                std::unique_lock<std::mutex> lk(queueMutex);
                if (queue.empty())
                    queueCond.wait_for(lk, FLUSH_INTERVAL);
                // move up to MAX_BATCH items
                while (!queue.empty() && batch.size() < MAX_BATCH)
                {
                    batch.emplace_back(std::move(queue.front()));
                    queue.pop_front();
                }
            }

            if (!batch.empty())
            {
                // Format and write batch
                writeBatch(batch);
                batch.clear();
            }
        }

        // flush remaining items before exit
        {
            std::unique_lock<std::mutex> lk(queueMutex);
            while (!queue.empty())
            {
                batch.emplace_back(std::move(queue.front()));
                queue.pop_front();
                if (batch.size() >= MAX_BATCH)
                {
                    writeBatch(batch);
                    batch.clear();
                }
            }
        }

        if (!batch.empty())
            writeBatch(batch);
    }

    // Helper: format timestamp (thread-safe here), rotate file if required, and append lines
    void writeBatch(const std::vector<Item> &batch)
    {
        std::ostringstream oss;

        for (const auto &it : batch)
        {
            // format timestamp (YYYY-MM-DD HH:MM:SS.mmm)
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(it.tp.time_since_epoch()) % 1000;
            std::time_t t = std::chrono::system_clock::to_time_t(it.tp);
            std::tm local_tm{};
#ifdef _WIN32
            localtime_s(&local_tm, &t);
#else
            localtime_r(&t, &local_tm);
#endif
            oss << "[" << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S")
                << "." << std::setw(3) << std::setfill('0') << ms.count() << "]";

            // level string
            switch (it.level)
            {
            case Level::DEBUG:
                oss << "[DEBUG]";
                break;
            case Level::INFO:
                oss << "[INFO]";
                break;
            case Level::WARN:
                oss << "[WARN]";
                break;
            case Level::ERROR:
                oss << "[ERROR]";
                break;
            default:
                oss << "[UNKNOWN]";
                break;
            }

            if (!it.tag.empty())
                oss << "[" << it.tag << "]";

            if (!it.client.empty())
                oss << "[" << it.client << "]";

            oss << " " << it.msg << "\n";
        }

        // Lock file and rotate if needed, then write in one shot
        std::lock_guard<std::mutex> lk(fileMutex);
        rotateFileIfNeeded(batch.front().tp);
        if (outFile.is_open())
        {
            outFile << oss.str();
            outFile.flush();
        }
    }

    // Rotation using the first timestamp in batch (worker-only)
    void rotateFileIfNeeded(const std::chrono::system_clock::time_point &tp)
    {
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        std::tm local_tm{};
#ifdef _WIN32
        localtime_s(&local_tm, &t);
#else
        localtime_r(&t, &local_tm);
#endif

        int y = local_tm.tm_year;
        int mo = local_tm.tm_mon;
        int d = local_tm.tm_mday;
        int h = local_tm.tm_hour;

        if (y == currentYear && mo == currentMonth && d == currentDay && h == currentHour)
            return; // same hour

        // close old file (if any)
        if (outFile.is_open())
        {
            outFile.close();
        }

        currentYear = y;
        currentMonth = mo;
        currentDay = d;
        currentHour = h;

        std::ostringstream ss;
        ss << "redis-" << (local_tm.tm_year + 1900) << "-"
           << std::setw(2) << std::setfill('0') << (local_tm.tm_mon + 1) << "-"
           << std::setw(2) << std::setfill('0') << local_tm.tm_mday << "-"
           << std::setw(2) << std::setfill('0') << local_tm.tm_hour
           << ".log";

        outFile.open(ss.str(), std::ios::app);
        // if open fails, we silently drop logs (to avoid throwing in worker)
    }
};
