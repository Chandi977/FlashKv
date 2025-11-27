#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"
#include "../include/Logger.h"

#include <vector>
#include <algorithm>
#include <string_view>
#include <chrono>
#include <string>
#include <cctype>

// -----------------------------------------------------------------------------
// Zero-copy RESP parser that returns string_views into the original buffer.
// The caller must ensure the buffer/string outlives the views.
// -----------------------------------------------------------------------------
std::vector<std::string_view> RedisCommandHandler::parseRespViews(std::string_view input)
{
    std::vector<std::string_view> out;
    if (input.empty())
        return out;

    // Non-RESP fallback: whitespace-split
    if (input[0] != '*')
    {
        size_t i = 0;
        while (i < input.size())
        {
            while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i])))
                ++i;
            if (i >= input.size()) break;
            size_t j = i;
            while (j < input.size() && !std::isspace(static_cast<unsigned char>(input[j])))
                ++j;
            out.emplace_back(input.data() + i, j - i);
            i = j;
        }
        return out;
    }

    size_t pos = 1;
    size_t crlf = input.find("\r\n", pos);
    if (crlf == std::string_view::npos)
        return out;

    int elements = 0;
    try
    {
        // make a temporary string for stoi because stoi needs a null-terminated string
        elements = std::stoi(std::string(input.data() + pos, crlf - pos));
    }
    catch (...)
    {
        return out;
    }

    pos = crlf + 2;
    if (elements <= 0) return out;
    out.reserve(static_cast<size_t>(elements));

    for (int i = 0; i < elements; ++i)
    {
        if (pos >= input.size() || input[pos] != '$')
            break;
        ++pos;

        crlf = input.find("\r\n", pos);
        if (crlf == std::string_view::npos)
            break;

        int len = 0;
        try
        {
            len = std::stoi(std::string(input.data() + pos, crlf - pos));
        }
        catch (...)
        {
            break;
        }

        pos = crlf + 2;
        if (pos + static_cast<size_t>(len) > input.size())
            break;

        out.emplace_back(input.data() + pos, static_cast<size_t>(len));
        pos += static_cast<size_t>(len) + 2; // skip data + CRLF
    }

    return out;
}

// Convert views -> owning strings (only when needed by DB API)
std::vector<std::string> RedisCommandHandler::materialize(const std::vector<std::string_view> &views)
{
    std::vector<std::string> res;
    res.reserve(views.size());
    for (auto &v : views)
        res.emplace_back(v.data(), v.size());
    return res;
}

// -----------------------------------------------------------------------------
// Helper RESP builders (small utilities used by handlers)
// -----------------------------------------------------------------------------
static inline std::string simpleString(const char *s)
{
    std::string out;
    out.reserve(std::char_traits<char>::length(s) + 3);
    out.push_back('+');
    out.append(s);
    out.append("\r\n");
    return out;
}

static inline std::string simpleString(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 3);
    out.push_back('+');
    out.append(s);
    out.append("\r\n");
    return out;
}

static inline std::string bulkStringFromStd(const std::string &s)
{
    if (s.empty())
        return "$0\r\n\r\n";
    std::string out;
    out.reserve(s.size() + 32);
    out.push_back('$');
    out.append(std::to_string(s.size()));
    out.append("\r\n");
    out.append(s);
    out.append("\r\n");
    return out;
}

static inline std::string bulkStringFromView(std::string_view v)
{
    if (v.empty())
        return "$0\r\n\r\n";
    std::string out;
    out.reserve(v.size() + 32);
    out.push_back('$');
    out.append(std::to_string(v.size()));
    out.append("\r\n");
    out.append(v.data(), v.size());
    out.append("\r\n");
    return out;
}

static inline std::string nilBulk() { return "$-1\r\n"; }
static inline std::string integerReply(long long v) { return ":" + std::to_string(v) + "\r\n"; }
static inline std::string arrayHeader(size_t n) { return "*" + std::to_string(n) + "\r\n"; }

// -----------------------------------------------------------------------------
// Main dispatch: handlers use string_view tokens where possible
// -----------------------------------------------------------------------------

