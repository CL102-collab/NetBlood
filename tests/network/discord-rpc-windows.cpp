// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone Windows named-pipe integration test. Run in an isolated Windows
// user session or Wine prefix, without a real Discord client in that session.
// Compile with discord_rpc.cpp, the Blood/Build include directories, and the
// compiler's thread library. No game assets or Discord SDK are required.
#include "discord_rpc.h"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

// Supply the engine's existing JSON implementation without its game allocator.
#define SJSON_IMPLEMENT
#define ABORT_IF_F(condition, ...) do { if (condition) std::abort(); } while (0)
#include "sjson.h"

namespace
{
unsigned checks = 0;
char const* const application = "111111111111111111";
char const* const endpoint = "[0201:f044:ca34:9cc9:2946:ad06:6f2d:5c4e]:23513";

void require(bool success, char const* message)
{
    ++checks;
    if (!success) throw std::runtime_error(message);
}

struct Json
{
    sjson_context* context = sjson_create_context(64, 1024, nullptr);
    sjson_node* root;
    explicit Json(std::string const& text) : root(sjson_decode(context, text.c_str()))
    {
        require(root && root->tag == SJSON_OBJECT, "Malformed JSON from IPC client");
    }
    ~Json() { sjson_destroy_context(context); }
};

std::string stringMember(sjson_node* node, char const* key)
{
    sjson_node* member = sjson_find_member(node, key);
    require(member && member->tag == SJSON_STRING, "Missing JSON string member");
    return member->string_;
}

sjson_node* objectMember(sjson_node* node, char const* key)
{
    sjson_node* member = sjson_find_member(node, key);
    require(member && member->tag == SJSON_OBJECT, "Missing JSON object member");
    return member;
}

std::vector<unsigned char> frame(unsigned opcode, std::string const& payload)
{
    std::vector<unsigned char> wire(8 + payload.size());
    for (unsigned i = 0; i < 4; ++i)
    {
        wire[i] = static_cast<unsigned char>(opcode >> (i * 8));
        wire[i + 4] = static_cast<unsigned char>(payload.size() >> (i * 8));
    }
    std::copy(payload.begin(), payload.end(), wire.begin() + 8);
    return wire;
}

struct Pipe
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    Pipe()
    {
        // A high index verifies that discovery does not only try pipe zero.
        handle = CreateNamedPipeW(L"\\\\.\\pipe\\discord-ipc-7",
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
            1, 65536, 65536, 0, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "Cannot create isolated mock Discord pipe");
    }
    ~Pipe() { close(); }
    void close()
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
    void accept()
    {
        ULONGLONG const deadline = GetTickCount64() + 15000;
        while (!ConnectNamedPipe(handle, nullptr))
        {
            DWORD const error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) return;
            require(error == ERROR_PIPE_LISTENING || error == ERROR_NO_DATA,
                    "Mock named pipe failed to accept connection");
            require(GetTickCount64() < deadline, "Discord client did not connect to pipe 7");
            Sleep(5);
        }
    }
    void write(std::vector<unsigned char> const& wire, bool fragmented = false)
    {
        size_t offset = 0;
        ULONGLONG const deadline = GetTickCount64() + 5000;
        while (offset != wire.size())
        {
            DWORD written = 0;
            DWORD const count = static_cast<DWORD>(fragmented ? 1 : wire.size() - offset);
            require(WriteFile(handle, wire.data() + offset, count, &written, nullptr) != 0,
                    "Mock named pipe write failed");
            require(written <= count && GetTickCount64() < deadline, "Mock pipe write stalled");
            offset += written;
            if (fragmented || !written) Sleep(1);
        }
    }
    std::vector<unsigned char> read(size_t length)
    {
        std::vector<unsigned char> result(length);
        size_t offset = 0;
        ULONGLONG const deadline = GetTickCount64() + 10000;
        while (offset < length)
        {
            DWORD available = 0;
            require(PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) != 0,
                    "Discord client unexpectedly disconnected");
            if (available)
            {
                DWORD received = 0;
                DWORD const count = static_cast<DWORD>(std::min<size_t>(available, length - offset));
                require(ReadFile(handle, result.data() + offset, count, &received, nullptr) != 0,
                        "Mock named pipe read failed");
                require(received > 0 && received <= count, "Invalid named pipe read size");
                offset += received;
            }
            else
            {
                require(GetTickCount64() < deadline, "Timed out reading Discord IPC frame");
                Sleep(5);
            }
        }
        return result;
    }
    std::string readFrame(unsigned& opcode)
    {
        auto header = read(8);
        opcode = 0;
        unsigned length = 0;
        for (unsigned i = 0; i < 4; ++i)
        {
            opcode |= static_cast<unsigned>(header[i]) << (i * 8);
            length |= static_cast<unsigned>(header[i + 4]) << (i * 8);
        }
        require(length < 65536, "Client emitted oversized IPC frame");
        auto payload = read(length);
        return std::string(payload.begin(), payload.end());
    }
    void handshake()
    {
        accept();
        unsigned opcode = 99;
        Json hello(readFrame(opcode));
        require(opcode == 0, "Expected Discord handshake opcode");
        require(stringMember(hello.root, "client_id") == application, "Wrong Discord application ID");
        require(sjson_get_int(hello.root, "v", -1) == 1, "Wrong Discord IPC version");
        write(frame(1, "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\",\"data\":{\"v\":1,"
                       "\"user\":{\"id\":\"222222222222222222\",\"username\":\"Mock\"}}}"), true);
    }
};

