#ifndef REDIS_DATABASE_H
#define REDIS_DATABASE_H

#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <sys/types.h> // ssize_t on POSIX; on Windows you may need to typedef ssize_t

// Simple, thread-safe Redis-like in-memory DB. Single-shard for clarity.
class RedisDatabase
{
public:
   // Singleton accessor
   static RedisDatabase &getInstance();

   /* ============================================================
      CORE OPERATIONS
      ============================================================ */
   bool flushAll();

   // Key-Value
   void set(const std::string &key, const std::string &value);
   bool get(const std::string &key, std::string &value);
   std::vector<std::string> keys();
   std::string type(const std::string &key);
   bool del(const std::string &key);

   bool expire(const std::string &key, int seconds);
   int ttl(const std::string &key); // (-1 no TTL, -2 missing)

   void purgeExpired();
   bool rename(const std::string &oldKey, const std::string &newKey);

   /* ============================================================
      NUMERIC OPERATIONS
      ============================================================ */
   // Strict increment - throws on error (convenience)
   long long incr(const std::string &key);

   // Safe INCR: returns false if value is non-integer; out contains new value when true
   bool incr(const std::string &key, long long &out);

   /* ============================================================
      LIST OPERATIONS
      ============================================================ */
   std::vector<std::string> lget(const std::string &key);
   ssize_t llen(const std::string &key);
   void lpush(const std::string &key, const std::string &value);
   void rpush(const std::string &key, const std::string &value);
   bool lpop(const std::string &key, std::string &value);
   bool rpop(const std::string &key, std::string &value);
   int lrem(const std::string &key, int count, const std::string &value);
   bool lindex(const std::string &key, int index, std::string &value);
   bool lset(const std::string &key, int index, const std::string &value);

   /* ============================================================
      HASH OPERATIONS
      ============================================================ */
   bool hset(const std::string &key, const std::string &field, const std::string &value);
   bool hget(const std::string &key, const std::string &field, std::string &value);
   bool hexists(const std::string &key, const std::string &field);
   bool hdel(const std::string &key, const std::string &field);

   std::unordered_map<std::string, std::string> hgetall(const std::string &key);
   std::vector<std::string> hkeys(const std::string &key);
   std::vector<std::string> hvals(const std::string &key);
   ssize_t hlen(const std::string &key);

   bool hmset(const std::string &key,
              const std::vector<std::pair<std::string, std::string>> &fieldValues);

   /* ============================================================
      PERSISTENCE
      ============================================================ */
   bool dump(const std::string &filename);
   bool load(const std::string &filename);

private:
   RedisDatabase() = default;
   ~RedisDatabase() = default;

   RedisDatabase(const RedisDatabase &) = delete;
   RedisDatabase &operator=(const RedisDatabase &) = delete;

   /* ============================================================
      INTERNAL STATE & HELPERS
      ============================================================ */
   std::mutex db_mutex; // global lock (simple and safe)

   // core stores
   std::unordered_map<std::string, std::string> kv_store;
   std::unordered_map<std::string, std::deque<std::string>> list_store;
   std::unordered_map<std::string, std::unordered_map<std::string, std::string>> hash_store;

   // expiry: key -> wall-clock deadline (system_clock)
   std::unordered_map<std::string, std::chrono::system_clock::time_point> expiry_map;

   // Expiry helpers
   void maybeFullPurge();                          // rate-limited full sweep
   void purgeExpired_locked();                     // purge (assumes db_mutex held)
   bool purgeKeyIfExpired(const std::string &key); // per-key purge (caller acquires mutex)
   bool checkExpired(const std::string &key);

   // Small helper to convert ms timestamp -> time_point when loading
   static std::chrono::system_clock::time_point tp_from_ms_since_epoch(long long ms);
};

#endif // REDIS_DATABASE_H
