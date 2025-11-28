#ifndef REDIS_COMMAND_HANDLER_H
#define REDIS_COMMAND_HANDLER_H

#include <string>
#include <string_view>
#include <vector>

class RedisCommandHandler
{
public:
    RedisCommandHandler();

    // Zero-copy command handler using string_view
    std::string processCommand(std::string_view commandView);

    // Legacy function (safe)
    std::string processCommand(const std::string &commandLine);

    // IMPORTANT:
    // This version CONSUMES bytes from buffer (by reference),
    // removes complete RESP frames, and returns them.
    std::vector<std::string> splitFrames(std::string &buffer);

private:
    // RESP parser that returns zero-copy views
    std::vector<std::string_view> parseRespViews(std::string_view input);

    // Convert views to real strings for DB operations
    std::vector<std::string> materialize(const std::vector<std::string_view> &views);
};

#endif