discordrpc::Event waitEvent(discordrpc::Client& client, discordrpc::Event::Kind kind)
{
    ULONGLONG const deadline = GetTickCount64() + 10000;
    for (;;)
    {
        discordrpc::Event event;
        while (client.poll(event))
            if (event.kind == kind) return event;
        require(GetTickCount64() < deadline, "Timed out waiting for Discord client event");
        Sleep(5);
    }
}

void activity(Pipe& pipe, discordrpc::Activity const& expected)
{
    bool subscribed = false, present = false;
    for (unsigned attempt = 0; attempt < 8 && (!subscribed || !present); ++attempt)
    {
        unsigned opcode = 99;
        Json message(pipe.readFrame(opcode));
        require(opcode == 1, "Expected command frame opcode");
        std::string const command = stringMember(message.root, "cmd");
        require(!stringMember(message.root, "nonce").empty(), "Missing RPC nonce");
        if (command == "SUBSCRIBE")
            subscribed = stringMember(message.root, "evt") == "ACTIVITY_JOIN" || subscribed;
        else if (command == "SET_ACTIVITY")
        {
            auto args = objectMember(message.root, "args");
            require(sjson_get_int(args, "pid", 0) == static_cast<int>(GetCurrentProcessId()),
                    "Presence contains wrong process ID");
            auto value = objectMember(args, "activity");
            require(stringMember(value, "details") == expected.details, "Map/mode text or JSON escaping differs");
            require(stringMember(value, "state") == expected.state, "Long IPv6 server address differs");
            require(sjson_get_double(objectMember(value, "timestamps"), "start", 0)
                        == static_cast<double>(expected.startedAt), "Elapsed-time origin changed");
            auto party = objectMember(value, "party");
            require(stringMember(party, "id") == expected.partyId, "Party ID differs");
            require(sjson_get_int(party, "privacy", 0) == 1, "Party is not directly joinable");
            auto size = sjson_find_member(party, "size");
            require(size && size->tag == SJSON_ARRAY, "Party size missing");
            auto current = sjson_find_element(size, 0), maximum = sjson_find_element(size, 1);
            require(current && maximum && current->tag == SJSON_NUMBER && maximum->tag == SJSON_NUMBER
                        && current->number_ == expected.partySize && maximum->number_ == expected.partyMax,
                    "Party occupancy/capacity differs");
            require(stringMember(objectMember(value, "secrets"), "join") == expected.joinSecret,
                    "IPv6 join secret was truncated or changed");
            present = true;
        }
        else require(false, "Unexpected Discord RPC command");
    }
    require(subscribed && present, "Missing join subscription or rich presence");
}
}

int main()
{
    try
    {
        Pipe first;
        discordrpc::Client client;
        discordrpc::Activity expected;
        expected.details = "Bloodbath | \"Cradle\"";
        expected.state = endpoint;
        expected.joinSecret = std::string("netblood://") + endpoint;
        expected.partyId = "netblood-test-party";
        expected.startedAt = 1800000000;
        expected.partySize = 2;
        expected.partyMax = 8;
        client.start(application);
        client.setActivity(expected);
        first.handshake();
        waitEvent(client, discordrpc::Event::Ready);
        activity(first, expected);

        // One read may contain several messages; each frame may also span reads.
        auto batch = frame(3, "netblood-ping");
        auto join = frame(1, std::string("{\"cmd\":\"DISPATCH\",\"evt\":\"ACTIVITY_JOIN\","
                          "\"data\":{\"secret\":\"") + expected.joinSecret + "\"}}");
        batch.insert(batch.end(), join.begin(), join.end());
        first.write(batch, true);
        require(waitEvent(client, discordrpc::Event::Join).text == expected.joinSecret,
                "Fragmented IPv6 join event differs");
        unsigned opcode = 99;
        require(first.readFrame(opcode) == "netblood-ping" && opcode == 4,
                "Ping was not answered with matching Pong");
        first.write(batch);
        require(waitEvent(client, discordrpc::Event::Join).text == expected.joinSecret,
                "Coalesced IPv6 join event differs");
        require(first.readFrame(opcode) == "netblood-ping" && opcode == 4,
                "Coalesced Ping was not answered with matching Pong");

        // A framing length is untrusted, even though the peer is local.
        first.write({1, 0, 0, 0, 255, 255, 255, 127});
        waitEvent(client, discordrpc::Event::Disconnected);
        first.close();
        expected.details = "Cooperative | E1M2";
        expected.partySize = 3;
        client.setActivity(expected);
        Pipe second;
        second.handshake();
        waitEvent(client, discordrpc::Event::Ready);
        activity(second, expected);

        ULONGLONG const beforeStop = GetTickCount64();
        client.stop();
        require(GetTickCount64() - beforeStop < 1500, "Discord shutdown blocked on named pipe");
        Json cleared(second.readFrame(opcode));
        require(opcode == 1 && stringMember(cleared.root, "cmd") == "SET_ACTIVITY",
                "Shutdown did not clear Rich Presence");
        auto empty = sjson_find_member(objectMember(cleared.root, "args"), "activity");
        require(empty && empty->tag == SJSON_NULL, "Shutdown left a joinable activity behind");
        std::printf("PASS Windows Discord named-pipe RPC (%u checks): discovery, handshake, "
                    "presence, IPv6 joins, framing, Ping/Pong, reconnect, bounded shutdown\n", checks);
        return 0;
    }
    catch (std::exception const& error)
    {
        std::fprintf(stderr, "FAIL Windows Discord named-pipe RPC: %s (Win32 error %lu)\n",
                     error.what(), GetLastError());
        return 1;
    }
}