// Note: We only log on unknown commands or errors to avoid hot-path logging.

static std::string handlePing() { return simpleString("PONG"); }

static std::string handleEcho(const std::vector<std::string_view> &t)
{
    if (t.size() < 2) return "-Error: ECHO requires a message\r\n";
    return bulkStringFromView(t[1]);
}

static std::string handleSet(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 3) return "-Error: SET requires key and value\r\n";
    // DB API expects std::string - create owning strings only here
    db.set(std::string(t[1].data(), t[1].size()), std::string(t[2].data(), t[2].size()));
    return simpleString("OK");
}

static std::string handleGet(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: GET requires key\r\n";
    std::string value;
    if (db.get(std::string(t[1].data(), t[1].size()), value))
        return bulkStringFromStd(value);
    return nilBulk();
}

static std::string handleDel(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: DEL requires key\r\n";
    bool removed = db.del(std::string(t[1].data(), t[1].size()));
    return integerReply(removed ? 1 : 0);
}

static std::string handleFlushAll(RedisDatabase &db)
{
    db.flushAll();
    return simpleString("OK");
}

static std::string handleKeys(const std::vector<std::string_view> &, RedisDatabase &db)
{
    auto keys = db.keys();
    std::string out = arrayHeader(keys.size());
    for (const auto &k : keys) out += bulkStringFromStd(k);
    return out;
}

static std::string handleType(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: TYPE requires key\r\n";
    return simpleString(db.type(std::string(t[1].data(), t[1].size())));
}

static std::string handleExpire(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 3) return "-Error: EXPIRE requires key and time\r\n";
    try
    {
        int seconds = std::stoi(std::string(t[2].data(), t[2].size()));
        return db.expire(std::string(t[1].data(), t[1].size()), seconds) ? simpleString("OK") : "-Error: Key not found\r\n";
    }
    catch (...) { return "-Error: Invalid expiration time\r\n"; }
}

static std::string handleRename(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 3) return "-Error: RENAME requires old and new key\r\n";
    return db.rename(std::string(t[1].data(), t[1].size()), std::string(t[2].data(), t[2].size())) ? simpleString("OK") : "-Error: Rename failed\r\n";
}

// ---- List handlers ----
static std::string handleLpush(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 3) return "-Error: LPUSH requires key and values\r\n";
    std::string key(t[1].data(), t[1].size());
    for (size_t i = 2; i < t.size(); ++i) db.lpush(key, std::string(t[i].data(), t[i].size()));
    return integerReply(db.llen(key));
}

static std::string handleRpush(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 3) return "-Error: RPUSH requires key and values\r\n";
    std::string key(t[1].data(), t[1].size());
    for (size_t i = 2; i < t.size(); ++i) db.rpush(key, std::string(t[i].data(), t[i].size()));
    return integerReply(db.llen(key));
}

static std::string handleLpop(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: LPOP requires key\r\n";
    std::string key(t[1].data(), t[1].size()), v;
    if (db.lpop(key, v)) return bulkStringFromStd(v);
    return nilBulk();
}

static std::string handleRpop(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: RPOP requires key\r\n";
    std::string key(t[1].data(), t[1].size()), v;
    if (db.rpop(key, v)) return bulkStringFromStd(v);
    return nilBulk();
}

static std::string handleLlen(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: LLEN requires key\r\n";
    return integerReply(db.llen(std::string(t[1].data(), t[1].size())));
}

static std::string handleLget(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: LGET requires key\r\n";
    auto elems = db.lget(std::string(t[1].data(), t[1].size()));
    std::string out = arrayHeader(elems.size());
    for (const auto &e : elems) out += bulkStringFromStd(e);
    return out;
}

static std::string handleLrem(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 4) return "-Error: LREM requires key, count and value\r\n";
    try
    {
        int count = std::stoi(std::string(t[2].data(), t[2].size()));
        int removed = db.lrem(std::string(t[1].data(), t[1].size()), count, std::string(t[3].data(), t[3].size()));
        return integerReply(removed);
    }
    catch (...) { return "-Error: Invalid count\r\n"; }
}

