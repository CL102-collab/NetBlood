// SPDX-License-Identifier: GPL-2.0-or-later
// Black-box checks of the real worker, framing, JSON and Unix IPC transport.
#include "discord_rpc.h"
#include "sjson.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " #x); } while (0)
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<unsigned char>;
char const* const ApplicationId = "123456789012345678";
char const* const JoinUri = "netblood://[201:f044:ca34:9cc9:2946:ad06:6f2d:5c4e]:23513";
char const* const Ready = "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\",\"data\":{\"v\":1,\"user\":{\"id\":\"1\",\"username\":\"Mock\"}}}";

void pause(unsigned milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

int remaining(Clock::time_point deadline)
{
    auto value = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return value > 0 ? static_cast<int>(value) : 0;
}

bool readable(int fd, unsigned milliseconds)
{
    pollfd item = {fd, POLLIN, 0};
    int result;
    do result = poll(&item, 1, static_cast<int>(milliseconds)); while (result < 0 && errno == EINTR);
    CHECK(result >= 0);
    return result > 0;
}

void append32(Bytes& output, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) output.push_back(static_cast<unsigned char>(value >> (i * 8)));
}

uint32_t read32(unsigned char const* value)
{
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result |= uint32_t(value[i]) << (i * 8);
    return result;
}

