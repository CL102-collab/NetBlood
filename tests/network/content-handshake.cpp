// Real ENet peers for content-handshake integration checks against the game.
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define ENET_IMPLEMENTATION
#include "enet.h"
#include "netassets.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<uint8_t>;
constexpr uint32_t ServerInfoMagic = 0x3156424eu; // Stable NBV1, independent of gameplay version.
constexpr uint32_t ServerInfoSchema = 1;
void check(bool condition, const char *message)
{ if (!condition) throw std::runtime_error(message); }
struct Message {
    netwire::Header header;
    Bytes body;
};
struct Endpoint {
    ENetHost *host = nullptr;
    ENetPeer *peer = nullptr;
    uint32_t sequence = 0;
    uint32_t wireVersion = netwire::Version, remoteVersion = 0;
    unsigned advertisements = 0, messagesBeforeAdvertisement = 0;
    uint32_t requiredProtocol = 0;
    std::string serverBuild;
    bool disconnected = false;
    Endpoint(unsigned port, bool server, uint32_t version = netwire::Version) : wireVersion(version) {
        ENetAddress address = {};
        char loopback[] = "127.0.0.1";
        check(enet_address_set_host(&address, loopback) == 0, "loopback address");
        address.port = enet_uint16(port);
        host = enet_host_create(server ? &address : nullptr, 1, 3, 0, 0);
        check(host != nullptr, "create ENet host");
        host->checksum = enet_crc32;
        host->packetCopies = 3;
        host->maximumPacketSize = netwire::MaxPayload + netwire::HeaderSize;
        if (!server) {
            peer = enet_host_connect(host, &address, 3, version);
            check(peer != nullptr, "create peer");
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            bool connected = false;
            while (Clock::now() < deadline && !connected) {
                ENetEvent event;
                if (enet_host_service(host, &event, 20) <= 0) continue;
                check(event.type == ENET_EVENT_TYPE_CONNECT, "unexpected connection event");
                connected = true;
            }
            check(connected, "connection timeout");
        }
    }
    ~Endpoint() {
        if (peer && !disconnected) enet_peer_disconnect_now(peer, 0);
        if (host) enet_host_destroy(host);
    }
    void sendWire(Bytes wire) {
        ENetPacket *packet = enet_packet_create(wire.data(), wire.size(), ENET_PACKET_FLAG_RELIABLE);
        check(packet != nullptr, "allocate packet");
        if (enet_peer_send(peer, 0, packet) < 0) {
            enet_packet_destroy(packet);
            throw std::runtime_error("send packet");
        }
        enet_host_flush(host);
    }
    void send(uint32_t type, const Bytes &body, uint32_t session = 0, uint32_t generation = 0) {
        netwire::Header h;
        h.type = type; h.session = session; h.generation = generation; h.sequence = ++sequence;
        sendWire(netwire::encode(h, body));
    }
    bool poll(Message &message, unsigned milliseconds = 20) {
        ENetEvent event;
        if (enet_host_service(host, &event, milliseconds) <= 0) return false;
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            peer = event.peer;
            remoteVersion = event.data;
            return false;
        }
        if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            disconnected = true;
            return false;
        }
        if (event.type != ENET_EVENT_TYPE_RECEIVE) return false;
        Bytes wire(event.packet->data, event.packet->data + event.packet->dataLength);
        enet_packet_destroy(event.packet);
        netwire::Reader prefix(wire.data(), wire.size());
        if (prefix.u32() == ServerInfoMagic) {
            check(prefix.u32() == ServerInfoSchema, "unsupported server advertisement schema");
            requiredProtocol = prefix.u32();
            serverBuild = prefix.text(96);
            check(prefix.end() && requiredProtocol != 0, "invalid server advertisement");
            for (unsigned char ch : serverBuild)
                check(ch >= 32 && ch <= 126, "server advertisement build is not printable ASCII");
            ++advertisements;
            return false;
        }
        check(wire.size() >= netwire::HeaderSize, "truncated response envelope");
        netwire::Reader versionReader(wire.data(), wire.size());
        check(versionReader.u32() == netwire::Magic, "invalid response magic");
        check(versionReader.u32() == wireVersion, "response does not match the peer's advertised protocol");
        // An old/new-version probe accepts only its version-stamped diagnostic
        // rejection. Normalize that header solely to reuse the strict parser.
        if (wireVersion != netwire::Version) {
            check(versionReader.u32() == netwire::Reject, "foreign-protocol peer received gameplay");
            for (unsigned byte = 0; byte < 4; ++byte)
                wire[4 + byte] = uint8_t(netwire::Version >> (byte * 8));
        }
        netwire::Reader body(nullptr, 0);
        const bool valid = netwire::decode(wire.data(), wire.size(), message.header, body);
        if (valid) message.body.assign(body.data, body.data + body.size);
        check(valid, "invalid response envelope");
        if (!advertisements) ++messagesBeforeAdvertisement;
        return true;
    }
    Message receive(unsigned seconds = 10) {
        const auto deadline = Clock::now() + std::chrono::seconds(seconds);
        Message message;
        while (Clock::now() < deadline) {
            if (poll(message)) return message;
            check(!disconnected, "disconnected before response");
        }
        throw std::runtime_error("response timeout");
    }
    void silence(unsigned milliseconds) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
        Message message;
        while (Clock::now() < deadline) {
            check(!poll(message), "unexpected response before content admission");
            check(!disconnected, "unexpected disconnect before content admission");
        }
    }
};