static std::string handleLindex(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 3) return "-Error: LINDEX requires key and index\r\n";
    try
    {
        int idx = std::stoi(std::string(t[2].data(), t[2].size()));
        std::string v;
        if (db.lindex(std::string(t[1].data(), t[1].size()), idx, v)) return bulkStringFromStd(v);
        return nilBulk();
    }
    catch (...) { return "-Error: Invalid index\r\n"; }
}

static std::string handleLset(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 4) return "-Error: LSET requires key, index and value\r\n";
    try
    {
        int idx = std::stoi(std::string(t[2].data(), t[2].size()));
        return db.lset(std::string(t[1].data(), t[1].size()), idx, std::string(t[3].data(), t[3].size())) ? simpleString("OK") : "-Error: Index out of range\r\n";
    }
    catch (...) { return "-Error: Invalid index\r\n"; }
}

// ---- Hash handlers ----
static std::string handleHset(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 4) return "-Error: HSET requires key, field and value\r\n";
    db.hset(std::string(t[1].data(), t[1].size()), std::string(t[2].data(), t[2].size()), std::string(t[3].data(), t[3].size()));
    return integerReply(1);
}

static std::string handleHget(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 3) return "-Error: HGET requires key and field\r\n";
    std::string v;
    if (db.hget(std::string(t[1].data(), t[1].size()), std::string(t[2].data(), t[2].size()), v)) return bulkStringFromStd(v);
    return nilBulk();
}

static std::string handleHexists(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 3) return "-Error: HEXISTS requires key and field\r\n";
    return integerReply(db.hexists(std::string(t[1].data(), t[1].size()), std::string(t[2].data(), t[2].size())) ? 1 : 0);
}

static std::string handleHdel(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 3) return "-Error: HDEL requires key and field\r\n";
    return integerReply(db.hdel(std::string(t[1].data(), t[1].size()), std::string(t[2].data(), t[2].size())) ? 1 : 0);
}

static std::string handleHgetall(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: HGETALL requires key\r\n";
    auto map = db.hgetall(std::string(t[1].data(), t[1].size()));
    std::string out = arrayHeader(map.size() * 2);
    for (const auto &p : map)
    {
        out += bulkStringFromStd(p.first);
        out += bulkStringFromStd(p.second);
    }
    return out;
}

static std::string handleHkeys(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: HKEYS requires key\r\n";
    auto keys = db.hkeys(std::string(t[1].data(), t[1].size()));
    std::string out = arrayHeader(keys.size());
    for (const auto &k : keys) out += bulkStringFromStd(k);
    return out;
}

static std::string handleHvals(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: HVALS requires key\r\n";
    auto vals = db.hvals(std::string(t[1].data(), t[1].size()));
    std::string out = arrayHeader(vals.size());
    for (const auto &v : vals) out += bulkStringFromStd(v);
    return out;
}

static std::string handleHlen(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 2) return "-Error: HLEN requires key\r\n";
    return integerReply(db.hlen(std::string(t[1].data(), t[1].size())));
}

static std::string handleHmset(const std::vector<std::string_view> &t, RedisDatabase &db)
{
    if (t.size() < 4 || ((t.size() - 2) % 2) != 0) return "-Error: HMSET requires field-value pairs\r\n";
    std::vector<std::pair<std::string, std::string>> fv;
    fv.reserve((t.size() - 2) / 2);
    for (size_t i = 2; i + 1 < t.size(); i += 2)
        fv.emplace_back(std::string(t[i].data(), t[i].size()), std::string(t[i + 1].data(), t[i + 1].size()));
    db.hmset(std::string(t[1].data(), t[1].size()), fv);
    return simpleString("OK");
}

// -----------------------------------------------------------------------------
// processCommand overloads
// -----------------------------------------------------------------------------

RedisCommandHandler::RedisCommandHandler() {}

// Legacy entrypoint: convert to string_view and call hot path
std::string RedisCommandHandler::processCommand(const std::string &commandLine)
{
    return processCommand(std::string_view(commandLine.data(), commandLine.size()));
}

