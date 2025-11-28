#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"
#include "../include/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

/* ============================================================================
                           RESP BUILDERS
============================================================================ */
static inline std::string simpleString(const std::string &s)
{
    return "+" + s + "\r\n";
}

static inline std::string errorString(const std::string &s)
{
    return "-ERR " + s + "\r\n";
}

static inline std::string bulkString(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 32);
    out += "$" + std::to_string(s.size()) + "\r\n";
    out += s + "\r\n";
    return out;
}

static inline std::string bulkStringView(std::string_view v)
{
    std::string out;
    out.reserve(v.size() + 32);
    out += "$" + std::to_string(v.size()) + "\r\n";
    out.append(v.data(), v.size());
    out += "\r\n";
    return out;
}

static inline std::string nilBulk() { return "$-1\r\n"; }
static inline std::string integerReply(long long n) { return ":" + std::to_string(n) + "\r\n"; }
static inline std::string arrayHeader(size_t n) { return "*" + std::to_string(n) + "\r\n"; }

/* ============================================================================
                               CONSTRUCTOR
============================================================================ */
RedisCommandHandler::RedisCommandHandler() {}

/* ============================================================================
             RESP PARSER → vector<string_view>
============================================================================ */
std::vector<std::string_view>
RedisCommandHandler::parseRespViews(std::string_view input)
{
    std::vector<std::string_view> out;
    if (input.empty())
        return out;

    /* INLINE COMMAND */
    if (input[0] != '*')
    {
        size_t i = 0;
        while (i < input.size())
        {
            while (i < input.size() && std::isspace((unsigned char)input[i]))
                i++;

            if (i >= input.size())
                break;

            size_t j = i;
            while (j < input.size() && !std::isspace((unsigned char)input[j]))
                j++;

            out.emplace_back(input.data() + i, j - i);
            i = j;
        }
        return out;
    }

    /* RESP HEADER */
    size_t pos = 1;
    size_t rn = input.find("\r\n", pos);
    if (rn == std::string_view::npos)
        return out;

    int elements = 0;
    try
    {
        elements = std::stoi(std::string(input.substr(pos, rn - pos)));
    }
    catch (...)
    {
        return out;
    }

    if (elements <= 0 || elements > 1'000'000)
        return out;

    pos = rn + 2;
    out.reserve(elements);

    /* BULK STRINGS */
    for (int i = 0; i < elements; i++)
    {
        if (pos >= input.size() || input[pos] != '$')
            return {};

        pos++;
        size_t rn2 = input.find("\r\n", pos);
        if (rn2 == std::string_view::npos)
            return {};

        int len = 0;
        try
        {
            len = std::stoi(std::string(input.substr(pos, rn2 - pos)));
        }
        catch (...)
        {
            return {};
        }

        if (len < 0 || len > 512 * 1024 * 1024)
            return {};

        pos = rn2 + 2;

        if (pos + len + 2 > input.size())
            return {};

        out.emplace_back(input.data() + pos, len);

        pos += len + 2;
    }

    return out;
}

/* ============================================================================
              MATERIALIZE string_view → string
============================================================================ */
std::vector<std::string>
RedisCommandHandler::materialize(const std::vector<std::string_view> &views)
{
    std::vector<std::string> out;
    out.reserve(views.size());
    for (auto &v : views)
        out.emplace_back(v.data(), v.size());
    return out;
}

/* ============================================================================
                               WRAPPER
============================================================================ */
std::string RedisCommandHandler::processCommand(const std::string &commandLine)
{
    return processCommand(std::string_view(commandLine));
}

