// SPDX-License-Identifier: GPL-2.0-or-later
#include "netchat.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace
{
unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

void rejectedRequest(const std::vector<uint8_t>& bytes)
{
    netchat::Request decoded;
    decoded.text = "unchanged request";
    netwire::Reader reader(bytes.data(), bytes.size());
    CHECK(!netchat::readRequest(reader, decoded));
    CHECK(decoded.text == "unchanged request");
}

void rejectedMessage(const std::vector<uint8_t>& bytes)
{
    netchat::Message decoded;
    decoded.sender = 6;
    decoded.text = "unchanged message";
    netwire::Reader reader(bytes.data(), bytes.size());
    CHECK(!netchat::readMessage(reader, decoded));
    CHECK(decoded.sender == 6 && decoded.text == "unchanged message");
}

void roundtrip(const std::string& text)
{
    netchat::Request request;
    request.text = text;
    netwire::Writer body;
    CHECK(netchat::writeRequest(body, request));
    CHECK(body.data.size() == text.size() + 2);

    // New public chat requests must retain the original length-prefixed bytes.
    netwire::Writer legacy;
    legacy.text(text, 240);
    CHECK(body.data == legacy.data);

    for (auto type : {netwire::Chat, netwire::TeamChat})
    {
        netwire::Header header;
        header.type = type;
        const auto packet = netwire::encode(header, body.data);
        netwire::Header received;
        netwire::Reader reader(nullptr, 0);
        CHECK(netwire::decode(packet.data(), packet.size(), received, reader));
        CHECK(received.type == type);
        netchat::Request decoded;
        CHECK(netchat::readRequest(reader, decoded) && reader.end());
        CHECK(decoded.text == text);
    }
    for (size_t length = 0; length < body.data.size(); ++length)
        rejectedRequest(std::vector<uint8_t>(body.data.begin(), body.data.begin() + length));
    auto extra = body.data;
    extra.push_back('!');
    rejectedRequest(extra);

    for (uint32_t sender = 0; sender < 8; ++sender)
    {
        netchat::Message message;
        message.sender = sender;
        message.text = text;
        netwire::Writer delivery;
        CHECK(netchat::writeMessage(delivery, message));
        legacy.data.clear();
        legacy.u8(uint8_t(sender));
        legacy.text(text, 240);
        CHECK(delivery.data == legacy.data);
        for (auto type : {netwire::Chat, netwire::TeamChat})
        {
            netwire::Header header;
            header.type = type;
            const auto packet = netwire::encode(header, delivery.data);
            netwire::Header received;
            netwire::Reader reader(nullptr, 0);
            CHECK(netwire::decode(packet.data(), packet.size(), received, reader));
            CHECK(received.type == type);
            netchat::Message decoded;
            CHECK(netchat::readMessage(reader, decoded) && reader.end());
            CHECK(decoded.sender == sender && decoded.text == text);
        }
        for (size_t length = 0; length < delivery.data.size(); ++length)
            rejectedMessage(std::vector<uint8_t>(delivery.data.begin(), delivery.data.begin() + length));
        extra = delivery.data;
        extra.push_back('!');
        rejectedMessage(extra);
    }
}

void validMessages()
{
    static_assert(netwire::Version == 7 && netwire::Chat == 7 && netwire::Intermission == 23 &&
                  netwire::TeamChat == 24, "Public and recorded chat retain their protocol identifiers");
    netchat::Request request;
    request.text = "hello";
    netwire::Writer writer;
    CHECK(netchat::writeRequest(writer, request));
    CHECK(writer.data == std::vector<uint8_t>({5, 0, 'h', 'e', 'l', 'l', 'o'}));
    netchat::Message message;
    message.sender = 3;
    message.text = "hello";
    writer.data.clear();
    CHECK(netchat::writeMessage(writer, message));
    CHECK(writer.data == std::vector<uint8_t>({3, 5, 0, 'h', 'e', 'l', 'l', 'o'}));

    roundtrip(""); // Preserve public chat's historic empty/space payload support.
    roundtrip("   ");
    roundtrip("Regroup at the blue key!");
    roundtrip(std::string(netchat::MaxTextBytes - 1, 'b'));
    roundtrip(std::string(netchat::MaxTextBytes, 'a'));
    std::string extended;
    for (unsigned character = 32; character <= 255; ++character)
        if (character != 127) extended += char(character);
    roundtrip(extended);
}

void legacyPublicNotices()
{
    // Public system messages and old packet demos can contain line breaks.
    // Keep their existing decoder/layout; only team delivery and user-originated
    // requests use the strict printable codec.
    const std::string notice = "Caleb defeated an opponent\n";
    netwire::Writer payload;
    payload.u8(2);
    payload.text(notice, 240);
    netwire::Header header;
    header.type = netwire::Chat;
    const auto packet = netwire::encode(header, payload.data);
    netwire::Header decodedHeader;
    netwire::Reader reader(nullptr, 0);
    CHECK(netwire::decode(packet.data(), packet.size(), decodedHeader, reader));
    CHECK(decodedHeader.type == netwire::Chat);
    CHECK(reader.u8() == 2);
    CHECK(reader.text(240) == notice && reader.end());
    rejectedMessage(payload.data);
}

void invalidMessages()
{
    // Reject every C0 control and DEL on encode and decode, including embedded
    // NULs and line breaks which could impersonate another player/channel.
    for (unsigned character = 0; character < 128; ++character)
    {
        if (character >= 32 && character != 127) continue;
        std::string text = "a";
        text += char(character);
        text += "b";
        netchat::Request request;
        request.text = text;
        netchat::Message message;
        message.sender = 0;
        message.text = text;
        netwire::Writer writer;
        writer.data = {42};
        CHECK(!netchat::writeRequest(writer, request));
        CHECK(writer.data == std::vector<uint8_t>({42}));
        CHECK(!netchat::writeMessage(writer, message));
        CHECK(writer.data == std::vector<uint8_t>({42}));
        writer.data.clear();
        writer.text(text, 240);
        rejectedRequest(writer.data);
        writer.data.insert(writer.data.begin(), 0);
        rejectedMessage(writer.data);
    }
    for (size_t length : {size_t(241), size_t(256), size_t(65535)})
    {
        netchat::Request request;
        request.text.assign(length, 'a');
        netchat::Message message;
        message.sender = 7;
        message.text = request.text;
        netwire::Writer writer;
        writer.data = {42};
        CHECK(!netchat::writeRequest(writer, request));
        CHECK(writer.data == std::vector<uint8_t>({42}));
        CHECK(!netchat::writeMessage(writer, message));
        CHECK(writer.data == std::vector<uint8_t>({42}));
        writer.data.clear();
        writer.text(request.text, length);
        rejectedRequest(writer.data);
        writer.data.insert(writer.data.begin(), 7);
        rejectedMessage(writer.data);
    }
    for (uint32_t sender : {8u, 255u, 256u, std::numeric_limits<uint32_t>::max()})
    {
        netchat::Message message;
        message.sender = sender;
        message.text = "hello";
        netwire::Writer writer;
        writer.data = {42};
        CHECK(!netchat::writeMessage(writer, message));
        CHECK(writer.data == std::vector<uint8_t>({42}));
    }
    for (unsigned sender = 8; sender <= 255; ++sender)
        rejectedMessage({uint8_t(sender), 1, 0, 'a'});
    rejectedRequest({0xff, 0xff});
    rejectedMessage({1, 0xff, 0xff});
}
}

int main()
{
    validMessages();
    legacyPublicNotices();
    invalidMessages();
    std::printf("PASS: %u public/team chat codec assertions (legacy bytes, bounds, controls, truncation, atomic rejection)\n", checks);
}
