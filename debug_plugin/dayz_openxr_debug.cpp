// Optional debug plugin for the DayZ VR proxy. Loaded by dxgi.dll only when
// [debug] enabled=true; never linked into the proxy. Serves the line protocol in
// protocol.hpp on 127.0.0.1:<port> so a tool on the host (scripts/dayz-vr-ctl.py)
// can read tracking state and change tunables while DayZ runs.
#include "protocol.hpp"
#include "socket_wait.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <charconv>
#include <string>
#include <thread>
#include <vector>

namespace
{
    DayzVrDebugHost g_host{};
    std::atomic_bool g_running{};
    SOCKET g_listener{INVALID_SOCKET};
    std::thread g_thread;
    std::mutex g_lifecycleMutex;
    HANDLE g_stopEvent{};
    bool g_winsockStarted{};

    void Log(const char* message) noexcept
    {
        if (g_host.log)
        {
            try { g_host.log(g_host.context, message); }
            catch (...) { OutputDebugStringA("DayZ debug plugin: log callback threw.\n"); }
        }
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
        case CommandKind::Haptic:
            return FormatResult(g_host.run_command(g_host.context, "haptic"),
                "unknown command", "command failed");
        case CommandKind::DumpEyes:
            return FormatResult(g_host.run_command(g_host.context, "dump_eyes"),
                "unknown command", "command failed");
        case CommandKind::Action:
        {
            // The host parses "action <name> <value>" itself (run_command takes one string).
            std::string request = "action " + command.name + ' ';
            char number[32]{};
            const auto written = std::to_chars(number, number + sizeof(number), command.value);
            request.append(number, written.ptr);
            return FormatResult(g_host.run_command(g_host.context, request.c_str()),
                "unknown command or action", "direct input inactive");
        }
        case CommandKind::Ping:
            return "{\"ok\":true,\"pong\":true}";
        case CommandKind::Invalid:
            break;
        }
        return "{\"ok\":false,\"error\":\"unknown command\"}";
    }

    void Serve(SOCKET client)
    {
        dayz::debug_transport::SocketWait readiness(client, g_stopEvent, FD_READ | FD_WRITE | FD_CLOSE);
        if (!readiness.Valid())
            return;
        std::string pending;
        char chunk[512]{};
        while (g_running.load())
        {
            const int received = recv(client, chunk, sizeof(chunk), 0);
            if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            {
                if (!readiness.Wait())
                    break;
                continue;
            }
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
                    if (wrote == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
                    {
                        if (!readiness.Wait())
                            return;
                        continue;
                    }
                    if (wrote <= 0)
                        return;
                    sent += static_cast<size_t>(wrote);
                }
            }
            if (pending.size() > 4096)
                return; // a line that long is not one of ours
        }
    }

    void AcceptLoop(SOCKET listener) noexcept
    {
        dayz::debug_transport::SocketWait readiness(listener, g_stopEvent, FD_ACCEPT | FD_CLOSE);
        if (!readiness.Valid())
        {
            Log("Debug plugin: listener event setup failed");
            return;
        }
        while (g_running.load())
        {
            const SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
            {
                if (WSAGetLastError() == WSAEWOULDBLOCK && readiness.Wait())
                    continue;
                break;
            }
            try { Serve(client); }
            catch (...) { Log("Debug plugin: client request failed with an exception"); }
            closesocket(client);
        }
    }

    // Caller holds the lifecycle lock; the worker never takes it.
    void StopLocked()
    {
        g_running.store(false);
        if (g_stopEvent)
            SetEvent(g_stopEvent);
        if (g_thread.joinable())
            g_thread.join();
        if (g_listener != INVALID_SOCKET)
        {
            closesocket(g_listener);
            g_listener = INVALID_SOCKET;
        }
        if (g_stopEvent)
        {
            CloseHandle(g_stopEvent);
            g_stopEvent = nullptr;
        }
        if (g_winsockStarted)
        {
            WSACleanup();
            g_winsockStarted = false;
        }
    }

}

static int StartLocked(const DayzVrDebugHost* host)
{
    if (!host || host->api_version != DAYZ_VR_DEBUG_API_VERSION ||
        host->struct_size < sizeof(DayzVrDebugHost) || !host->get_state ||
        !host->set_tunable || !host->list_tunables || !host->run_command)
        return -1;
    if (g_thread.joinable())
        return -2; // Already started; do not replace callbacks underneath the worker.
    g_host = *host;

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        Log("Debug plugin: WSAStartup failed");
        return -1;
    }
    g_winsockStarted = true;
    g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listener == INVALID_SOCKET)
    {
        Log("Debug plugin: socket() failed");
        StopLocked();
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
        StopLocked();
        return -1;
    }
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent)
    {
        Log("Debug plugin: stop event creation failed");
        StopLocked();
        return -1;
    }
    g_running.store(true);
    g_thread = std::thread(AcceptLoop, g_listener);
    char message[96]{};
    std::snprintf(message, sizeof(message), "Debug plugin listening on 127.0.0.1:%u", host->port);
    Log(message);
    return 0;
}

extern "C" __declspec(dllexport) int DayzVrDebugStart(const DayzVrDebugHost* host)
{
    std::lock_guard lock(g_lifecycleMutex);
    try { return StartLocked(host); }
    catch (...)
    {
        StopLocked();
        Log("Debug plugin: start failed with an exception");
        return -1;
    }
}

extern "C" __declspec(dllexport) void DayzVrDebugStop(void)
{
    std::lock_guard lock(g_lifecycleMutex);
    StopLocked();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