/* ============================================================================
                               DISPATCHER
============================================================================ */
std::string RedisCommandHandler::processCommand(std::string_view command)
{
    auto tokens = parseRespViews(command);
    if (tokens.empty())
        return errorString("empty command");

    std::string cmd(tokens[0]);
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

    RedisDatabase &db = RedisDatabase::getInstance();

    /* ------------------------------ PING ----------------------------------- */
    if (cmd == "PING")
        return simpleString("PONG");

    if (cmd == "ECHO")
    {
        if (tokens.size() < 2)
            return errorString("missing arg");
        return bulkStringView(tokens[1]);
    }

    /* ------------------------------ SET ------------------------------------ */
    if (cmd == "SET")
    {
        if (tokens.size() < 3)
            return errorString("SET key value");

        std::string key(tokens[1]);
        std::string value(tokens[2]);
        db.set(key, value);

        if (tokens.size() >= 5)
        {
            std::string opt(tokens[3]);
            std::transform(opt.begin(), opt.end(), opt.begin(), ::toupper);
            int time = std::stoi(std::string(tokens[4]));
            if (opt == "EX")
                db.expire(key, time);
            else if (opt == "PX")
                db.expire(key, (time + 999) / 1000);
        }

        return simpleString("OK");
    }

    /* ------------------------------ GET ------------------------------------ */
    if (cmd == "GET")
    {
        if (tokens.size() < 2)
            return errorString("GET key");

        std::string val;
        if (!db.get(std::string(tokens[1]), val))
            return nilBulk();
        return bulkString(val);
    }

    /* ------------------------------ DEL ------------------------------------ */
    if (cmd == "DEL")
    {
        if (tokens.size() < 2)
            return errorString("DEL key");
        return integerReply(db.del(std::string(tokens[1])) ? 1 : 0);
    }

    /* ------------------------------ EXPIRE ---------------------------------- */
    if (cmd == "EXPIRE")
    {
        if (tokens.size() < 3)
            return errorString("EXPIRE key sec");
        int sec = std::stoi(std::string(tokens[2]));
        return integerReply(db.expire(std::string(tokens[1]), sec));
    }

    /* =========================================================================
                            LIST COMMANDS
    ========================================================================= */

    if (cmd == "LPUSH")
    {
        std::string key(tokens[1]);
        for (size_t i = 2; i < tokens.size(); i++)
            db.lpush(key, std::string(tokens[i]));
        return integerReply(db.llen(key));
    }

    if (cmd == "RPUSH")
    {
        std::string key(tokens[1]);
        for (size_t i = 2; i < tokens.size(); i++)
            db.rpush(key, std::string(tokens[i]));
        return integerReply(db.llen(key));
    }

    if (cmd == "LPOP")
    {
        std::string val;
        if (db.lpop(std::string(tokens[1]), val))
            return bulkString(val);
        return nilBulk();
    }

    if (cmd == "RPOP")
    {
        std::string val;
        if (db.rpop(std::string(tokens[1]), val))
            return bulkString(val);
        return nilBulk();
    }

    /* ------------------------------ LINDEX --------------------------------- */
    if (cmd == "LINDEX")
    {
        int idx = std::stoi(std::string(tokens[2]));
        std::string val;
        if (db.lindex(std::string(tokens[1]), idx, val))
            return bulkString(val);
        return nilBulk();
    }

    /* ------------------------------ LSET ----------------------------------- */
    if (cmd == "LSET")
    {
        int idx = std::stoi(std::string(tokens[2]));
        if (db.lset(std::string(tokens[1]), idx, std::string(tokens[3])))
            return simpleString("OK");
        return errorString("index out of range");
    }

    /* ------------------------------ LRANGE --------------------------------- */
    if (cmd == "LRANGE")
    {
        std::string key(tokens[1]);
        int start = std::stoi(std::string(tokens[2]));
        int stop = std::stoi(std::string(tokens[3]));

        auto vec = db.lget(key);
        int n = vec.size();

        if (start < 0)
            start = n + start;
        if (stop < 0)
            stop = n + stop;

        if (start < 0)
            start = 0;
        if (stop >= n)
            stop = n - 1;

        if (start > stop || start >= n)
            return arrayHeader(0);

        std::string out = arrayHeader(stop - start + 1);
        for (int i = start; i <= stop; i++)
            out += bulkString(vec[i]);
        return out;
    }

    /* ------------------------------ LREM ----------------------------------- */
    if (cmd == "LREM")
    {
        if (tokens.size() < 4)
            return errorString("LREM key count value");

        std::string key(tokens[1]);
        int count = std::stoi(std::string(tokens[2]));
        std::string value(tokens[3]);

        int removed = db.lrem(key, count, value);
        return integerReply(removed);
    }

    /* =========================================================================
                            HASH COMMANDS
    ========================================================================= */

    if (cmd == "HSET")
    {
        db.hset(std::string(tokens[1]), std::string(tokens[2]), std::string(tokens[3]));
        return integerReply(1);
    }

    if (cmd == "HGET")
    {
        std::string val;
        if (db.hget(std::string(tokens[1]), std::string(tokens[2]), val))
            return bulkString(val);
        return nilBulk();
    }

    if (cmd == "HEXISTS")
        return integerReply(db.hexists(std::string(tokens[1]), std::string(tokens[2])));

    if (cmd == "HGETALL")
    {
        auto map = db.hgetall(std::string(tokens[1]));
        std::string out = arrayHeader(map.size() * 2);
        for (auto &p : map)
        {
            out += bulkString(p.first);
            out += bulkString(p.second);
        }
        return out;
    }

    /* ------------------------------ INCR ----------------------------------- */
    if (cmd == "INCR")
    {
        long long v;
        if (!db.incr(std::string(tokens[1]), v))
            return errorString("value is not an integer");
        return integerReply(v);
    }

    Logger::getInstance().warn("Unknown command: " + cmd);
    return errorString("unknown command");
}

/* ============================================================================
                      FRAME SPLITTER (RESP)
============================================================================ */
std::vector<std::string>
RedisCommandHandler::splitFrames(std::string &buffer)
{
    std::vector<std::string> frames;
    size_t cursor = 0;
    size_t n = buffer.size();
    const size_t MAX_FRAMES = 1000;
    size_t count = 0;

    while (cursor < n && count < MAX_FRAMES)
    {
        if (buffer[cursor] != '*')
        {
            size_t nl = buffer.find("\r\n", cursor);
            if (nl == std::string::npos)
                break;

            frames.emplace_back(buffer.substr(cursor, nl + 2 - cursor));
            cursor = nl + 2;
            count++;
            continue;
        }

        size_t rn = buffer.find("\r\n", cursor + 1);
        if (rn == std::string::npos)
            break;

        int elements = 0;
        try
        {
            elements = std::stoi(buffer.substr(cursor + 1, rn - cursor - 1));
        }
        catch (...)
        {
            break;
        }

        if (elements <= 0 || elements > 1'000'000)
            break;

        size_t pos = rn + 2;
        bool ok = true;

        for (int i = 0; i < elements; i++)
        {
            if (pos >= n || buffer[pos] != '$')
            {
                ok = false;
                break;
            }

            size_t rn2 = buffer.find("\r\n", pos + 1);
            if (rn2 == std::string::npos)
            {
                ok = false;
                break;
            }

            int len = std::stoi(buffer.substr(pos + 1, rn2 - pos - 1));

            pos = rn2 + 2;
            if (pos + len + 2 > n)
            {
                ok = false;
                break;
            }

            pos += len + 2;
        }

        if (!ok)
            break;

        frames.emplace_back(buffer.substr(cursor, pos - cursor));
        cursor = pos;
        count++;
    }

    if (cursor > 0)
        buffer.erase(0, cursor);

    return frames;
}
