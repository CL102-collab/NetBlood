// SPDX-License-Identifier: GPL-2.0-or-later
#include "discord_rpc.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "sjson.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace discordrpc
{
namespace
{
typedef std::chrono::steady_clock Clock;
const size_t MaxMessage = 64 * 1024;
const size_t MaxEvents = 32;

uint32_t read32(unsigned char const* bytes)
{
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
           uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

void append32(std::vector<unsigned char>& bytes, uint32_t value)
{
    for (unsigned i = 0; i != 4; ++i) bytes.push_back((value >> (i * 8)) & 255);
}

// sjson validates UTF-8 while parsing. Bound nesting before calling its
// recursive parser, and reject embedded NUL rather than decoding a prefix.
bool boundedJson(std::string const& text)
{
    if (text.empty() || text.size() > MaxMessage) return false;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (unsigned char ch : text)
    {
        if (!ch) return false;
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[')
        {
            if (++depth > 64) return false;
        }
        else if (ch == '}' || ch == ']')
        {
            if (!depth) return false;
            --depth;
        }
    }
    return !quoted && !depth;
}

sjson_node* member(sjson_node* object, char const* key, sjson_tag tag)
{
    if (!object || object->tag != SJSON_OBJECT) return nullptr;
    sjson_node* value = sjson_find_member(object, key);
    return value && value->tag == tag ? value : nullptr;
}

char const* stringMember(sjson_node* object, char const* key)
{
    sjson_node* value = member(object, key, SJSON_STRING);
    return value ? value->string_ : "";
}

// Game strings can come from old byte-oriented map titles or user settings.
// Ensure invalid UTF-8 cannot reach sjson's encoder or Discord's API.
std::string displayText(std::string const& text, size_t limit)
{
    std::string result;
    for (size_t i = 0; i < text.size() && result.size() < limit;)
    {
        unsigned char first = text[i];
        if (first < 0x80)
        {
            result += (first < 32 || first == 127) ? ' ' : char(first);
            ++i;
            continue;
        }
        unsigned count = first >= 0xf0 && first <= 0xf4 ? 4 :
                         first >= 0xe0 && first <= 0xef ? 3 :
                         first >= 0xc2 && first <= 0xdf ? 2 : 0;
        uint32_t point = count ? (first & ((1 << (7 - count)) - 1)) : 0;
        bool valid = count && i + count <= text.size();
        for (unsigned j = 1; valid && j < count; ++j)
        {
            unsigned char next = text[i + j];
            valid = (next & 0xc0) == 0x80;
            point = (point << 6) | (next & 63);
        }
        valid = valid && point >= (count == 2 ? 0x80u : count == 3 ? 0x800u : 0x10000u) &&
                point <= 0x10ffff && !(point >= 0xd800 && point <= 0xdfff);
        if (!valid) { result += '?'; ++i; }
        else
        {
            if (result.size() + count > limit) break;
            result.append(text, i, count);
            i += count;
        }
    }
    // Discord requires at least two characters when these fields are present.
    if (result.size() == 1) result += ' ';
    return result;
}

bool secretText(std::string const& text)
{
    if (text.empty() || text.size() > 128) return false;
    for (unsigned char ch : text) if (ch < 33 || ch > 126) return false;
    return true;
}

Activity cleanActivity(Activity const& input)
{
    Activity result = input;
    result.details = displayText(input.details, 128);
    result.state = displayText(input.state, 128);
    result.partyId = displayText(input.partyId, 128);
    if (!secretText(result.joinSecret)) result.joinSecret.clear();
    if (result.partyId.empty() || result.partySize < 1 ||
        result.partyMax < result.partySize || result.partyMax > 10000)
    {
        result.joinSecret.clear();
        result.partyId.clear();
        result.partySize = result.partyMax = 0;
    }
    if (result.startedAt < 0) result.startedAt = 0;
    return result;
}

bool sameActivity(Activity const& a, Activity const& b)
{
    return a.details == b.details && a.state == b.state && a.joinSecret == b.joinSecret &&
           a.partyId == b.partyId && a.startedAt == b.startedAt &&
           a.partySize == b.partySize && a.partyMax == b.partyMax;
}

bool emptyActivity(Activity const& activity)
{
    return activity.details.empty() && activity.state.empty() && activity.joinSecret.empty();
}

struct Json
{
    sjson_context* context;
    Json() : context(sjson_create_context(64, 1024, nullptr)) {}
    ~Json() { sjson_destroy_context(context); }
    std::string encode(sjson_node* value)
    {
        char* encoded = sjson_encode(context, value);
        std::string result = encoded ? encoded : "";
        if (encoded) sjson_free_string(context, encoded);
        return result;
    }
};

std::string handshake(std::string const& applicationId)
{
    Json json;
    sjson_node* root = sjson_mkobject(json.context);
    sjson_put_int(json.context, root, "v", 1);
    sjson_put_string(json.context, root, "client_id", applicationId.c_str());
    return json.encode(root);
}

std::string subscription(uint64_t nonce)
{
    Json json;
    sjson_node* root = sjson_mkobject(json.context);
    sjson_put_string(json.context, root, "cmd", "SUBSCRIBE");
    sjson_put_string(json.context, root, "evt", "ACTIVITY_JOIN");
    sjson_put_string(json.context, root, "nonce", std::to_string(nonce).c_str());
    return json.encode(root);
}

std::string activityMessage(Activity const& activity, uint64_t nonce)
{
    Json json;
    sjson_node* root = sjson_mkobject(json.context);
    sjson_put_string(json.context, root, "cmd", "SET_ACTIVITY");
    sjson_put_string(json.context, root, "nonce", std::to_string(nonce).c_str());
    sjson_node* args = sjson_put_obj(json.context, root, "args");
#ifdef _WIN32
    sjson_put_int(json.context, args, "pid", int(GetCurrentProcessId()));
#else
    sjson_put_int(json.context, args, "pid", int(getpid()));
#endif
    if (emptyActivity(activity))
        sjson_append_member(json.context, args, "activity", sjson_mknull(json.context));
    else
    {
        sjson_node* data = sjson_put_obj(json.context, args, "activity");
        if (!activity.details.empty()) sjson_put_string(json.context, data, "details", activity.details.c_str());
        if (!activity.state.empty()) sjson_put_string(json.context, data, "state", activity.state.c_str());
        sjson_put_bool(json.context, data, "instance", true);
        if (activity.startedAt > 0)
        {
            sjson_node* times = sjson_put_obj(json.context, data, "timestamps");
            sjson_put_double(json.context, times, "start", double(activity.startedAt));
        }
        if (!activity.partyId.empty())
        {
            sjson_node* party = sjson_put_obj(json.context, data, "party");
            sjson_put_string(json.context, party, "id", activity.partyId.c_str());
            sjson_put_int(json.context, party, "privacy", 1); // Public, direct Join/invite.
            int size[2] = {activity.partySize, activity.partyMax};
            sjson_put_ints(json.context, party, "size", size, 2);
            if (!activity.joinSecret.empty())
            {
                sjson_node* secrets = sjson_put_obj(json.context, data, "secrets");
                sjson_put_string(json.context, secrets, "join", activity.joinSecret.c_str());
            }
        }
    }
    return json.encode(root);
}

// A byte stream with bounded, nonblocking reads and writes. Pipe/socket handles
// are owned exclusively by the worker, including during shutdown.
class Transport
{
public:
    Transport() = default;
    ~Transport() { close(); }

    bool open()
    {
#ifdef _WIN32
        for (unsigned i = 0; i < 10; ++i)
        {
            std::string path = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
            pipe = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) continue;
            readOperation.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            writeOperation.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            if (!readOperation.hEvent || !writeOperation.hEvent) { close(); return false; }
            return true;
        }
#else
        std::vector<std::string> bases;
        char const* variables[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
        for (char const* variable : variables)
        {
            char const* value = std::getenv(variable);
            if (value && value[0] == '/') bases.push_back(value);
        }
        bases.push_back("/run/user/" + std::to_string(geteuid()));
        bases.push_back("/tmp");
        char const* variants[] = {"/", "/app/com.discordapp.Discord/", "/snap.discord/"};
        for (std::string const& base : bases)
        for (char const* variant : variants)
        for (unsigned i = 0; i < 10; ++i)
        {
            std::string path = base + variant + "discord-ipc-" + std::to_string(i);
            struct sockaddr_un address = {};
            if (path.size() >= sizeof(address.sun_path)) continue;
            struct stat status;
            if (lstat(path.c_str(), &status) || !S_ISSOCK(status.st_mode) || status.st_uid != geteuid()) continue;
            fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) continue;
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
            { close(); continue; }
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
            if (!connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return true;
            if (errno == EINPROGRESS)
            {
                pollfd poller = {fd, POLLOUT, 0};
                int error = 0;
                socklen_t length = sizeof(error);
                if (::poll(&poller, 1, 0) > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) && !error)
                    return true;
            }
            close();
        }
#endif
        return false;
    }

    void close()
    {
#ifdef _WIN32
        if (pipe != INVALID_HANDLE_VALUE)
        {
            CancelIo(pipe);
            DWORD ignored;
            if (readPending) GetOverlappedResult(pipe, &readOperation, &ignored, TRUE);
            if (writePending) GetOverlappedResult(pipe, &writeOperation, &ignored, TRUE);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (readOperation.hEvent) CloseHandle(readOperation.hEvent);
        if (writeOperation.hEvent) CloseHandle(writeOperation.hEvent);
        readOperation = OVERLAPPED();
        writeOperation = OVERLAPPED();
        readPending = writePending = false;
#else
        if (fd >= 0) ::close(fd);
        fd = -1;
#endif
        output.clear();
        outputOffset = 0;
    }

    bool queue(uint32_t opcode, std::string const& payload)
    {
        if (payload.size() > MaxMessage || output.size() >= 32) return false;
        std::vector<unsigned char> frame;
        append32(frame, opcode);
        append32(frame, uint32_t(payload.size()));
        frame.insert(frame.end(), payload.begin(), payload.end());
        output.push_back(std::move(frame));
        return true;
    }

    bool drained() const { return output.empty(); }

    bool pump(std::vector<unsigned char>& input)
    {
        // Limit work per iteration so a busy or malicious local peer cannot
        // prevent observing stop requests.
        for (unsigned iteration = 0; iteration < 4; ++iteration)
        {
#ifdef _WIN32
            DWORD count = 0;
            if (readPending)
            {
                if (!GetOverlappedResult(pipe, &readOperation, &count, FALSE))
                {
                    if (GetLastError() == ERROR_IO_INCOMPLETE) break;
                    return false;
                }
                readPending = false;
            }
            else
            {
                ResetEvent(readOperation.hEvent);
                if (!ReadFile(pipe, readBuffer, sizeof(readBuffer), &count, &readOperation))
                {
                    if (GetLastError() == ERROR_IO_PENDING) { readPending = true; break; }
                    return false;
                }
            }
            if (!count) return false;
#else
            unsigned char readBuffer[4096];
            ssize_t count = recv(fd, readBuffer, sizeof(readBuffer), 0);
            if (!count) return false;
            if (count < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                return false;
            }
#endif
            input.insert(input.end(), readBuffer, readBuffer + count);
            if (input.size() > MaxMessage + 8) break;
        }
        for (unsigned iteration = 0; iteration < 4 && !output.empty(); ++iteration)
        {
            std::vector<unsigned char> const& frame = output.front();
#ifdef _WIN32
            DWORD count = 0;
            if (writePending)
            {
                if (!GetOverlappedResult(pipe, &writeOperation, &count, FALSE))
                {
                    if (GetLastError() == ERROR_IO_INCOMPLETE) break;
                    return false;
                }
                writePending = false;
            }
            else
            {
                ResetEvent(writeOperation.hEvent);
                if (!WriteFile(pipe, frame.data() + outputOffset, DWORD(frame.size() - outputOffset), &count, &writeOperation))
                {
                    if (GetLastError() == ERROR_IO_PENDING) { writePending = true; break; }
                    return false;
                }
            }
            if (!count) return false;
#else
#ifdef MSG_NOSIGNAL
            const int flags = MSG_NOSIGNAL;
#else
            const int flags = 0;
#endif
            ssize_t count = send(fd, frame.data() + outputOffset, frame.size() - outputOffset, flags);
            if (count <= 0)
            {
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
                return false;
            }
#endif
            outputOffset += size_t(count);
            if (outputOffset == frame.size()) { output.pop_front(); outputOffset = 0; }
        }
        return true;
    }

private:
    std::deque<std::vector<unsigned char>> output;
    size_t outputOffset = 0;
#ifdef _WIN32
    HANDLE pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED readOperation = {}, writeOperation = {};
    bool readPending = false, writePending = false;
    unsigned char readBuffer[4096];
#else
    int fd = -1;
#endif
};
}

bool validApplicationId(std::string const& value)
{
    if (value.empty() || value.size() > 20 || value[0] == '0') return false;
    uint64_t number = 0;
    for (char ch : value)
    {
        if (ch < '0' || ch > '9') return false;
        unsigned digit = unsigned(ch - '0');
        if (number > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        number = number * 10 + digit;
    }
    return number != 0;
}

struct Client::Impl
{
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping = false;
    Activity desired;
    uint64_t revision = 0;
    std::deque<Event> events;
    std::string lastError;
    Clock::time_point lastErrorAt = Clock::time_point::min();

    void event(Event::Kind kind, std::string const& text = "")
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (kind == Event::Error)
        {
            if (lastError == text) return;
            if (!lastError.empty() && Clock::now() - lastErrorAt < std::chrono::seconds(5)) return;
            lastError = text;
            lastErrorAt = Clock::now();
        }
        if (kind == Event::Ready) lastError.clear();
        if (events.size() >= MaxEvents) events.pop_front();
        Event next;
        next.kind = kind;
        next.text = text;
        events.push_back(std::move(next));
    }

    bool stopRequested()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return stopping;
    }

    void wait(unsigned milliseconds)
    {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(milliseconds));
    }

    bool dispatch(std::string const& payload, bool& ready, Transport& transport, uint64_t& nonce)
    {
        if (!boundedJson(payload)) return false;
        Json json;
        sjson_node* root = sjson_decode(json.context, payload.c_str());
        if (!root || root->tag != SJSON_OBJECT) return false;
        char const* command = stringMember(root, "cmd");
        char const* type = stringMember(root, "evt");
        if (!std::strcmp(type, "ERROR"))
        {
            sjson_node* data = member(root, "data", SJSON_OBJECT);
            std::string reason = displayText(stringMember(data, "message"), 256);
            event(Event::Error, reason.empty() ? "Discord rejected an RPC request." : reason);
            return true;
        }
        if (std::strcmp(command, "DISPATCH")) return true;
        if (!std::strcmp(type, "READY"))
        {
            if (!ready)
            {
                ready = true;
                event(Event::Ready);
                return transport.queue(1, subscription(++nonce));
            }
        }
        else if (ready && !std::strcmp(type, "ACTIVITY_JOIN"))
        {
            sjson_node* data = member(root, "data", SJSON_OBJECT);
            std::string secret = stringMember(data, "secret");
            if (secretText(secret)) event(Event::Join, secret);
        }
        return true;
    }

    void run(std::string applicationId)
    {
        uint64_t nonce = 0;
        while (!stopRequested())
        {
            Transport transport;
            if (!transport.open())
            {
                // Game activity updates do not cause repeated connection
                // attempts while Discord is absent.
                Clock::time_point retry = Clock::now() + std::chrono::seconds(5);
                while (!stopRequested() && Clock::now() < retry) wait(100);
                continue;
            }
            bool ready = false;
            bool alive = transport.queue(0, handshake(applicationId));
            std::vector<unsigned char> input;
            uint64_t sentRevision = std::numeric_limits<uint64_t>::max();
            std::string sentJoinSecret;
            Clock::time_point opened = Clock::now();
            Clock::time_point lastActivity = Clock::time_point::min();
            while (alive && !stopRequested())
            {
                alive = transport.pump(input);
                size_t used = 0;
                while (alive && input.size() - used >= 8)
                {
                    uint32_t opcode = read32(input.data() + used);
                    uint32_t length = read32(input.data() + used + 4);
                    if (length > MaxMessage) { alive = false; break; }
                    if (input.size() - used - 8 < length) break;
                    std::string payload(reinterpret_cast<char const*>(input.data() + used + 8), length);
                    used += 8 + length;
                    if (opcode == 1) alive = dispatch(payload, ready, transport, nonce);
                    else if (opcode == 3) alive = transport.queue(4, payload);
                    else if (opcode == 2)
                    {
                        if (boundedJson(payload))
                        {
                            Json json;
                            sjson_node* root = sjson_decode(json.context, payload.c_str());
                            std::string reason = displayText(stringMember(root, "message"), 256);
                            if (!reason.empty()) event(Event::Error, reason);
                        }
                        alive = false;
                    }
                    else if (opcode != 4) alive = false;
                }
                if (used) input.erase(input.begin(), input.begin() + used);
                if (input.size() > MaxMessage + 8) alive = false;
                if (alive && ready)
                {
                    Activity activity;
                    uint64_t nextRevision;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        activity = desired;
                        nextRevision = revision;
                    }
                    bool first = sentRevision == std::numeric_limits<uint64_t>::max();
                    bool removeJoin = !sentJoinSecret.empty() && activity.joinSecret.empty();
                    if (nextRevision != sentRevision &&
                        (first || emptyActivity(activity) || removeJoin ||
                         Clock::now() - lastActivity >= std::chrono::seconds(15)))
                    {
                        alive = transport.queue(1, activityMessage(activity, ++nonce));
                        sentRevision = nextRevision;
                        sentJoinSecret = activity.joinSecret;
                        lastActivity = Clock::now();
                    }
                }
                if (!ready && Clock::now() - opened > std::chrono::seconds(10))
                {
                    event(Event::Error, "Discord did not complete the RPC handshake.");
                    alive = false;
                }
                if (alive) wait(20);
            }
            if (ready && stopRequested())
            {
                // Clearing is best effort; an unresponsive Discord must not
                // stall game shutdown. Closing the connection also clears it.
                transport.queue(1, activityMessage(Activity(), ++nonce));
                Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(200);
                input.clear();
                while (!transport.drained() && Clock::now() < deadline)
                {
                    if (!transport.pump(input)) break;
                    input.clear();
                    if (!transport.drained()) wait(10);
                }
            }
            transport.close();
            if (ready) event(Event::Disconnected);
            if (!stopRequested())
            {
                Clock::time_point retry = Clock::now() + std::chrono::seconds(5);
                while (!stopRequested() && Clock::now() < retry) wait(100);
            }
        }
    }
};

Client::Client() : impl(new Impl) {}
Client::~Client() { stop(); }

void Client::start(std::string const& applicationId)
{
    stop();
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->stopping = false;
        impl->events.clear();
        impl->lastError.clear();
    }
    if (!validApplicationId(applicationId))
    {
        impl->event(Event::Error, "Discord Application ID must be a nonzero decimal 64-bit number.");
        return;
    }
    impl->worker = std::thread(&Impl::run, impl.get(), applicationId);
}

void Client::stop()
{
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->stopping = true;
    }
    impl->wake.notify_all();
    if (impl->worker.joinable()) impl->worker.join();
}

void Client::setActivity(Activity const& activity)
{
    Activity cleaned = cleanActivity(activity);
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (sameActivity(impl->desired, cleaned)) return;
        impl->desired = std::move(cleaned);
        ++impl->revision;
    }
    impl->wake.notify_all();
}

bool Client::poll(Event& event)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->events.empty()) return false;
    event = std::move(impl->events.front());
    impl->events.pop_front();
    return true;
}
}