// Encode independently of netcompat.h so integration tests catch a changed
// advertisement schema instead of silently inheriting both sides of a mistake.
Bytes advertisement(uint32_t required, const std::string &build) {
    netwire::Writer writer;
    writer.u32(ServerInfoMagic); writer.u32(ServerInfoSchema); writer.u32(required);
    writer.text(build, 96);
    return writer.data;
}

void versionProbe(unsigned port, uint32_t version) {
    check(version != 0, "client protocol must be nonzero");
    Endpoint client(port, false, version);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    std::string reason;
    bool rejected = false;
    while (Clock::now() < deadline) {
        Message message;
        if (client.poll(message)) {
            check(client.advertisements == 1 && client.messagesBeforeAdvertisement == 0,
                  "server sent a gameplay envelope before its version advertisement");
            check(version != netwire::Version && message.header.type == netwire::Reject && !rejected,
                  "unexpected gameplay after version advertisement");
            netwire::Reader reader(message.body.data(), message.body.size());
            reason = reader.text(160);
            check(reader.end(), "invalid version rejection reason");
            std::string lower = reason;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
                return char(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
            });
            check(reason.find(std::to_string(netwire::Version)) != std::string::npos &&
                  reason.find(std::to_string(version)) != std::string::npos &&
                  lower.find("update") != std::string::npos,
                  "version rejection omits protocols or update guidance");
            rejected = true;
        }
        if (version == netwire::Version && client.advertisements) break;
        if (client.disconnected) break;
    }
    check(client.advertisements == 1 && client.requiredProtocol == netwire::Version && !client.serverBuild.empty(),
          "server did not advertise its required protocol and build");
    check(version == netwire::Version || (rejected && client.disconnected),
          "incompatible client was not explicitly rejected and disconnected");
    std::printf("advertisement=1 schema=1 required_protocol=%u client_protocol=%u build=\"%s\" gameplay_messages=0\n",
                client.requiredProtocol, version, client.serverBuild.c_str());
    if (rejected)
        std::printf("rejected=1 reject_protocol=%u disconnected=1 reason=\"%s\"\n", version, reason.c_str());
}