// Hot path: accepts string_view -> zero-copy parse
std::string RedisCommandHandler::processCommand(std::string_view commandView)
{
    auto tokens = parseRespViews(commandView);
    if (tokens.empty())
        return "-Error: Empty command\r\n";

    // Make uppercase command string (short allocation; unavoidable for many comparisons)
    std::string cmd(tokens[0].data(), tokens[0].size());
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return std::toupper(c); });

    RedisDatabase &db = RedisDatabase::getInstance();

    // --- top-level dispatch (kept simple and branchy for performance) ---
    if (cmd == "PING") return handlePing();
    if (cmd == "ECHO") return handleEcho(tokens);
    if (cmd == "SET") return handleSet(tokens, db);
    if (cmd == "GET") return handleGet(tokens, db);
    if (cmd == "DEL" || cmd == "UNLINK") return handleDel(tokens, db);
    if (cmd == "FLUSHALL") return handleFlushAll(db);
    if (cmd == "KEYS") return handleKeys(tokens, db);
    if (cmd == "TYPE") return handleType(tokens, db);
    if (cmd == "EXPIRE") return handleExpire(tokens, db);
    if (cmd == "RENAME") return handleRename(tokens, db);

    // list
    if (cmd == "LPUSH") return handleLpush(tokens, db);
    if (cmd == "RPUSH") return handleRpush(tokens, db);
    if (cmd == "LPOP") return handleLpop(tokens, db);
    if (cmd == "RPOP") return handleRpop(tokens, db);
    if (cmd == "LLEN") return handleLlen(tokens, db);
    if (cmd == "LGET") return handleLget(tokens, db);
    if (cmd == "LREM") return handleLrem(tokens, db);
    if (cmd == "LINDEX") return handleLindex(tokens, db);
    if (cmd == "LSET") return handleLset(tokens, db);

    // hash
    if (cmd == "HSET") return handleHset(tokens, db);
    if (cmd == "HGET") return handleHget(tokens, db);
    if (cmd == "HEXISTS") return handleHexists(tokens, db);
    if (cmd == "HDEL") return handleHdel(tokens, db);
    if (cmd == "HGETALL") return handleHgetall(tokens, db);
    if (cmd == "HKEYS") return handleHkeys(tokens, db);
    if (cmd == "HVALS") return handleHvals(tokens, db);
    if (cmd == "HLEN") return handleHlen(tokens, db);
    if (cmd == "HMSET") return handleHmset(tokens, db);

    // Unknown command — log and reply
    Logger::getInstance().warn("Unknown command: " + cmd);
    return "-Error: Unknown command\r\n";
}

// -----------------------------------------------------------------------------
// splitFrames: extract complete RESP frames from an input buffer (used for pipelining)
// returns vector of std::string frames (owning buffers) for further processing.
// -----------------------------------------------------------------------------
std::vector<std::string> RedisCommandHandler::splitFrames(const std::string &buffer)
{
    std::vector<std::string> frames;
    size_t pos = 0;
    while (pos < buffer.size())
    {
        if (buffer[pos] != '*')
            break;

        size_t rn = buffer.find("\r\n", pos);
        if (rn == std::string::npos) break;

        int elements = 0;
        try
        {
            elements = std::stoi(buffer.substr(pos + 1, rn - pos - 1));
        }
        catch (...)
        {
            break;
        }

        size_t cursor = rn + 2;
        bool valid = true;
        for (int i = 0; i < elements; ++i)
        {
            if (cursor >= buffer.size() || buffer[cursor] != '$') { valid = false; break; }
            ++cursor;
            size_t rn2 = buffer.find("\r\n", cursor);
            if (rn2 == std::string::npos) { valid = false; break; }

            int len = 0;
            try { len = std::stoi(buffer.substr(cursor, rn2 - cursor)); }
            catch (...) { valid = false; break; }

            cursor = rn2 + 2;
            if (cursor + static_cast<size_t>(len) + 2 > buffer.size()) { valid = false; break; }
            cursor += static_cast<size_t>(len) + 2;
        }

        if (!valid) break;

        size_t frame_end = cursor;
        frames.emplace_back(buffer.substr(pos, frame_end - pos));
        pos = frame_end;
    }

    return frames;
}
