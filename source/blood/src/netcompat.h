// SPDX-License-Identifier: GPL-2.0-or-later
// Stable connection diagnostics, independent of the gameplay wire version.
#pragma once

#include "netprotocol.h"

namespace netcompat
{
constexpr uint32_t ServerInfoMagic = 0x3156424e; // NBV1
constexpr uint32_t ServerInfoSchema = 1;
constexpr size_t MaxBuild = 96;
constexpr size_t MaxReason = 160;

struct ServerInfo
{
    uint32_t requiredProtocol = netwire::Version;
    std::string build;
};

inline bool printable(const std::string& text, size_t maximum)
{
    if (text.size() > maximum) return false;
    for (unsigned char c : text)
        if (c < 32 || c > 126) return false;
    return true;
}

// Identification only: a matching magic still requires the full decoder below.
inline bool isServerInfo(const void* data, size_t size)
{
    if (!data || size < sizeof(uint32_t)) return false;
    netwire::Reader reader(data, size);
    return reader.u32() == ServerInfoMagic;
}

inline std::vector<uint8_t> encodeServerInfo(const ServerInfo& info)
{
    if (!info.requiredProtocol || !printable(info.build, MaxBuild)) return {};
    netwire::Writer writer;
    writer.u32(ServerInfoMagic);
    writer.u32(ServerInfoSchema);
    writer.u32(info.requiredProtocol);
    writer.text(info.build, MaxBuild);
    return writer.data;
}

inline bool decodeServerInfo(const void* data, size_t size, ServerInfo& info)
{
    if (!data || size < 14 || size > 14 + MaxBuild) return false;
    netwire::Reader reader(data, size);
    if (reader.u32() != ServerInfoMagic || reader.u32() != ServerInfoSchema) return false;
    ServerInfo candidate;
    candidate.requiredProtocol = reader.u32();
    candidate.build = reader.text(MaxBuild);
    if (!reader.end() || !candidate.requiredProtocol || !printable(candidate.build, MaxBuild)) return false;
    info = candidate;
    return true;
}

// Reject's envelope and text layout are stable across gameplay protocol
// versions. Stamp the peer's advertised version so existing clients can show
// the error without relaxing their gameplay packet validation.
inline std::vector<uint8_t> encodeReject(uint32_t peerProtocol, const std::string& reason)
{
    if (!peerProtocol || !printable(reason, MaxReason)) return {};
    netwire::Writer writer;
    writer.u32(netwire::Magic);
    writer.u32(peerProtocol);
    writer.u32(netwire::Reject);
    writer.u32(0); // Session, map generation and sequence are not yet established.
    writer.u32(0);
    writer.u32(0);
    writer.u32(static_cast<uint32_t>(2 + reason.size()));
    writer.text(reason, MaxReason);
    return writer.data;
}

inline bool decodeReject(const void* data, size_t size, uint32_t& wireProtocol, std::string& reason)
{
    if (!data || size < netwire::HeaderSize + 2 || size > netwire::HeaderSize + 2 + MaxReason) return false;
    netwire::Reader reader(data, size);
    if (reader.u32() != netwire::Magic) return false;
    uint32_t const candidateProtocol = reader.u32();
    if (!candidateProtocol || reader.u32() != netwire::Reject) return false;
    reader.u32(); reader.u32(); reader.u32(); // Preserve compatibility with existing server envelopes.
    if (reader.u32() != size - netwire::HeaderSize) return false;
    std::string const candidateReason = reader.text(MaxReason);
    if (!reader.end() || !printable(candidateReason, MaxReason)) return false;
    wireProtocol = candidateProtocol;
    reason = candidateReason;
    return true;
}
}