void versionServer(unsigned port, const std::string &mode) {
    check(mode == "advert-newer" || mode == "advert-older" || mode == "legacy-reject",
          "unknown version server mode");
    const uint32_t required = mode == "advert-newer" ? netwire::Version + 1 : netwire::Version - 1;
    Endpoint server(port, true);
    std::printf("listening=1 mode=%s required_protocol=%u\n", mode.c_str(), required);
    std::fflush(stdout);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    unsigned hellos = 0;
    bool sent = false;
    while (Clock::now() < deadline && !server.disconnected) {
        Message message;
        const bool received = server.poll(message);
        if (server.peer && !sent) {
            if (mode == "legacy-reject") {
                netwire::Writer reason;
                reason.text("Protocol version mismatch", 160);
                netwire::Header header; header.type = netwire::Reject;
                Bytes wire = netwire::encode(header, reason.data);
                for (unsigned byte = 0; byte < 4; ++byte)
                    wire[4 + byte] = uint8_t(required >> (byte * 8));
                server.sendWire(wire);
            } else {
                server.sendWire(advertisement(required, "compat-fixture-protocol-" + std::to_string(required)));
            }
            sent = true;
        }
        if (received) {
            check(message.header.type == netwire::Hello && ++hellos <= 1,
                  "incompatible client sent content approval or gameplay");
        }
    }
    check(sent && server.disconnected, "client did not disconnect after protocol incompatibility");
    check(server.remoteVersion == netwire::Version, "test client advertised an unexpected protocol");
    std::printf("version_server_disconnected=1 expected_protocol=%u client_protocol=%u hello_sent=%u "
                "content_ready_sent=0 ready_sent=0 commands_sent=0\n",
                required, server.remoteVersion, hellos);
}
Bytes hello(uint32_t fingerprint, bool capable = true, uint8_t autoAim = 1, uint8_t skill = 2) {
    netwire::Writer writer;
    writer.u32(fingerprint); writer.text("content-probe", 15);
    writer.u8(autoAim); writer.u8(1); writer.u8(skill);
    if (capable) writer.u32(netassets::ManifestVersion);
    return writer.data;
}
uint32_t manifest(const Message &message) {
    check(message.header.type == netwire::ContentManifest, "expected asset manifest");
    check(message.header.session != 0, "manifest session missing");
    netwire::Reader reader(message.body.data(), message.body.size());
    const uint32_t fingerprint = reader.u32();
    std::vector<netassets::Asset> assets;
    std::string error;
    check(netassets::decodeManifest(reader, assets, error), "server manifest invalid");
    check(!assets.empty(), "stock server manifest empty");
    return fingerprint;
}
void probe(unsigned port, const std::string &mode, uint32_t fingerprint, const char *trigger) {
    Endpoint client(port, false);
    client.send(netwire::Hello, hello(fingerprint, mode != "legacy", mode == "profile-hold" ? 3 : 1,
                                    mode == "profile-hold" ? 0 : 2));
    const Message offered = client.receive();
    if (mode == "legacy") {
        check(offered.header.type == netwire::Welcome, "legacy CRC client was not admitted");
        std::puts("legacy_admitted=1");
        return;
    }
    const uint32_t expected = manifest(offered);
    if (mode == "inspect") {
        std::printf("fingerprint=%u generation=%u assets_manifest=1\n", expected, offered.header.generation);
        return;
    }
    check(expected == fingerprint, "probe supplied incorrect fingerprint");
    if (mode == "profile-hold") {
        netwire::Writer ready;
        ready.u32(fingerprint);
        client.send(netwire::ContentReady, ready.data, offered.header.session, offered.header.generation);
        const Message welcomed = client.receive();
        check(welcomed.header.type == netwire::Welcome, "spoofed profile was not admitted");
        netwire::Reader welcome(welcomed.body.data(), welcomed.body.size());
        const uint32_t slot = welcome.u32(), slotGeneration = welcome.u32(), mapCRC = welcome.u32();
        check(welcome.ok && slot < 8, "invalid welcome slot");
        netwire::Writer mapReady;
        mapReady.u32(slotGeneration); mapReady.u32(mapCRC);
        client.send(netwire::Ready, mapReady.data, welcomed.header.session, welcomed.header.generation);
        // Profile updates must not turn the client's preference into a server
        // rule. The production protocol currently ignores this message type.
        client.send(netwire::Profile, hello(fingerprint, false, 3, 0), welcomed.header.session, welcomed.header.generation);
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        bool installed = false;
        unsigned snapshots = 0;
        Message message;
        while (!std::ifstream(trigger).good()) {
            check(Clock::now() < deadline, "profile probe release timeout");
            if (client.poll(message) && message.header.type == netwire::Snapshot) {
                netwire::Reader snapshot(message.body.data(), message.body.size());
                snapshot.u32();
                netwire::Writer ack; ack.u32(snapshot.u32());
                client.send(netwire::SnapshotAck, ack.data, message.header.session, message.header.generation);
                ++snapshots;
                if (!installed) {
                    std::printf("profile_ready=1 slot=%u requested_autoaim=3 requested_skill=0\n", slot);
                    std::fflush(stdout);
                    installed = true;
                }
            }
            check(!client.disconnected, "spoofed-profile client disconnected");
        }
        check(installed, "profile peer never received authoritative state");
        std::printf("profile_snapshots=%u\n", snapshots);
        return;
    }
    if (mode == "invalid-ready" || mode == "wrong-ready") {
        netwire::Writer ready;
        if (mode == "invalid-ready") ready.u16(uint16_t(fingerprint));
        else ready.u32(fingerprint ^ 1u);
        client.send(netwire::ContentReady, ready.data, offered.header.session, offered.header.generation);
        const Message rejected = client.receive();
        check(rejected.header.type == netwire::Reject, "bad content reply was admitted");
        netwire::Reader reason(rejected.body.data(), rejected.body.size());
        const std::string explanation = reason.text(160);
        check(reason.end() && explanation.find("content mismatch") != std::string::npos, "bad reply rejection unclear");
        std::puts("invalid_ready_rejected=1");
        return;
    }
    check(mode == "race", "unknown probe mode");
    for (unsigned i = 0; i < 32; ++i) client.send(netwire::Hello, hello(fingerprint));
    client.silence(600);
    // An unrelated session cannot authorize an offered content check.
    netwire::Writer ready;
    ready.u32(fingerprint);
    client.send(netwire::ContentReady, ready.data, offered.header.session ^ 1u, offered.header.generation);
    client.silence(300);
    std::printf("offered_generation=%u repeated_hello_suppressed=1 wrong_session_ignored=1\n", offered.header.generation);
    std::fflush(stdout);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!std::ifstream(trigger).good()) {
        check(Clock::now() < deadline, "map change trigger timeout");
        client.silence(25);
    }
    client.send(netwire::ContentReady, ready.data, offered.header.session, offered.header.generation);
    const Message welcomed = client.receive();
    check(welcomed.header.type == netwire::Welcome, "stale map generation did not admit valid content");
    check(welcomed.header.generation != offered.header.generation, "server did not welcome current map");
    std::printf("welcome_generation=%u stale_generation_ready_admitted=1\n", welcomed.header.generation);
}
void fakeServer(unsigned port, const std::string &mode) {
    Endpoint server(port, true);
    std::puts("listening=1"); std::fflush(stdout);
    const Message request = server.receive(30);
    check(request.header.type == netwire::Hello, "client did not send Hello");
    netwire::Reader helloReader(request.body.data(), request.body.size());
    const uint32_t fingerprint = helloReader.u32();
    helloReader.text(15); helloReader.u8(); helloReader.u8(); helloReader.u8();
    check(helloReader.u32() == netassets::ManifestVersion && helloReader.end(), "client omitted manifest capability");
    netwire::Writer response;
    uint32_t type = netwire::ContentManifest;
    if (mode == "old-reject") {
        type = netwire::Reject;
        response.text("Invalid client profile", 160);
    } else if (mode == "no-manifest") {
        type = netwire::Welcome;
    } else {
        response.u32(fingerprint);
        response.u32(netassets::ManifestVersion);
        if (mode == "bad-count") response.u32(uint32_t(netassets::MaxAssets + 1));
        else if (mode == "truncated") {
            response.u32(1);
            response.text("archive:system", netassets::MaxAssetKey);
            response.text("BLOOD.RFF", netassets::MaxAssetName);
            response.u32(100); response.u32(0);
            response.u8(0); // Only one of the required sixteen digest bytes.
        } else throw std::runtime_error("unknown fake server mode");
    }
    server.send(type, response.data, 0x73492811u, 1);
    if (mode == "old-reject") enet_peer_disconnect_later(server.peer, 0);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    Message message;
    while (Clock::now() < deadline && !server.disconnected)
        check(!server.poll(message), "client sent gameplay or content approval after invalid manifest");
    check(server.disconnected, "client did not disconnect after invalid manifest");
    std::puts("invalid_server_disconnected=1 content_ready_sent=0");
}
}
int main(int argc, char **argv) {
    try {
        check(argc >= 4, "usage: peer probe|server|version-server PORT MODE [CRC [TRIGGER]], or version-probe PORT VERSION");
        check(enet_initialize() == 0, "ENet init");
        const unsigned port = unsigned(std::strtoul(argv[2], nullptr, 10));
        check(port && port <= 65535, "invalid port");
        if (std::string(argv[1]) == "probe")
            probe(port, argv[3], argc > 4 ? uint32_t(std::strtoul(argv[4], nullptr, 10)) : 0, argc > 5 ? argv[5] : "");
        else if (std::string(argv[1]) == "server") fakeServer(port, argv[3]);
        else if (std::string(argv[1]) == "version-probe")
            versionProbe(port, uint32_t(std::strtoul(argv[3], nullptr, 10)));
        else if (std::string(argv[1]) == "version-server") versionServer(port, argv[3]);
        else throw std::runtime_error("unknown peer mode");
        enet_deinitialize();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
