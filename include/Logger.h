#pragma once

// --- FIX WINDOWS MACRO COLLISION -----------------------------------
#ifdef _WIN32
#ifdef ERROR
#undef ERROR
#endif
#endif
// -------------------------------------------------------------------

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
#include <filesystem>

class Logger
{
public:
    enum class Level : int
    {
        DEBUG = 0,
        INFO = 1,
        WARN = 2,
        ERROR = 3, // SAFE NOW (macro undef done above)
        OFF = 4
    };

    static Logger &getInstance()
    {
        static Logger instance;
        return instance;
    }

    void setLevel(Level lvl)
    {
        level.store(static_cast<int>(lvl), std::memory_order_relaxed);
    }

    Level getLevel() const
    {
        return static_cast<Level>(level.load(std::memory_order_relaxed));
    }

    void debug(const std::string &msg) { log(Level::DEBUG, "", msg); }
    void info(const std::string &msg) { log(Level::INFO, "", msg); }
    void warn(const std::string &msg) { log(Level::WARN, "", msg); }
    void error(const std::string &msg) { log(Level::ERROR, "", msg); }

    void request(const std::string &client, const std::string &msg) { log(Level::DEBUG, client, msg, "REQUEST"); }
    void response(const std::string &client, const std::string &msg) { log(Level::DEBUG, client, msg, "RESPONSE"); }

    void shutdown()
    {
        bool expected = false;
        if (!stopping.compare_exchange_strong(expected, true))
            return;

        {
            std::lock_guard<std::mutex> lk(queueMutex);
            queueCond.notify_one();
        }

        if (worker.joinable())
            worker.join();
    }

private:
    struct Item
    {
        std::chrono::system_clock::time_point tp;
        Level level;
        std::string client;
        std::string tag;
        std::string msg;
    };

    static constexpr size_t MAX_BATCH = 256;
    static constexpr std::chrono::milliseconds FLUSH_INTERVAL{200};
    static constexpr size_t MAX_QUEUE = 64 * 1024;

    std::atomic<int> level{static_cast<int>(Level::INFO)};
    std::mutex queueMutex;
    std::condition_variable queueCond;
    std::deque<Item> queue;

    std::thread worker;
    std::atomic<bool> stopping{false};

    std::ofstream outFile;
    int currentYear{-1}, currentMonth{-1}, currentDay{-1}, currentHour{-1};
    std::mutex fileMutex;

    Logger()
    {
        std::filesystem::create_directory("logs");
        worker = std::thread([this]()
                             { workerLoop(); });
    }

    ~Logger()
    {
        shutdown();
        std::lock_guard<std::mutex> lk(fileMutex);
        if (outFile.is_open())
            outFile.close();
    }

    void log(Level lvl, const std::string &client, const std::string &msg, const std::string &tag = "")
    {
        if (static_cast<int>(lvl) < level.load(std::memory_order_relaxed))
            return;

        Item it;
        it.tp = std::chrono::system_clock::now();
        it.level = lvl;
        it.client = client;
        it.tag = tag;
        it.msg = msg;

        std::unique_lock<std::mutex> lk(queueMutex);
        if (queue.size() >= MAX_QUEUE)
            queue.pop_front();

        queue.emplace_back(std::move(it));
        lk.unlock();
        queueCond.notify_one();
    }

    void workerLoop()
    {
        std::vector<Item> batch;
        batch.reserve(MAX_BATCH);

        while (!stopping.load())
        {
            {
                std::unique_lock<std::mutex> lk(queueMutex);
                if (queue.empty())
                    queueCond.wait_for(lk, FLUSH_INTERVAL);

                while (!queue.empty() && batch.size() < MAX_BATCH)
                {
                    batch.emplace_back(std::move(queue.front()));
                    queue.pop_front();
                }
            }

            if (!batch.empty())
            {
                writeBatch(batch);
                batch.clear();
            }
        }

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

    void writeBatch(const std::vector<Item> &batch)
    {
        std::ostringstream oss;

        for (const auto &it : batch)
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(it.tp.time_since_epoch()) % 1000;
            std::time_t t = std::chrono::system_clock::to_time_t(it.tp);
            std::tm tm{};

#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif

            oss << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
                << "." << std::setw(3) << std::setfill('0') << ms.count() << "]";

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

        std::lock_guard<std::mutex> lk(fileMutex);
        rotateFile(batch.front().tp);

        if (outFile.is_open())
        {
            outFile << oss.str();
            outFile.flush();
        }
    }

    void rotateFile(const std::chrono::system_clock::time_point &tp)
    {
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};

#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif

        if (tm.tm_year == currentYear &&
            tm.tm_mon == currentMonth &&
            tm.tm_mday == currentDay &&
            tm.tm_hour == currentHour)
            return;

        if (outFile.is_open())
            outFile.close();

        currentYear = tm.tm_year;
        currentMonth = tm.tm_mon;
        currentDay = tm.tm_mday;
        currentHour = tm.tm_hour;

        std::ostringstream ss;
        ss << "logs/redis-"
           << (tm.tm_year + 1900) << "-"
           << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1) << "-"
           << std::setw(2) << std::setfill('0') << tm.tm_mday << "-"
           << std::setw(2) << std::setfill('0') << tm.tm_hour
           << ".log";

        outFile.open(ss.str(), std::ios::app);
    }
};
