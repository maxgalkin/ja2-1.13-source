/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 */

#include "../include/SidecarClient.h"

#include <string.h>
#include <stdlib.h>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tacnn
{
    int64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // ----------------------------------------------------------------------
    // framing
    // ----------------------------------------------------------------------

    namespace protocol
    {
        void encodeRequest(TacnnSidecarRequest head, const TacnnObservation& obs, const TacnnActionMask& mask,
                           const TacnnCandidateSet& cand, uint8_t* out)
        {
            head.magic = TACNN_MAGIC_REQUEST;
            head.schema_version = TACNN_SCHEMA_VERSION;
            head.schema_hash = TACNN_SCHEMA_HASH;
            size_t at = 0;
            memcpy(out + at, &head, sizeof head); at += sizeof head;
            memcpy(out + at, &obs, sizeof obs); at += sizeof obs;
            memcpy(out + at, &mask, sizeof mask); at += sizeof mask;
            memcpy(out + at, &cand, sizeof cand); at += sizeof cand;
        }

        bool decodeResponse(const uint8_t* in, uint32_t expectedSequence, TacnnPolicyAction& action)
        {
            TacnnSidecarResponse response;
            memcpy(&response, in, sizeof response);
            if(response.magic != TACNN_MAGIC_RESPONSE)
                return false;
            if(response.schema_hash != TACNN_SCHEMA_HASH)
                return false;
            if(response.sequence != expectedSequence)
                return false;
            action = response.policy_action;
            return true;
        }
    }

    // ----------------------------------------------------------------------
    // transport
    // ----------------------------------------------------------------------

    /// A connected byte stream with deadlines. One implementation per platform.
    class Transport
    {
        public:
            virtual ~Transport() { }
            virtual bool connect(const std::string& name, int64_t deadline, const char*& error) = 0;
            virtual bool writeAll(const uint8_t* data, size_t bytes, int64_t deadline, const char*& error) = 0;
            virtual bool readAll(uint8_t* data, size_t bytes, int64_t deadline, const char*& error) = 0;
            virtual void close() = 0;
            virtual bool isOpen() const = 0;
    };

#ifdef _WIN32

    class PipeTransport : public Transport
    {
        public:
            PipeTransport() : handle_(INVALID_HANDLE_VALUE), event_(0) { }
            virtual ~PipeTransport() { close(); }

            virtual bool connect(const std::string& name, int64_t deadline, const char*& error)
            {
                close();
                for(;;)
                {
                    handle_ = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                          FILE_FLAG_OVERLAPPED, NULL);
                    if(handle_ != INVALID_HANDLE_VALUE)
                        break;
                    DWORD err = GetLastError();
                    if(err != ERROR_PIPE_BUSY)
                    {
                        error = err == ERROR_FILE_NOT_FOUND ? "no server" : "CreateFile failed";
                        return false;
                    }
                    int64_t remaining = deadline - NowMs();
                    if(remaining <= 0 || !WaitNamedPipeA(name.c_str(), static_cast<DWORD>(remaining)))
                    {
                        error = "pipe busy";
                        return false;
                    }
                }
                DWORD mode = PIPE_READMODE_BYTE;
                SetNamedPipeHandleState(handle_, &mode, NULL, NULL);
                event_ = CreateEventA(NULL, TRUE, FALSE, NULL);
                if(!event_)
                {
                    error = "CreateEvent failed";
                    close();
                    return false;
                }
                return true;
            }

            virtual bool writeAll(const uint8_t* data, size_t bytes, int64_t deadline, const char*& error)
            {
                size_t done = 0;
                while(done < bytes)
                {
                    DWORD moved = 0;
                    if(!transfer(false, const_cast<uint8_t*>(data) + done, bytes - done, deadline, moved, error))
                        return false;
                    done += moved;
                }
                return true;
            }

            virtual bool readAll(uint8_t* data, size_t bytes, int64_t deadline, const char*& error)
            {
                size_t done = 0;
                while(done < bytes)
                {
                    DWORD moved = 0;
                    if(!transfer(true, data + done, bytes - done, deadline, moved, error))
                        return false;
                    if(moved == 0)
                    {
                        error = "pipe closed";
                        return false;
                    }
                    done += moved;
                }
                return true;
            }

            virtual void close()
            {
                if(handle_ != INVALID_HANDLE_VALUE)
                {
                    CancelIo(handle_);
                    CloseHandle(handle_);
                    handle_ = INVALID_HANDLE_VALUE;
                }
                if(event_)
                {
                    CloseHandle(event_);
                    event_ = 0;
                }
            }

            virtual bool isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

        private:
            bool transfer(bool reading, uint8_t* data, size_t bytes, int64_t deadline, DWORD& moved, const char*& error)
            {
                OVERLAPPED overlapped;
                memset(&overlapped, 0, sizeof overlapped);
                overlapped.hEvent = event_;
                ResetEvent(event_);
                BOOL ok = reading
                    ? ReadFile(handle_, data, static_cast<DWORD>(bytes), NULL, &overlapped)
                    : WriteFile(handle_, data, static_cast<DWORD>(bytes), NULL, &overlapped);
                if(!ok)
                {
                    DWORD err = GetLastError();
                    if(err != ERROR_IO_PENDING)
                    {
                        error = reading ? "ReadFile failed" : "WriteFile failed";
                        return false;
                    }
                    int64_t remaining = deadline - NowMs();
                    if(remaining < 0)
                        remaining = 0;
                    DWORD waited = WaitForSingleObject(event_, static_cast<DWORD>(remaining));
                    if(waited != WAIT_OBJECT_0)
                    {
                        // a late answer would desynchronise the stream, so the connection goes
                        CancelIo(handle_);
                        error = "timeout";
                        return false;
                    }
                }
                if(!GetOverlappedResult(handle_, &overlapped, &moved, FALSE))
                {
                    error = "GetOverlappedResult failed";
                    return false;
                }
                return true;
            }

            HANDLE handle_;
            HANDLE event_;
    };

    typedef PipeTransport PlatformTransport;

