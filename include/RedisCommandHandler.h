#ifndef REDIS_COMMAND_HANDLER_H
#define REDIS_COMMAND_HANDLER_H

#include <string>
#include <string_view>
#include <vector>

class RedisCommandHandler
{
public:
    RedisCommandHandler();

    // Process a command using string_view (zero-copy preferred)
    std::string processCommand(std::string_view commandView);

    // Legacy entrypoint (still supported)
    std::string processCommand(const std::string &commandLine);

    // Split an incoming TCP buffer into multiple RESP frames
    // Used for pipelining and handling multiple commands at once.
    std::vector<std::string> splitFrames(const std::string &buffer);

private:
    //
    // INTERNAL HELPERS
    //

    // Parse RESP tokens as string_view (no allocations)
    std::vector<std::string_view> parseRespViews(std::string_view input);

    // Convert string_view tokens → owning std::string
    // Needed when passing into your existing handlers.
    std::vector<std::string> materialize(const std::vector<std::string_view> &views);
};

#endif
