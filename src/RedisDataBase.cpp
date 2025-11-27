#include "../include/RedisDatabase.h"
#include "../include/Logger.h"

#include <mutex>
#include <fstream>
#include <sstream>
#include <algorithm>

/* ============================================================================
   RedisDatabase — PHASE-5 Optimized Implementation
   - Minimal allocations
   - Avoid repeated lookups
   - Thread safe
   - Expiry handled in correct hot-paths
============================================================================ */

RedisDatabase &RedisDatabase::getInstance()
{
    static RedisDatabase instance;
    return instance;
}

/* ============================================================================
   BASIC COMMANDS
============================================================================ */

bool RedisDatabase::flushAll()
{
    std::lock_guard<std::mutex> lock(db_mutex);
    kv_store.clear();
    list_store.clear();
    hash_store.clear();
    expiry_map.clear();
    return true;
}

/* ============================================================================
   STRING OPERATIONS
============================================================================ */

void RedisDatabase::set(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    kv_store[key] = value; // direct write, O(1)
}

bool RedisDatabase::get(const std::string &key, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();

    auto it = kv_store.find(key);
    if (it == kv_store.end())
        return false;

    value = it->second; // copy actual stored string
    return true;
}

std::vector<std::string> RedisDatabase::keys()
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();

    size_t total = kv_store.size() + list_store.size() + hash_store.size();
    std::vector<std::string> result;
    result.reserve(total);

    for (auto &p : kv_store)
        result.emplace_back(p.first);
    for (auto &p : list_store)
        result.emplace_back(p.first);
    for (auto &p : hash_store)
        result.emplace_back(p.first);

    return result;
}

std::string RedisDatabase::type(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();

    if (kv_store.find(key) != kv_store.end())
        return "string";
    if (list_store.find(key) != list_store.end())
        return "list";
    if (hash_store.find(key) != hash_store.end())
        return "hash";

    return "none";
}

bool RedisDatabase::del(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();

    bool removed = false;

    removed |= kv_store.erase(key) > 0;
    removed |= list_store.erase(key) > 0;
    removed |= hash_store.erase(key) > 0;

    expiry_map.erase(key);
    return removed;
}

/* ============================================================================
   EXPIRY SYSTEM
============================================================================ */

bool RedisDatabase::expire(const std::string &key, int seconds)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();

    if (!(
            kv_store.count(key) ||
            list_store.count(key) ||
            hash_store.count(key)))
        return false;

    expiry_map[key] = std::chrono::steady_clock::now() +
                      std::chrono::seconds(seconds);

    return true;
}

void RedisDatabase::purgeExpired()
{
    auto now = std::chrono::steady_clock::now();

    for (auto it = expiry_map.begin(); it != expiry_map.end();)
    {
        if (now >= it->second)
        {
            kv_store.erase(it->first);
            list_store.erase(it->first);
            hash_store.erase(it->first);
            it = expiry_map.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool RedisDatabase::rename(const std::string &oldKey, const std::string &newKey)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();

    bool found = false;

    auto kv_it = kv_store.find(oldKey);
    if (kv_it != kv_store.end())
    {
        kv_store[newKey] = std::move(kv_it->second);
        kv_store.erase(kv_it);
        found = true;
    }

    auto li_it = list_store.find(oldKey);
    if (li_it != list_store.end())
    {
        list_store[newKey] = std::move(li_it->second);
        list_store.erase(li_it);
        found = true;
    }

    auto h_it = hash_store.find(oldKey);
    if (h_it != hash_store.end())
    {
        hash_store[newKey] = std::move(h_it->second);
        hash_store.erase(h_it);
        found = true;
    }

    auto exp_it = expiry_map.find(oldKey);
    if (exp_it != expiry_map.end())
    {
        expiry_map[newKey] = exp_it->second;
        expiry_map.erase(exp_it);
    }

    return found;
}

/* ============================================================================
   LIST OPERATIONS (Optimized for minimal lookups)
============================================================================ */

std::vector<std::string> RedisDatabase::lget(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = list_store.find(key);
    if (it == list_store.end())
        return {};
    return it->second;
}

ssize_t RedisDatabase::llen(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = list_store.find(key);
    return (it != list_store.end()) ? it->second.size() : 0;
}

void RedisDatabase::lpush(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto &lst = list_store[key];
    lst.insert(lst.begin(), value);
}

void RedisDatabase::rpush(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    list_store[key].push_back(value);
}

bool RedisDatabase::lpop(const std::string &key, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = list_store.find(key);
    if (it == list_store.end() || it->second.empty())
        return false;

    auto &lst = it->second;
    value = std::move(lst.front());
    lst.erase(lst.begin());
    return true;
}

bool RedisDatabase::rpop(const std::string &key, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = list_store.find(key);
    if (it == list_store.end() || it->second.empty())
        return false;

    auto &lst = it->second;
    value = std::move(lst.back());
    lst.pop_back();
    return true;
}

int RedisDatabase::lrem(const std::string &key, int count, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = list_store.find(key);
    if (it == list_store.end())
        return 0;

    auto &lst = it->second;
    int removed = 0;

    if (count == 0)
    {
        auto new_end = std::remove(lst.begin(), lst.end(), value);
        removed = std::distance(new_end, lst.end());
        lst.erase(new_end, lst.end());
    }
    else if (count > 0)
    {
        for (auto iter = lst.begin(); iter != lst.end() && removed < count;)
        {
            if (*iter == value)
            {
                iter = lst.erase(iter);
                removed++;
            }
            else
                ++iter;
        }
    }
    else
    {
        for (auto iter = lst.rbegin(); iter != lst.rend() && removed < -count;)
        {
            if (*iter == value)
            {
                auto base = iter.base();
                --base;
                base = lst.erase(base);
                removed++;
                iter = std::reverse_iterator(base);
            }
            else
                ++iter;
        }
    }

    return removed;
}

bool RedisDatabase::lindex(const std::string &key, int index, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = list_store.find(key);
    if (it == list_store.end())
        return false;

    auto &lst = it->second;

    if (index < 0)
        index += lst.size();
    if (index < 0 || index >= (int)lst.size())
        return false;

    value = lst[index];
    return true;
}

bool RedisDatabase::lset(const std::string &key, int index, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = list_store.find(key);
    if (it == list_store.end())
        return false;

    auto &lst = it->second;
    if (index < 0)
        index += lst.size();
    if (index < 0 || index >= (int)lst.size())
        return false;

    lst[index] = value;
    return true;
}

/* ============================================================================
   HASH OPERATIONS
============================================================================ */

bool RedisDatabase::hset(const std::string &key, const std::string &field, const std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    hash_store[key][field] = value;
    return true;
}

bool RedisDatabase::hget(const std::string &key, const std::string &field, std::string &value)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return false;

    auto &h = it->second;
    auto f = h.find(field);
    if (f == h.end())
        return false;

    value = f->second;
    return true;
}

bool RedisDatabase::hexists(const std::string &key, const std::string &field)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = hash_store.find(key);
    return it != hash_store.end() && it->second.count(field);
}