#else

    /// `tcp:<port>` on the loopback interface, for tests on systems without named pipes.
    class SocketTransport : public Transport
    {
        public:
            SocketTransport() : fd_(-1) { }
            virtual ~SocketTransport() { close(); }

            virtual bool connect(const std::string& name, int64_t deadline, const char*& error)
            {
                close();
                if(name.compare(0, 4, "tcp:") != 0)
                {
                    error = "name is not tcp:<port>";
                    return false;
                }
                int port = atoi(name.c_str() + 4);
                fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
                if(fd_ < 0)
                {
                    error = "socket failed";
                    return false;
                }
                fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
                sockaddr_in addr;
                memset(&addr, 0, sizeof addr);
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<uint16_t>(port));
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
                if(rc != 0 && errno != EINPROGRESS)
                {
                    error = "no server";
                    close();
                    return false;
                }
                if(rc != 0)
                {
                    if(!wait(POLLOUT, deadline))
                    {
                        error = "connect timeout";
                        close();
                        return false;
                    }
                    int soerr = 0;
                    socklen_t len = sizeof soerr;
                    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &len);
                    if(soerr != 0)
                    {
                        error = "no server";
                        close();
                        return false;
                    }
                }
                return true;
            }

            virtual bool writeAll(const uint8_t* data, size_t bytes, int64_t deadline, const char*& error)
            {
                size_t done = 0;
                while(done < bytes)
                {
                    if(!wait(POLLOUT, deadline))
                    {
                        error = "timeout";
                        return false;
                    }
                    ssize_t n = ::send(fd_, data + done, bytes - done, MSG_NOSIGNAL);
                    if(n < 0)
                    {
                        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                            continue;
                        error = "send failed";
                        return false;
                    }
                    done += static_cast<size_t>(n);
                }
                return true;
            }

            virtual bool readAll(uint8_t* data, size_t bytes, int64_t deadline, const char*& error)
            {
                size_t done = 0;
                while(done < bytes)
                {
                    if(!wait(POLLIN, deadline))
                    {
                        error = "timeout";
                        return false;
                    }
                    ssize_t n = ::recv(fd_, data + done, bytes - done, 0);
                    if(n == 0)
                    {
                        error = "connection closed";
                        return false;
                    }
                    if(n < 0)
                    {
                        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                            continue;
                        error = "recv failed";
                        return false;
                    }
                    done += static_cast<size_t>(n);
                }
                return true;
            }

            virtual void close()
            {
                if(fd_ >= 0)
                {
                    ::close(fd_);
                    fd_ = -1;
                }
            }

            virtual bool isOpen() const { return fd_ >= 0; }

        private:
            bool wait(short events, int64_t deadline)
            {
                int64_t remaining = deadline - NowMs();
                if(remaining < 0)
                    remaining = 0;
                pollfd p;
                p.fd = fd_;
                p.events = events;
                p.revents = 0;
                int rc = ::poll(&p, 1, static_cast<int>(remaining));
                return rc > 0 && (p.revents & (events | POLLHUP | POLLERR)) != 0;
            }

            int fd_;
    };

    typedef SocketTransport PlatformTransport;

