// src/RedisDatabase.cpp
#include "../include/RedisDatabase.h"
#include "../include/Logger.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

using sys_clock = std::chrono::system_clock;

/* -----------------------------------------------------------------------------
   Singleton
----------------------------------------------------------------------------- */
RedisDatabase &RedisDatabase::getInstance()
{
    static RedisDatabase inst;
    return inst;
}

/* -----------------------------------------------------------------------------
   Expiry sweep (safe & race-free)
----------------------------------------------------------------------------- */
static constexpr std::chrono::milliseconds SWEEP_INTERVAL{1000};
static sys_clock::time_point last_sweep = sys_clock::now() - SWEEP_INTERVAL;

namespace
{
    inline bool tp_expired(const sys_clock::time_point &tp)
    {
        return sys_clock::now() >= tp;
    }
}

/* -----------------------------------------------------------------------------
   Internal helper: fast delete of all data types for key
----------------------------------------------------------------------------- */
inline void fastErase(
    const std::string &key,
    std::unordered_map<std::string, std::string> &kv,
    std::unordered_map<std::string, std::deque<std::string>> &lists,
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> &hash)
{
    if (!kv.erase(key))
        if (!lists.erase(key))
            hash.erase(key);
}

/* -----------------------------------------------------------------------------
   Internal helper: check & purge single key expiration
----------------------------------------------------------------------------- */
bool RedisDatabase::checkExpired(const std::string &key)
{
    auto it = expiry_map.find(key);
    if (it != expiry_map.end() && tp_expired(it->second))
    {
        fastErase(key, kv_store, list_store, hash_store);
        expiry_map.erase(it);
        return true;
    }
    return false;
}

/* -----------------------------------------------------------------------------
   Full purge (rate-limited)
----------------------------------------------------------------------------- */
void RedisDatabase::maybeFullPurge()
{
    auto now = sys_clock::now();
    if (now - last_sweep < SWEEP_INTERVAL)
        return;

    last_sweep = now; // no mutex → no deadlock risk

    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired_locked();
}

void RedisDatabase::purgeExpired_locked()
{
    for (auto it = expiry_map.begin(); it != expiry_map.end();)
    {
        if (tp_expired(it->second))
        {
            fastErase(it->first, kv_store, list_store, hash_store);
            it = expiry_map.erase(it);
        }
        else
            ++it;
    }
}

bool RedisDatabase::purgeKeyIfExpired(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    return checkExpired(key);
}

sys_clock::time_point RedisDatabase::tp_from_ms_since_epoch(long long ms)
{
    return sys_clock::time_point(std::chrono::milliseconds(ms));
}

/* -----------------------------------------------------------------------------
   FLUSHALL
----------------------------------------------------------------------------- */
bool RedisDatabase::flushAll()
{
    std::lock_guard<std::mutex> lock(db_mutex);
    kv_store.clear();
    list_store.clear();
    hash_store.clear();
    expiry_map.clear();
    return true;
}

/* -----------------------------------------------------------------------------
   STRING OPERATIONS
----------------------------------------------------------------------------- */
void RedisDatabase::set(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    kv_store[key] = value;
}

bool RedisDatabase::get(const std::string &key, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return false;

    auto it = kv_store.find(key);
    if (it == kv_store.end())
        return false;

    value = it->second;
    return true;
}

bool RedisDatabase::del(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    bool removed = false;
    if (kv_store.erase(key) > 0)
        removed = true;
    if (list_store.erase(key) > 0)
        removed = true;
    if (hash_store.erase(key) > 0)
        removed = true;

    expiry_map.erase(key);
    return removed;
}

std::vector<std::string> RedisDatabase::keys()
{
    maybeFullPurge();

    std::lock_guard<std::mutex> lock(db_mutex);
    std::vector<std::string> out;
    out.reserve(kv_store.size() + list_store.size() + hash_store.size());

    for (auto &p : kv_store)
        out.push_back(p.first);
    for (auto &p : list_store)
        out.push_back(p.first);
    for (auto &p : hash_store)
        out.push_back(p.first);

    return out;
}

