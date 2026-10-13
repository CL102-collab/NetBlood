// SPDX-License-Identifier: GPL-2.0-or-later
// Printable user-chat payloads, preserving the historic public Chat layout.
#pragma once

#include "netprotocol.h"

namespace netchat
{
constexpr size_t MaxTextBytes = 240;

struct Request
{
    std::string text;
};

struct Message
{
    uint32_t sender = 0;
    std::string text;
};

inline bool validText(const std::string& text)
{
    if (text.size() > MaxTextBytes) return false;
    // Keep the game's extended character set, but never permit newlines or
    // other controls to impersonate another chat line or its channel label.
    for (unsigned char character : text)
        if (character < 32 || character == 127) return false;
    return true;
}

inline bool writeRequest(netwire::Writer& writer, const Request& request)
{
    if (!validText(request.text)) return false;
    writer.text(request.text, MaxTextBytes);
    return true;
}

inline bool readRequest(netwire::Reader& reader, Request& request)
{
    Request candidate;
    candidate.text = reader.text(MaxTextBytes);
    if (!reader.end() || !validText(candidate.text)) return false;
    request = candidate;
    return true;
}

inline bool writeMessage(netwire::Writer& writer, const Message& message)
{
    if (message.sender >= 8 || !validText(message.text)) return false;
    writer.u8(uint8_t(message.sender));
    writer.text(message.text, MaxTextBytes);
    return true;
}

inline bool readMessage(netwire::Reader& reader, Message& message)
{
    // Team delivery is strictly printable. Historic public Chat delivery also
    // carries system notices with line breaks and keeps its legacy decoder.
    Message candidate;
    candidate.sender = reader.u8();
    candidate.text = reader.text(MaxTextBytes);
    if (!reader.end() || candidate.sender >= 8 || !validText(candidate.text)) return false;
    message = candidate;
    return true;
}
}