Bytes frame(uint32_t opcode, std::string const& payload)
{
    Bytes output;
    append32(output, opcode);
    append32(output, static_cast<uint32_t>(payload.size()));
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

struct Frame
{
    uint32_t opcode;
    std::string payload;
};

class MockDiscord
{
public:
    MockDiscord()
    {
        char path[] = "/tmp/nb-discord-test.XXXXXX";
        char* created = mkdtemp(path);
        CHECK(created != nullptr);
        directory = created;
        socketPath = directory + "/discord-ipc-0";
        CHECK(setenv("XDG_RUNTIME_DIR", directory.c_str(), 1) == 0);
        CHECK(setenv("TMPDIR", directory.c_str(), 1) == 0);
        CHECK(setenv("TMP", directory.c_str(), 1) == 0);
        CHECK(setenv("TEMP", directory.c_str(), 1) == 0);
        listener = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(listener >= 0);
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        CHECK(socketPath.size() < sizeof(address.sun_path));
        std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
        CHECK(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(listen(listener, 4) == 0);
    }

    ~MockDiscord()
    {
        disconnect();
        if (listener >= 0) close(listener);
        unlink(socketPath.c_str());
        rmdir(directory.c_str());
    }

    void connect(unsigned timeout = 7000)
    {
        CHECK(peer < 0);
        CHECK(readable(listener, timeout));
        peer = accept(listener, nullptr, nullptr);
        CHECK(peer >= 0);
    }

    void disconnect()
    {
        if (peer >= 0) close(peer);
        peer = -1;
    }

    void send(Bytes const& bytes)
    {
        size_t offset = 0;
        while (offset < bytes.size())
        {
            ssize_t size = ::send(peer, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            if (size < 0 && errno == EINTR) continue;
            CHECK(size > 0);
            offset += static_cast<size_t>(size);
        }
    }

    void send(uint32_t opcode, std::string const& payload) { send(frame(opcode, payload)); }

    void sendFragmented(uint32_t opcode, std::string const& payload)
    {
        Bytes const bytes = frame(opcode, payload);
        size_t offset = 0;
        for (size_t count : {size_t(1), size_t(2), size_t(4), size_t(3)})
        {
            send(Bytes(bytes.begin() + offset, bytes.begin() + offset + count));
            offset += count;
            pause(15);
        }
        send(Bytes(bytes.begin() + offset, bytes.end()));
    }

    bool available(unsigned timeout = 150) const { return readable(peer, timeout); }

    Frame receive(unsigned timeout = 3000)
    {
        Clock::time_point const deadline = Clock::now() + std::chrono::milliseconds(timeout);
        unsigned char header[8];
        readExact(header, sizeof(header), deadline);
        Frame result;
        result.opcode = read32(header);
        size_t const length = read32(header + 4);
        CHECK(length <= 128 * 1024);
        result.payload.resize(length);
        if (length) readExact(&result.payload[0], length, deadline);
        return result;
    }

    void expectClosed(unsigned timeout = 3000)
    {
        CHECK(readable(peer, timeout));
        char byte;
        CHECK(recv(peer, &byte, 1, 0) == 0);
    }

private:
    void readExact(void* target, size_t length, Clock::time_point deadline)
    {
        size_t offset = 0;
        while (offset < length)
        {
            CHECK(readable(peer, static_cast<unsigned>(remaining(deadline))));
            ssize_t size = recv(peer, static_cast<char*>(target) + offset, length - offset, 0);
            if (size < 0 && errno == EINTR) continue;
            CHECK(size > 0);
            offset += static_cast<size_t>(size);
        }
    }

    int listener = -1;
    int peer = -1;
    std::string directory;
    std::string socketPath;
};

class Json
{
public:
    explicit Json(std::string const& text)
    {
        context = sjson_create_context(64, 4096, nullptr);
        CHECK(context != nullptr);
        root = sjson_decode(context, text.c_str());
        CHECK(root && root->tag == SJSON_OBJECT);
    }
    ~Json() { sjson_destroy_context(context); }
    sjson_node* root;
private:
    sjson_context* context;
};

sjson_node* member(sjson_node* object, char const* key, sjson_tag type)
{
    CHECK(object && object->tag == SJSON_OBJECT);
    sjson_node* node = sjson_find_member(object, key);
    CHECK(node && node->tag == type);
    return node;
}

std::string stringMember(sjson_node* object, char const* key)
{
    return member(object, key, SJSON_STRING)->string_;
}

void handshake(MockDiscord& mock)
{
    mock.connect();
    Frame const received = mock.receive();
    CHECK(received.opcode == 0);
    Json json(received.payload);
    CHECK(member(json.root, "v", SJSON_NUMBER)->number_ == 1);
    CHECK(stringMember(json.root, "client_id") == ApplicationId);
    CHECK(!mock.available(120)); // Presence and subscriptions must wait for READY.
}

discordrpc::Event event(discordrpc::Client& client, discordrpc::Event::Kind kind, unsigned timeout = 3000)
{
    Clock::time_point const deadline = Clock::now() + std::chrono::milliseconds(timeout);
    discordrpc::Event found;
    while (Clock::now() < deadline)
    {
        if (client.poll(found))
        {
            CHECK(found.kind == kind);
            return found;
        }
        pause(10);
    }
    CHECK(false && "Discord event timed out");
    return found;
}

void noJoin(discordrpc::Client& client, unsigned timeout = 180)
{
    Clock::time_point const deadline = Clock::now() + std::chrono::milliseconds(timeout);
    do
    {
        discordrpc::Event found;
        while (client.poll(found)) CHECK(found.kind != discordrpc::Event::Join);
        pause(10);
    } while (Clock::now() < deadline);
}

void checkActivity(Frame const& received, discordrpc::Activity const& expected)
{
    CHECK(received.opcode == 1);
    Json json(received.payload);
    CHECK(stringMember(json.root, "cmd") == "SET_ACTIVITY");
    CHECK(!stringMember(json.root, "nonce").empty());
    sjson_node* args = member(json.root, "args", SJSON_OBJECT);
    CHECK(member(args, "pid", SJSON_NUMBER)->number_ > 0);
    sjson_node* activity = member(args, "activity", SJSON_OBJECT);
    CHECK(stringMember(activity, "details") == expected.details);
    CHECK(stringMember(activity, "state") == expected.state);
    sjson_node* timestamps = member(activity, "timestamps", SJSON_OBJECT);
    CHECK(member(timestamps, "start", SJSON_NUMBER)->number_ == expected.startedAt);
    if (!expected.partyId.empty())
    {
        sjson_node* party = member(activity, "party", SJSON_OBJECT);
        CHECK(stringMember(party, "id") == expected.partyId);
        CHECK(member(party, "privacy", SJSON_NUMBER)->number_ == 1);
        sjson_node* size = member(party, "size", SJSON_ARRAY);
        CHECK(sjson_child_count(size) == 2);
        CHECK(sjson_find_element(size, 0)->tag == SJSON_NUMBER);
        CHECK(sjson_find_element(size, 0)->number_ == expected.partySize);
        CHECK(sjson_find_element(size, 1)->tag == SJSON_NUMBER);
        CHECK(sjson_find_element(size, 1)->number_ == expected.partyMax);
    }
    else
    {
        CHECK(sjson_find_member(activity, "party") == nullptr);
    }
    if (!expected.joinSecret.empty())
        CHECK(stringMember(member(activity, "secrets", SJSON_OBJECT), "join") == expected.joinSecret);
    else
        CHECK(sjson_find_member(activity, "secrets") == nullptr);
}

void ready(MockDiscord& mock, discordrpc::Client& client, discordrpc::Activity const& activity)
{
    mock.sendFragmented(1, Ready);
    event(client, discordrpc::Event::Ready);
    bool subscribed = false, published = false;
    for (unsigned i = 0; i < 5 && (!subscribed || !published); ++i)
    {
        Frame const received = mock.receive();
        CHECK(received.opcode == 1);
        Json json(received.payload);
        std::string const cmd = stringMember(json.root, "cmd");
        if (cmd == "SUBSCRIBE")
        {
            CHECK(stringMember(json.root, "evt") == "ACTIVITY_JOIN");
            CHECK(!stringMember(json.root, "nonce").empty());
            subscribed = true;
        }
        else
        {
            checkActivity(received, activity);
            published = true;
        }
    }
    CHECK(subscribed && published);
}

discordrpc::Activity activity()
{
    discordrpc::Activity value;
    value.details = "E1M1: Cradle \"to\" Grave \\ UTF-8: caf\xc3\xa9";
    value.state = "Co-op | server.example:23513";
    value.joinSecret = JoinUri;
    value.partyId = "server:[201:f044:ca34:9cc9:2946:ad06:6f2d:5c4e]:23513";
    value.startedAt = 1700000000;
    value.partySize = 2;
    value.partyMax = 8;
    return value;
}

void transport()
{
    MockDiscord mock;
    discordrpc::Client client;
    discordrpc::Activity current = activity();
    client.start(ApplicationId);
    client.setActivity(current);
    handshake(mock);
    mock.send(1, std::string("{\"cmd\":\"DISPATCH\",\"evt\":\"ACTIVITY_JOIN\",\"data\":{\"secret\":\"") + JoinUri + "\"}}");
    noJoin(client);
    ready(mock, client, current);

    std::string const payload = "ping with \"quotes\" and backslash \\";
    mock.sendFragmented(3, payload);
    Frame const pong = mock.receive();
    CHECK(pong.opcode == 4 && pong.payload == payload);

    // A response with an event-like field is not a dispatch. Wrong secret types
    // must also never reach the game's join queue.
    std::string const data = std::string("\"data\":{\"secret\":\"") + JoinUri + "\"}}";
    Bytes batch = frame(1, "{\"cmd\":\"SET_ACTIVITY\",\"evt\":\"ACTIVITY_JOIN\"," + data);
    for (std::string const& invalid : {
             "{\"cmd\":22,\"evt\":\"ACTIVITY_JOIN\"," + data,
             std::string("{\"cmd\":\"DISPATCH\",\"evt\":22," + data),
             std::string("{\"cmd\":\"DISPATCH\",\"evt\":\"ACTIVITY_JOIN\",\"data\":[]}"),
             std::string("{\"cmd\":\"DISPATCH\",\"evt\":\"ACTIVITY_JOIN\",\"data\":{\"secret\":\"") +
                 std::string(129, 'a') + "\"}}"})
    {
        Bytes const next = frame(1, invalid);
        batch.insert(batch.end(), next.begin(), next.end());
    }
    for (char const* invalid : {"123", "null", "true", "{}", "[]"})
    {
        Bytes const next = frame(1, std::string("{\"cmd\":\"DISPATCH\",\"evt\":\"ACTIVITY_JOIN\",\"data\":{\"secret\":") + invalid + "}}");
        batch.insert(batch.end(), next.begin(), next.end());
    }
    mock.send(batch);
    noJoin(client);
    mock.sendFragmented(1, "{\"cmd\":\"DISPATCH\",\"evt\":\"ACTIVITY_JOIN\"," + data);
    CHECK(event(client, discordrpc::Event::Join).text == JoinUri);
    batch = frame(3, "coalesced ping");
    Bytes const join = frame(1, "{\"cmd\":\"DISPATCH\",\"evt\":\"ACTIVITY_JOIN\"," + data);
    batch.insert(batch.end(), join.begin(), join.end());
    mock.send(batch);
    CHECK(event(client, discordrpc::Event::Join).text == JoinUri);
    Frame const coalescedPong = mock.receive();
    CHECK(coalescedPong.opcode == 4 && coalescedPong.payload == "coalesced ping");

    // Changing maps preserves the session epoch and updates are coalesced.
    current.details = "E1M2: Wrong intermediate map";
    client.setActivity(current);
    current.details = "E1M3: Phantom Express";
    current.partySize = 3;
    client.setActivity(current);
    CHECK(!mock.available(200));
    checkActivity(mock.receive(18000), current);

    // Repeating an identical activity must not produce a periodic flood.
    client.setActivity(current);
    CHECK(!mock.available(200));

    // A full server stops advertising its join secret immediately, while the
    // visible party counts remain useful and accurate.
    current.partySize = current.partyMax;
    current.joinSecret.clear();
    client.setActivity(current);
    checkActivity(mock.receive(1000), current);

    mock.disconnect();
    event(client, discordrpc::Event::Disconnected);
    current.details = "At the main menu";
    current.state = "Single player";
    current.joinSecret.clear();
    current.partyId.clear();
    current.partySize = current.partyMax = 0;
    client.setActivity(current);
    handshake(mock);
    ready(mock, client, current); // Reconnect sends the newest state immediately.
    // Disabling presence must clear the activity promptly, not after the next
    // regular update interval.
    client.setActivity(discordrpc::Activity());
    Frame const cleared = mock.receive(1000);
    CHECK(cleared.opcode == 1);
    Json clearJson(cleared.payload);
    CHECK(stringMember(clearJson.root, "cmd") == "SET_ACTIVITY");
    member(member(clearJson.root, "args", SJSON_OBJECT), "activity", SJSON_NULL);
    Clock::time_point const before = Clock::now();
    client.stop();
    CHECK(Clock::now() - before < std::chrono::seconds(1));
    std::puts("Discord RPC: handshake, presence, JSON escaping, events, IPv6 joins and reconnect passed");
}

void malformed()
{
    // The malformed frame must disconnect without entering the event queue.
    std::vector<Bytes> invalid;
    Bytes huge;
    append32(huge, 1);
    append32(huge, UINT32_MAX);
    invalid.push_back(huge);
    invalid.push_back(frame(1, std::string(1000, '[') + "0" + std::string(1000, ']')));
    invalid.push_back(frame(1, "{broken json"));
    invalid.push_back(frame(1, std::string("{\"cmd\":\"DISPATCH\"}") + '\0' + "{}"));
    invalid.push_back(frame(99, "{}"));
    for (Bytes const& bytes : invalid)
    {
        MockDiscord mock;
        discordrpc::Client client;
        client.start(ApplicationId);
        client.setActivity(activity());
        handshake(mock);
        ready(mock, client, activity());
        mock.send(bytes);
        mock.expectClosed();
        noJoin(client);
        Clock::time_point const before = Clock::now();
        client.stop();
        CHECK(Clock::now() - before < std::chrono::seconds(1));
    }
    std::puts("Discord RPC: oversized, nested, malformed, NUL and unknown-opcode frames rejected");
}

void absentServer()
{
    // The previous mock's private directory has been removed. There is no IPC
    // server here; the worker must not block game startup or shutdown.
    discordrpc::Client client;
    Clock::time_point const beforeStart = Clock::now();
    client.start(ApplicationId);
    client.setActivity(activity());
    CHECK(Clock::now() - beforeStart < std::chrono::seconds(1));
    pause(120);
    Clock::time_point const beforeStop = Clock::now();
    client.stop();
    CHECK(Clock::now() - beforeStop < std::chrono::seconds(1));
    std::puts("Discord RPC: absent Discord remains nonblocking");
}
}

int main()
{
    try
    {
        CHECK(discordrpc::validApplicationId(ApplicationId));
        CHECK(discordrpc::validApplicationId("18446744073709551615"));
        for (char const* invalid : {"", "0", "0123456789012345678", "abc", "123abc", " 123456789012345678", "123456789012345678 ", "-123456789012345678", "18446744073709551616", "123456789012345678901"})
            CHECK(!discordrpc::validApplicationId(invalid));
        transport();
        malformed();
        absentServer();
        std::printf("Discord RPC: %u checks passed\n", checks);
        return 0;
    }
    catch (std::exception const& error)
    {
        std::fprintf(stderr, "Discord RPC FAIL: %s\n", error.what());
        return 1;
    }
}