std::string RedisDatabase::type(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return "none";

    if (kv_store.count(key))
        return "string";
    if (list_store.count(key))
        return "list";
    if (hash_store.count(key))
        return "hash";

    return "none";
}

/* -----------------------------------------------------------------------------
   EXPIRATION
----------------------------------------------------------------------------- */
bool RedisDatabase::expire(const std::string &key, int seconds)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (!(kv_store.count(key) || list_store.count(key) || hash_store.count(key)))
        return false;

    expiry_map[key] = sys_clock::now() + std::chrono::seconds(seconds);
    return true;
}

int RedisDatabase::ttl(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    auto it = expiry_map.find(key);
    if (it == expiry_map.end())
    {
        if (kv_store.count(key) || list_store.count(key) || hash_store.count(key))
            return -1; // exists, no TTL
        return -2;     // does not exist
    }

    if (tp_expired(it->second))
    {
        fastErase(key, kv_store, list_store, hash_store);
        expiry_map.erase(it);
        return -2;
    }

    return (int)std::chrono::duration_cast<std::chrono::seconds>(it->second - sys_clock::now()).count();
}

/* -----------------------------------------------------------------------------
   RENAME
----------------------------------------------------------------------------- */
bool RedisDatabase::rename(const std::string &oldKey, const std::string &newKey)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    // Clear new key always
    fastErase(newKey, kv_store, list_store, hash_store);
    expiry_map.erase(newKey);

    bool found = false;

    if (auto it = kv_store.find(oldKey); it != kv_store.end())
    {
        kv_store[newKey] = std::move(it->second);
        kv_store.erase(it);
        found = true;
    }

    if (auto it = list_store.find(oldKey); it != list_store.end())
    {
        list_store[newKey] = std::move(it->second);
        list_store.erase(it);
        found = true;
    }

    if (auto it = hash_store.find(oldKey); it != hash_store.end())
    {
        hash_store[newKey] = std::move(it->second);
        hash_store.erase(it);
        found = true;
    }

    if (auto it = expiry_map.find(oldKey); it != expiry_map.end())
    {
        expiry_map[newKey] = it->second;
        expiry_map.erase(it);
    }

    return found;
}

/* -----------------------------------------------------------------------------
   LIST OPERATIONS  (all TTL safe)
----------------------------------------------------------------------------- */
std::vector<std::string> RedisDatabase::lget(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return {};

    auto it = list_store.find(key);
    if (it == list_store.end())
        return {};

    return std::vector<std::string>(it->second.begin(), it->second.end());
}

ssize_t RedisDatabase::llen(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return 0;

    auto it = list_store.find(key);
    return (it == list_store.end()) ? 0 : it->second.size();
}

void RedisDatabase::lpush(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
    {
        // create new list
        list_store[key].push_front(value);
        return;
    }

    list_store[key].push_front(value);
}

void RedisDatabase::rpush(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
    {
        list_store[key].push_back(value);
        return;
    }

    list_store[key].push_back(value);
}

int RedisDatabase::lrem(const std::string &key, int count, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return 0;

    auto it = list_store.find(key);
    if (it == list_store.end())
        return 0;

    auto &lst = it->second;
    int removed = 0;

    if (count == 0)
    {
        std::deque<std::string> newList;
        for (auto &v : lst)
        {
            if (v == value)
                removed++;
            else
                newList.push_back(v);
        }
        lst.swap(newList);
        return removed;
    }

    if (count > 0)
    {
        std::deque<std::string> newList;
        for (auto &v : lst)
        {
            if (v == value && removed < count)
                removed++;
            else
                newList.push_back(v);
        }
        lst.swap(newList);
        return removed;
    }

    int target = -count;
    std::deque<std::string> newList;
    for (auto it2 = lst.rbegin(); it2 != lst.rend(); ++it2)
    {
        if (*it2 == value && removed < target)
        {
            removed++;
            continue;
        }
        newList.push_front(*it2);
    }
    lst.swap(newList);
    return removed;
}

bool RedisDatabase::lpop(const std::string &key, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return false;

    auto it = list_store.find(key);
    if (it == list_store.end() || it->second.empty())
        return false;

    value = std::move(it->second.front());
    it->second.pop_front();
    return true;
}