bool RedisDatabase::hdel(const std::string &key, const std::string &field)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return false;
    return it->second.erase(field) > 0;
}

std::unordered_map<std::string, std::string> RedisDatabase::hgetall(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return {};
    return it->second;
}

std::vector<std::string> RedisDatabase::hkeys(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return {};

    std::vector<std::string> out;
    out.reserve(it->second.size());
    for (auto &p : it->second)
        out.emplace_back(p.first);
    return out;
}

std::vector<std::string> RedisDatabase::hvals(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = hash_store.find(key);
    if (it == hash_store.end())
        return {};

    std::vector<std::string> out;
    out.reserve(it->second.size());
    for (auto &p : it->second)
        out.emplace_back(p.second);
    return out;
}

ssize_t RedisDatabase::hlen(const std::string &key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = hash_store.find(key);
    return (it != hash_store.end()) ? it->second.size() : 0;
}

bool RedisDatabase::hmset(const std::string &key,
                          const std::vector<std::pair<std::string, std::string>> &fv)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto &h = hash_store[key];
    for (auto &p : fv)
        h[p.first] = p.second;
    return true;
}

/* ============================================================================
   DUMP / LOAD (same logic, clean structure)
============================================================================ */

bool RedisDatabase::dump(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    std::ofstream ofs(filename);
    if (!ofs)
        return false;

    for (auto &kv : kv_store)
        ofs << "K " << kv.first << " " << kv.second << "\n";

    for (auto &kv : list_store)
    {
        ofs << "L " << kv.first;
        for (auto &item : kv.second)
            ofs << " " << item;
        ofs << "\n";
    }

    for (auto &kv : hash_store)
    {
        ofs << "H " << kv.first;
        for (auto &item : kv.second)
            ofs << " " << item.first << ":" << item.second;
        ofs << "\n";
    }

    return true;
}

bool RedisDatabase::load(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    std::ifstream ifs(filename);
    if (!ifs)
        return false;

    kv_store.clear();
    list_store.clear();
    hash_store.clear();
    expiry_map.clear();

    std::string line;

    while (std::getline(ifs, line))
    {
        std::istringstream iss(line);
        char type;
        iss >> type;

        if (type == 'K')
        {
            std::string key, val;
            iss >> key >> val;
            kv_store[key] = val;
        }
        else if (type == 'L')
        {
            std::string key, item;
            iss >> key;
            std::vector<std::string> lst;
            while (iss >> item)
                lst.push_back(item);
            list_store[key] = std::move(lst);
        }
        else if (type == 'H')
        {
            std::string key, pair;
            iss >> key;
            while (iss >> pair)
            {
                auto pos = pair.find(':');
                if (pos != std::string::npos)
                    hash_store[key][pair.substr(0, pos)] =
                        pair.substr(pos + 1);
            }
        }
    }

    return true;
}
