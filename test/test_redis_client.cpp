#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

// ----------------------------------------------------------
// CONNECT TO SERVER
// ----------------------------------------------------------
int connectToServer(const std::string &host, int port)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        std::cerr << "Socket creation failed\n";
        return -1;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(host.c_str());

    if (connect(sock, (sockaddr *)&server, sizeof(server)) < 0)
    {
        std::cerr << "Connection to server failed\n";
        return -1;
    }

    return sock;
}

// ----------------------------------------------------------
// SEND RESP COMMAND
// ----------------------------------------------------------
std::string sendCommand(int sock, const std::string &cmd)
{
    send(sock, cmd.c_str(), cmd.size(), 0);

    char buffer[4096];
    memset(buffer, 0, sizeof(buffer));
    int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);

    if (bytes > 0)
        return std::string(buffer, bytes);

    return "ERR: No response";
}

// RESP builders
std::string respArray(const std::vector<std::string> &parts)
{
    std::string out = "*" + std::to_string(parts.size()) + "\r\n";
    for (const auto &p : parts)
    {
        out += "$" + std::to_string(p.size()) + "\r\n" + p + "\r\n";
    }
    return out;
}

// ----------------------------------------------------------
// MEANINGFUL INDIVIDUAL TESTS
// ----------------------------------------------------------

// Test 1: PING & ECHO
void testPingEcho(int sock)
{
    std::cout << "\n[TEST] PING/ECHO\n";
    std::cout << sendCommand(sock, respArray({"PING"}));
    std::cout << sendCommand(sock, respArray({"ECHO", "Hello Redis"}));
}

// Test 2: SET/GET correctness
void testSetGet(int sock)
{
    std::cout << "\n[TEST] SET/GET\n";
    std::cout << sendCommand(sock, respArray({"SET", "testKey", "12345"}));
    std::cout << sendCommand(sock, respArray({"GET", "testKey"}));
}

// Test 3: Atomic increment simulation (LPUSH + LLEN)
void testAtomicIncrement(int sock)
{
    std::cout << "\n[TEST] Atomic List Increment\n";

    // Push value
    sendCommand(sock, respArray({"LPUSH", "counterList", "X"}));

    // Read list size
    std::cout << "LLEN: ";
    std::cout << sendCommand(sock, respArray({"LLEN", "counterList"}));
}

// Test 4: List operations
void testListOps(int sock)
{
    std::cout << "\n[TEST] LIST Operations\n";
    sendCommand(sock, respArray({"DEL", "myList"}));
    std::cout << sendCommand(sock, respArray({"LPUSH", "myList", "A"}));
    std::cout << sendCommand(sock, respArray({"RPUSH", "myList", "B"}));
    std::cout << sendCommand(sock, respArray({"LGET", "myList"}));
}

// Test 5: Hash operations
void testHashOps(int sock)
{
    std::cout << "\n[TEST] HASH Operations\n";
    sendCommand(sock, respArray({"DEL", "user:1"}));
    std::cout << sendCommand(sock, respArray({"HSET", "user:1", "name", "Alice"}));
    std::cout << sendCommand(sock, respArray({"HSET", "user:1", "age", "22"}));
    std::cout << sendCommand(sock, respArray({"HGETALL", "user:1"}));
}

// Test 6: Expiry behavior
void testExpire(int sock)
{
    std::cout << "\n[TEST] EXPIRY\n";
    std::cout << sendCommand(sock, respArray({"SET", "tempkey", "temporary"}));
    std::cout << sendCommand(sock, respArray({"EXPIRE", "tempkey", "1"})); // expire in 1 sec

    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << sendCommand(sock, respArray({"GET", "tempkey"})); // should return nil
}

// ----------------------------------------------------------
// WORKER THREAD (for concurrency showcase)
// ----------------------------------------------------------
void workerThread(int id)
{
    int sock = connectToServer("127.0.0.1", 6379);
    if (sock < 0)
        return;

    std::string key = "client" + std::to_string(id);
    std::string value = "value" + std::to_string(id);

    // SET
    sendCommand(sock, respArray({"SET", key, value}));

    // GET
    std::string resp = sendCommand(sock, respArray({"GET", key}));
    std::cout << "[Thread " << id << "] GET -> " << resp;

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

// ----------------------------------------------------------
// MAIN
// ----------------------------------------------------------
int main()
{
    std::cout << "\n========= CUSTOM REDIS SERVER TEST SUITE =========\n";

    int sock = connectToServer("127.0.0.1", 6379);
    if (sock < 0)
        return 0;

    // Run comprehensive tests
    testPingEcho(sock);
    testSetGet(sock);
    testListOps(sock);
    testHashOps(sock);
    testExpire(sock);
    testAtomicIncrement(sock);

    // ----------------------------------------------
    // Concurrency showcase (10 threads)
    // ----------------------------------------------
    std::cout << "\n[TEST] Concurrency Showcase (10 threads)\n";
    std::vector<std::thread> clients;
    for (int i = 1; i <= 10; i++)
        clients.emplace_back(workerThread, i);

    for (auto &t : clients)
        t.join();

    std::cout << "\n========= TEST SUITE FINISHED =========\n";
    return 0;
}