#endif

    // ----------------------------------------------------------------------
    // client
    // ----------------------------------------------------------------------

    namespace
    {
        const int64_t RETRY_BACKOFF_MS = 2000;
    }

    SidecarClient::SidecarClient()
        : name_("\\\\.\\pipe\\ja2mod-policy"), timeoutMs_(50), transport_(new PlatformTransport()), sequence_(0),
          failures_(0), answered_(0), retryAfter_(0), lastDeclined_(false), lastError_("")
    {
    }

    SidecarClient::~SidecarClient()
    {
        delete transport_;
    }

    void SidecarClient::configure(const std::string& name, unsigned timeoutMs)
    {
        disconnect();
        if(!name.empty())
            name_ = name;
        timeoutMs_ = timeoutMs ? timeoutMs : 50;
        retryAfter_ = 0;
    }

    bool SidecarClient::connected() const
    {
        return transport_->isOpen();
    }

    void SidecarClient::disconnect()
    {
        transport_->close();
    }

    bool SidecarClient::ensureConnected(int64_t deadline)
    {
        if(transport_->isOpen())
            return true;
        if(NowMs() < retryAfter_)
        {
            lastError_ = "backing off";
            return false;
        }
        const char* error = "";
        if(!transport_->connect(name_, deadline, error))
        {
            lastError_ = error;
            retryAfter_ = NowMs() + RETRY_BACKOFF_MS;
            return false;
        }
        lastError_ = "";
        return true;
    }

    bool SidecarClient::request(TacnnSidecarRequest head, const TacnnObservation& obs, const TacnnActionMask& mask,
                                const TacnnCandidateSet& cand, TacnnPolicyAction& action)
    {
        lastDeclined_ = false;
        const int64_t deadline = NowMs() + timeoutMs_;
        if(!ensureConnected(deadline))
        {
            ++failures_;
            return false;
        }

        head.sequence = ++sequence_;
        head.timeout_ms = static_cast<uint16_t>(timeoutMs_ > 65535 ? 65535 : timeoutMs_);
        static uint8_t buffer[protocol::REQUEST_BYTES];
        protocol::encodeRequest(head, obs, mask, cand, buffer);

        const char* error = "";
        uint8_t reply[protocol::RESPONSE_BYTES];
        if(!transport_->writeAll(buffer, sizeof buffer, deadline, error)
           || !transport_->readAll(reply, sizeof reply, deadline, error))
        {
            lastError_ = error;
            transport_->close();
            retryAfter_ = NowMs() + RETRY_BACKOFF_MS;
            ++failures_;
            return false;
        }
        if(!protocol::decodeResponse(reply, head.sequence, action))
        {
            lastError_ = "bad response frame";
            transport_->close();
            retryAfter_ = NowMs() + RETRY_BACKOFF_MS;
            ++failures_;
            return false;
        }
        ++answered_;
        lastError_ = "";
        if(action.decline)
        {
            lastDeclined_ = true;
            return false;
        }
        return true;
    }

    SidecarClient& GetSidecarClient()
    {
        static SidecarClient client;
        return client;
    }
}
