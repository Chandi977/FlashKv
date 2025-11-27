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

// Helper function to connect to Redis server
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

// Send RESP formatted command
void sendCommand(int sock, const std::string &cmd)
{
    send(sock, cmd.c_str(), cmd.size(), 0);

    char buffer[2048];
    memset(buffer, 0, sizeof(buffer));
    int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);

    if (bytes > 0)
        std::cout << "[SERVER] " << buffer << std::endl;
    else
        std::cout << "Server closed connection\n";
}

// Worker thread test
void workerThread(int id)
{
    int sock = connectToServer("127.0.0.1", 6379);
    if (sock < 0)
        return;

    std::cout << "Client " << id << " connected\n";

    // Test PING
    sendCommand(sock, "*1\r\n$4\r\nPING\r\n");

    // Test SET
    std::string key = "client" + std::to_string(id);
    std::string value = "value" + std::to_string(id);

    std::string setCmd = "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" +
                         key + "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";

    sendCommand(sock, setCmd);

    // Test GET
    std::string getCmd = "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
    sendCommand(sock, getCmd);

    // Close connection
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

int main()
{
    // Launch 5 concurrent clients
    std::cout << "Starting Redis concurrency test...\n";

    std::vector<std::thread> clients;

    for (int i = 1; i <= 5; i++)
        clients.emplace_back(workerThread, i);

    for (auto &t : clients)
        t.join();

    std::cout << "Concurrent test finished.\n";
    return 0;
}
