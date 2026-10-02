#include "../common/dayz_vr_debug_api.h"
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int DayzVrDebugStart(const DayzVrDebugHost* host);
extern "C" void DayzVrDebugStop(void);

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "debug_plugin_lifecycle_test: %s (WSA=%d)\n", message, WSAGetLastError());
            std::exit(1);
        }
    }

    void GetState(void*, DayzVrDebugState*) {}
    int Set(void*, const char*, double) { return 0; }
    size_t List(void*, char* buffer, size_t capacity)
    {
        if (capacity) buffer[0] = '\0';
        return 0;
    }
    int Command(void*, const char*) { return 0; }
    void Log(void*, const char* text) { std::puts(text); }

    SOCKET Connect(unsigned short port)
    {
        const SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(client != INVALID_SOCKET, "client socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        Require(connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "connect");
        DWORD timeout = 2000;
        Require(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0, "timeout");
        Require(send(client, "ping\n", 5, 0) == 5, "ping send");
        char reply[64]{};
        const int length = recv(client, reply, sizeof(reply) - 1, 0);
        Require(length > 0 && std::strstr(reply, "pong"), "worker accepted and served client");
        return client;
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("lifecycle: starting");
    WSADATA data{};
    Require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "test winsock startup");
    const SOCKET occupied = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Require(bind(occupied, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "reserve port");
    int size = sizeof(address);
    Require(getsockname(occupied, reinterpret_cast<sockaddr*>(&address), &size) == 0, "port number");
    DayzVrDebugHost host{};
    host.struct_size = sizeof(host);
    host.api_version = DAYZ_VR_DEBUG_API_VERSION;
    host.port = ntohs(address.sin_port);
    host.get_state = GetState;
    host.set_tunable = Set;
    host.list_tunables = List;
    host.run_command = Command;
    host.log = Log;
    Require(DayzVrDebugStart(&host) == -1, "occupied port fails cleanly");
    std::puts("lifecycle: bind failure recovered");
    closesocket(occupied);
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        Require(DayzVrDebugStart(&host) == 0, "start/restart");
        Require(DayzVrDebugStart(&host) == -2, "repeat start rejected without replacing thread");
        std::printf("lifecycle: cycle %d connecting\n", cycle);
        const SOCKET client = Connect(host.port);
        std::puts("lifecycle: stopping idle client");
        const auto before = GetTickCount64();
        DayzVrDebugStop(); // client deliberately stays connected and silent
        Require(GetTickCount64() - before < 2000, "stop must wake idle accepted socket");
        DayzVrDebugStop();
        closesocket(client);
    }
    std::puts("lifecycle: stopping idle accept");
    Require(DayzVrDebugStart(&host) == 0, "start with no client");
    DayzVrDebugStop(); // blocked accept must wake too
    WSACleanup();
    std::puts("debug_plugin_lifecycle_test: occupied port, repeat start, idle client, restart and idle accept passed");
}