bool RedisDatabase::rpop(const std::string &key, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return false;

    auto it = list_store.find(key);
    if (it == list_store.end() || it->second.empty())
        return false;

    value = std::move(it->second.back());
    it->second.pop_back();
    return true;
}

bool RedisDatabase::lindex(const std::string &key, int index, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return false;

    auto it = list_store.find(key);
    if (it == list_store.end())
        return false;

    auto &lst = it->second;
    int sz = lst.size();

    if (index < 0)
        index += sz;
    if (index < 0 || index >= sz)
        return false;

    value = lst[index];
    return true;
}

bool RedisDatabase::lset(const std::string &key, int index, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return false;

    auto it = list_store.find(key);
    if (it == list_store.end())
        return false;

    auto &lst = it->second;
    int sz = lst.size();

    if (index < 0)
        index += sz;
    if (index < 0 || index >= sz)
        return false;

    lst[index] = value;
    return true;
}

/* -----------------------------------------------------------------------------
   HASH OPERATIONS (all TTL safe)
----------------------------------------------------------------------------- */
bool RedisDatabase::hset(const std::string &key, const std::string &field, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
    {
        hash_store[key][field] = value;
        return true;
    }

    hash_store[key][field] = value;
    return true;
}

bool RedisDatabase::hget(const std::string &key, const std::string &field, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return false;

    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return false;

    auto f = it->second.find(field);
    if (f == it->second.end())
        return false;

    value = f->second;
    return true;
}

bool RedisDatabase::hexists(const std::string &key, const std::string &field)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return false;

    auto it = hash_store.find(key);
    return (it != hash_store.end() && it->second.count(field) > 0);
}

bool RedisDatabase::hdel(const std::string &key, const std::string &field)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return false;

    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return false;

    return (it->second.erase(field) > 0);
}

std::unordered_map<std::string, std::string> RedisDatabase::hgetall(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return {};

    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return {};

    return it->second;
}

std::vector<std::string> RedisDatabase::hkeys(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return {};

    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return {};

    std::vector<std::string> out;
    out.reserve(it->second.size());
    for (auto &p : it->second)
        out.push_back(p.first);

    return out;
}

std::vector<std::string> RedisDatabase::hvals(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return {};

    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return {};

    std::vector<std::string> out;
    out.reserve(it->second.size());
    for (auto &p : it->second)
        out.push_back(p.second);

    return out;
}

ssize_t RedisDatabase::hlen(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
        return 0;

    auto it = hash_store.find(key);
    return (it == hash_store.end()) ? 0 : it->second.size();
}

bool RedisDatabase::hmset(
    const std::string &key,
    const std::vector<std::pair<std::string, std::string>> &fv)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    if (checkExpired(key))
    {
        for (auto &p : fv)
            hash_store[key][p.first] = p.second;
        return true;
    }

    auto &mp = hash_store[key];
    for (auto &p : fv)
        mp[p.first] = p.second;

    return true;
}

