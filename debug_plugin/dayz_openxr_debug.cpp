// Optional debug plugin for the DayZ VR proxy. Loaded by dxgi.dll only when
// [debug] enabled=true; never linked into the proxy. Serves the line protocol in
// protocol.hpp on 127.0.0.1:<port> so a tool on the host (scripts/dayz-vr-ctl.py)
// can read tracking state and change tunables while DayZ runs.
#include "protocol.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{
    DayzVrDebugHost g_host{};
    std::atomic_bool g_running{};
    SOCKET g_listener{INVALID_SOCKET};
    std::thread g_thread;

    void Log(const char* message) noexcept
    {
        if (g_host.log)
            g_host.log(g_host.context, message);
    }

    std::string Handle(const std::string& line)
    {
        using namespace dayz::debug_protocol;
        const Command command = ParseCommand(line);
        switch (command.kind)
        {
        case CommandKind::Get:
        {
            DayzVrDebugState state{};
            state.struct_size = sizeof(state);
            g_host.get_state(g_host.context, &state);
            return FormatState(state);
        }
        case CommandKind::Tunables:
        {
            std::vector<char> buffer(4096);
            const size_t needed = g_host.list_tunables(g_host.context, buffer.data(), buffer.size());
            if (needed >= buffer.size())
            {
                buffer.resize(needed + 1);
                g_host.list_tunables(g_host.context, buffer.data(), buffer.size());
            }
            return FormatTunables(buffer.data());
        }
        case CommandKind::Set:
            return FormatResult(g_host.set_tunable(g_host.context, command.name.c_str(),
                command.value), "unknown tunable", "value out of range");
        case CommandKind::Recenter:
            return FormatResult(g_host.run_command(g_host.context, "recenter"),
                "unknown command", "command failed");
        case CommandKind::Ping:
            return "{\"ok\":true,\"pong\":true}";
        case CommandKind::Invalid:
            break;
        }
        return "{\"ok\":false,\"error\":\"unknown command\"}";
    }

    void Serve(SOCKET client)
    {
        std::string pending;
        char chunk[512]{};
        while (g_running.load())
        {
            const int received = recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0)
                break;
            pending.append(chunk, static_cast<size_t>(received));
            for (auto newline = pending.find('\n'); newline != std::string::npos;
                newline = pending.find('\n'))
            {
                std::string reply = Handle(pending.substr(0, newline));
                pending.erase(0, newline + 1);
                reply += '\n';
                size_t sent = 0;
                while (sent < reply.size())
                {
                    const int wrote = send(client, reply.data() + sent,
                        static_cast<int>(reply.size() - sent), 0);
                    if (wrote <= 0)
                        return;
                    sent += static_cast<size_t>(wrote);
                }
            }
            if (pending.size() > 4096)
                return; // a line that long is not one of ours
        }
    }

    void AcceptLoop()
    {
        while (g_running.load())
        {
            const SOCKET client = accept(g_listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                break;
            Serve(client);
            closesocket(client);
        }
    }
}

extern "C" __declspec(dllexport) int DayzVrDebugStart(const DayzVrDebugHost* host)
{
    if (!host || host->api_version != DAYZ_VR_DEBUG_API_VERSION ||
        host->struct_size < sizeof(DayzVrDebugHost) || !host->get_state ||
        !host->set_tunable || !host->list_tunables || !host->run_command)
        return -1;
    g_host = *host;

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        Log("Debug plugin: WSAStartup failed");
        return -1;
    }
    g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listener == INVALID_SOCKET)
    {
        Log("Debug plugin: socket() failed");
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(host->port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // never reachable from the LAN
    if (bind(g_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(g_listener, 1) != 0)
    {
        char message[96]{};
        std::snprintf(message, sizeof(message),
            "Debug plugin: bind/listen on 127.0.0.1:%u failed (%d)", host->port, WSAGetLastError());
        Log(message);
        closesocket(g_listener);
        g_listener = INVALID_SOCKET;
        return -1;
    }
    g_running.store(true);
    g_thread = std::thread(AcceptLoop);
    char message[96]{};
    std::snprintf(message, sizeof(message), "Debug plugin listening on 127.0.0.1:%u", host->port);
    Log(message);
    return 0;
}

extern "C" __declspec(dllexport) void DayzVrDebugStop(void)
{
    if (!g_running.exchange(false))
        return;
    if (g_listener != INVALID_SOCKET)
    {
        closesocket(g_listener);
        g_listener = INVALID_SOCKET;
    }
    if (g_thread.joinable())
        g_thread.join();
    WSACleanup();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
