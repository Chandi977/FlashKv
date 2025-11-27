#ifndef REDIS_DATABASE_H
#define REDIS_DATABASE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <sys/types.h>

/*
  RedisDatabase — Phase-5 Optimized Header
  ----------------------------------------
  - String / List / Hash support
  - TTL Expiry
  - Single Global Mutex (Phase-1)
  - Lock Striping Infrastructure (Phase-5, used in Phase-6)
  - Zero-copy friendly, thread-safe design
*/

class RedisDatabase
{
public:
  // Singleton instance
  static RedisDatabase &getInstance();

  // -------------------------------------------------------------------------
  // Core Operations
  // -------------------------------------------------------------------------
  bool flushAll();

  // --- KeyValue ---
  void set(const std::string &key, const std::string &value);
  bool get(const std::string &key, std::string &value);
  std::vector<std::string> keys();
  std::string type(const std::string &key);
  bool del(const std::string &key);

  bool expire(const std::string &key, int seconds);
  void purgeExpired();
  bool rename(const std::string &oldKey, const std::string &newKey);

  // -------------------------------------------------------------------------
  // List Operations
  // -------------------------------------------------------------------------
  std::vector<std::string> lget(const std::string &key);
  ssize_t llen(const std::string &key);
  void lpush(const std::string &key, const std::string &value);
  void rpush(const std::string &key, const std::string &value);
  bool lpop(const std::string &key, std::string &value);
  bool rpop(const std::string &key, std::string &value);
  int lrem(const std::string &key, int count, const std::string &value);
  bool lindex(const std::string &key, int index, std::string &value);
  bool lset(const std::string &key, int index, const std::string &value);

  // -------------------------------------------------------------------------
  // Hash Operations
  // -------------------------------------------------------------------------
  bool hset(const std::string &key, const std::string &field, const std::string &value);
  bool hget(const std::string &key, const std::string &field, std::string &value);
  bool hexists(const std::string &key, const std::string &field);
  bool hdel(const std::string &key, const std::string &field);
  std::unordered_map<std::string, std::string> hgetall(const std::string &key);
  std::vector<std::string> hkeys(const std::string &key);
  std::vector<std::string> hvals(const std::string &key);
  ssize_t hlen(const std::string &key);
  bool hmset(const std::string &key, const std::vector<std::pair<std::string, std::string>> &fieldValues);

  // -------------------------------------------------------------------------
  // Persistence
  // -------------------------------------------------------------------------
  bool dump(const std::string &filename);
  bool load(const std::string &filename);

private:
  // Hidden constructor / destructor
  RedisDatabase() = default;
  ~RedisDatabase() = default;

  RedisDatabase(const RedisDatabase &) = delete;
  RedisDatabase &operator=(const RedisDatabase &) = delete;

  // -------------------------------------------------------------------------
  // GLOBAL LOCK (Phase-1)
  // -------------------------------------------------------------------------
  std::mutex db_mutex;

  // -------------------------------------------------------------------------
  // PHASE-5: Lock Striping Infrastructure (not yet activated)
  // -------------------------------------------------------------------------
  static constexpr int SHARD_COUNT = 32;
  std::mutex shard_mutexes[SHARD_COUNT];

  inline size_t shardFor(const std::string &key) const noexcept
  {
    return std::hash<std::string>{}(key) & (SHARD_COUNT - 1);
  }

  // -------------------------------------------------------------------------
  // Primary Data Stores
  // -------------------------------------------------------------------------
  std::unordered_map<std::string, std::string> kv_store;
  std::unordered_map<std::string, std::vector<std::string>> list_store;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> hash_store;

  // Expiry map
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> expiry_map;
};

#endif
