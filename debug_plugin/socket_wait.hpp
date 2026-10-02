#pragma once

#include <winsock2.h>
#include <windows.h>

namespace dayz::debug_transport
{
    // WSAEventSelect makes the socket nonblocking. Both network readiness and
    // shutdown wake one wait, so Stop never depends on a peer sending more bytes.
    class SocketWait
    {
    public:
        SocketWait(SOCKET socket, HANDLE stop, long events) noexcept
            : socket_(socket), stop_(stop), event_(WSACreateEvent())
        {
            valid_ = event_ != WSA_INVALID_EVENT && WSAEventSelect(socket_, event_, events) == 0;
        }
        ~SocketWait()
        {
            if (event_ != WSA_INVALID_EVENT)
                WSACloseEvent(event_);
        }
        SocketWait(const SocketWait&) = delete;
        SocketWait& operator=(const SocketWait&) = delete;
        bool Valid() const noexcept { return valid_; }
        bool Wait() const noexcept
        {
            const HANDLE handles[]{stop_, event_};
            if (!valid_ || WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
                return false;
            WSANETWORKEVENTS events{};
            return WSAEnumNetworkEvents(socket_, event_, &events) == 0;
        }
    private:
        SOCKET socket_;
        HANDLE stop_;
        WSAEVENT event_;
        bool valid_{};
    };
}