/* -----------------------------------------------------------------------------
   INCR (improved)
----------------------------------------------------------------------------- */
bool RedisDatabase::incr(const std::string &key, long long &out)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    checkExpired(key);

    auto it = kv_store.find(key);
    if (it == kv_store.end())
    {
        kv_store[key] = "1";
        out = 1;
        return true;
    }

    std::string s = it->second;

    // trim whitespace (Redis-compatible)
    auto trim = [](std::string &str)
    {
        str.erase(0, str.find_first_not_of(" \t\r\n"));
        str.erase(str.find_last_not_of(" \t\r\n") + 1);
    };
    trim(s);

    try
    {
        long long v = std::stoll(s);
        v += 1;
        it->second = std::to_string(v);
        out = v;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

long long RedisDatabase::incr(const std::string &key)
{
    long long out = 0;
    if (!incr(key, out))
        throw std::runtime_error("value is not an integer");
    return out;
}

/* -----------------------------------------------------------------------------
   Persistence (unchanged format)
----------------------------------------------------------------------------- */
bool RedisDatabase::dump(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    std::ofstream ofs(filename, std::ios::binary | std::ios::trunc);
    if (!ofs)
        return false;

    // Strings
    for (auto &kv : kv_store)
    {
        ofs.put('K');
        ofs << ' ' << kv.first.size() << ' ' << kv.second.size() << '\n';
        ofs.write(kv.first.data(), kv.first.size());
        ofs.write(kv.second.data(), kv.second.size());
        ofs.put('\n');
    }

    // Lists
    for (auto &kv : list_store)
    {
        ofs.put('L');
        ofs << ' ' << kv.first.size() << ' ' << kv.second.size() << '\n';
        ofs.write(kv.first.data(), kv.first.size());

        for (auto &item : kv.second)
        {
            ofs << ' ' << item.size() << '\n';
            ofs.write(item.data(), item.size());
        }
        ofs.put('\n');
    }

    // Hashes
    for (auto &kv : hash_store)
    {
        ofs.put('H');
        ofs << ' ' << kv.first.size() << ' ' << kv.second.size() << '\n';
        ofs.write(kv.first.data(), kv.first.size());

        for (auto &p : kv.second)
        {
            ofs << ' ' << p.first.size() << ' ' << p.second.size() << '\n';
            ofs.write(p.first.data(), p.first.size());
            ofs.write(p.second.data(), p.second.size());
        }
        ofs.put('\n');
    }

    // Expiries
    for (auto &e : expiry_map)
    {
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           e.second.time_since_epoch())
                           .count();

        ofs.put('E');
        ofs << ' ' << e.first.size() << ' ' << ms << '\n';
        ofs.write(e.first.data(), e.first.size());
        ofs.put('\n');
    }

    ofs.flush();
    return true;
}

bool RedisDatabase::load(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs)
        return false;

    kv_store.clear();
    list_store.clear();
    hash_store.clear();
    expiry_map.clear();

    while (ifs.peek() != EOF)
    {
        char type;
        ifs.get(type);
        if (ifs.eof())
            break;

        if (type == 'K')
        {
            size_t key_len = 0, val_len = 0;
            ifs >> key_len >> val_len;
            ifs.get();

            std::string key(key_len, '\0');
            std::string val(val_len, '\0');
            ifs.read(&key[0], key_len);
            ifs.read(&val[0], val_len);
            if (ifs.peek() == '\n')
                ifs.get();

            kv_store[std::move(key)] = std::move(val);
        }
        else if (type == 'L')
        {
            size_t key_len = 0, count = 0;
            ifs >> key_len >> count;
            ifs.get();

            std::string key(key_len, '\0');
            ifs.read(&key[0], key_len);

            std::deque<std::string> dq;

            for (size_t i = 0; i < count; i++)
            {
                size_t len = 0;
                ifs >> len;
                ifs.get();

                std::string item(len, '\0');
                ifs.read(&item[0], len);
                dq.push_back(std::move(item));
            }

            if (ifs.peek() == '\n')
                ifs.get();
            list_store[std::move(key)] = std::move(dq);
        }
        else if (type == 'H')
        {
            size_t key_len = 0, pairs = 0;
            ifs >> key_len >> pairs;
            ifs.get();

            std::string key(key_len, '\0');
            ifs.read(&key[0], key_len);

            std::unordered_map<std::string, std::string> mp;

            for (size_t i = 0; i < pairs; i++)
            {
                size_t fl, vl;
                ifs >> fl >> vl;
                ifs.get();

                std::string field(fl, '\0');
                std::string val(vl, '\0');
                ifs.read(&field[0], fl);
                ifs.read(&val[0], vl);

                mp.emplace(std::move(field), std::move(val));
            }

            if (ifs.peek() == '\n')
                ifs.get();
            hash_store[std::move(key)] = std::move(mp);
        }
        else if (type == 'E')
        {
            size_t key_len = 0;
            long long ms = 0;
            ifs >> key_len >> ms;
            ifs.get();

            std::string key(key_len, '\0');
            ifs.read(&key[0], key_len);

            if (ifs.peek() == '\n')
                ifs.get();
            expiry_map[std::move(key)] = tp_from_ms_since_epoch(ms);
        }
        else
        {
            std::string skip;
            std::getline(ifs, skip);
        }
    }

    purgeExpired_locked();
    return true;
}

void RedisDatabase::purgeExpired()
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired_locked();
}
