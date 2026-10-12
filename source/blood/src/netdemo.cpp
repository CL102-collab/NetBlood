// NBlood packet demos. Copyright (C) 2026 NBlood contributors.
// SPDX-License-Identifier: GPL-2.0-only
#include "netdemo.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
constexpr uint8_t kMagic[8] = {'N', 'B', 'P', 'K', 'D', 'E', 'M', 0x1a};
constexpr uint32_t kPacket = 0x31544b50; // PKT1, little endian
constexpr uint32_t kRebase = 0x31545352; // RST1, map-generation tick restart
constexpr uint32_t kEnd = 0x31444e45;    // END1, little endian
constexpr size_t kRecordHeaderBytes = 24;
constexpr size_t kMetadataFixedBytes = 28;
constexpr size_t kMaxMapNameBytes = 1024;

struct DemoState
{
    FILE *file = nullptr;
    uint32_t tickRate = 30;
    uint32_t lastTick = 0;
    uint64_t elapsedTicks = 0;
    bool first = true;
    std::vector<uint8_t> pending;
    uint32_t pendingTick = 0;
    uint64_t pendingElapsedTicks = 0;
};

DemoState gRecorder, gPlayback;
std::string gError;

void Put32(std::vector<uint8_t> &out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (i * 8)));
}

void Put64(std::vector<uint8_t> &out, uint64_t value)
{
    Put32(out, uint32_t(value));
    Put32(out, uint32_t(value >> 32));
}

uint32_t Get32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t Get64(const uint8_t *p)
{
    return uint64_t(Get32(p)) | (uint64_t(Get32(p + 4)) << 32);
}

// CRC32, including the record's time and length. File lengths
// are independently bounded before allocation; a CRC is not authentication.
uint32_t Checksum(const uint8_t *data, size_t size, uint32_t crc = 0xffffffffu)
{
    static const auto table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t index = 0; index < result.size(); ++index)
        {
            uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
            result[index] = value;
        }
        return result;
    }();
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return crc;
}

void Close(DemoState &state)
{
    if (state.file) std::fclose(state.file);
    state = DemoState{};
}

bool Fail(DemoState &state, const char *message)
{
    gError = message;
    Close(state);
    return false;
}

bool Write(DemoState &state, const uint8_t *bytes, size_t size)
{
    if (std::fwrite(bytes, 1, size, state.file) == size) return true;
    return Fail(state, "Could not write packet demo (disk full or I/O failure)");
}

bool WriteRecord(uint32_t marker, uint32_t tick, const std::vector<uint8_t> &packet)
{
    std::vector<uint8_t> header;
    Put32(header, marker);
    Put32(header, uint32_t(packet.size()));
    Put64(header, gRecorder.elapsedTicks);
    Put32(header, tick);
    uint32_t const crc = ~Checksum(packet.data(), packet.size(), Checksum(header.data(), header.size()));
    Put32(header, crc);
    return Write(gRecorder, header.data(), header.size())
        && (packet.empty() || Write(gRecorder, packet.data(), packet.size()));
}

bool ReadPending()
{
    uint8_t header[kRecordHeaderBytes];
    if (std::fread(header, 1, sizeof(header), gPlayback.file) != sizeof(header))
        return Fail(gPlayback, "Truncated packet demo record or missing end marker");
    uint32_t const marker = Get32(header);
    uint32_t const size = Get32(header + 4);
    uint64_t const elapsed = Get64(header + 8);
    uint32_t const tick = Get32(header + 16);
    if ((marker != kPacket && marker != kRebase && marker != kEnd)
        || (marker != kEnd && (!size || size > kNetDemoMaxPacketBytes))
        || (marker == kEnd && size != 0))
        return Fail(gPlayback, "Invalid packet demo record type or length");
    if (gPlayback.first ? elapsed != 0 : elapsed < gPlayback.elapsedTicks)
        return Fail(gPlayback, "Invalid packet demo timeline");
    if (!gPlayback.first)
    {
        uint32_t const delta = marker == kRebase ? 0 : tick - gPlayback.lastTick;
        if (delta > 0x7fffffffu || elapsed - gPlayback.elapsedTicks != delta)
            return Fail(gPlayback, "Packet demo tick and timeline disagree");
    }
    std::vector<uint8_t> bytes(size);
    if (size && std::fread(bytes.data(), 1, size, gPlayback.file) != size)
        return Fail(gPlayback, "Truncated packet demo payload");
    uint32_t const crc = ~Checksum(bytes.data(), bytes.size(), Checksum(header, 20));
    if (crc != Get32(header + 20))
        return Fail(gPlayback, "Packet demo record checksum mismatch");
    if (marker == kEnd)
    {
        if (gPlayback.first) return Fail(gPlayback, "Packet demo contains no server messages");
        if (std::fgetc(gPlayback.file) != EOF || std::ferror(gPlayback.file))
            return Fail(gPlayback, "Unexpected data after packet demo end marker");
        Close(gPlayback);
        return false;
    }
    gPlayback.pending.swap(bytes);
    gPlayback.pendingTick = tick;
    gPlayback.pendingElapsedTicks = elapsed;
    gPlayback.elapsedTicks = elapsed;
    gPlayback.lastTick = tick;
    gPlayback.first = false;
    return true;
}
} // namespace

bool netDemoStartRecording(const char *path, const NetDemoInfo &info, std::string &error)
{
    error.clear();
    if (gRecorder.file || gPlayback.file)
    {
        error = "A packet demo is already recording or playing";
        return false;
    }
    gError.clear();
    if (!path || !*path || !info.protocolVersion || !info.tickRate || info.tickRate > 1000
        || info.localSlot < -1 || info.localSlot >= 8 || info.mapName.empty()
        || info.mapName.size() > kMaxMapNameBytes || info.mapName.find('\0') != std::string::npos)
    {
        error = gError = "Invalid packet demo metadata";
        return false;
    }
    gRecorder.file = std::fopen(path, "wb");
    if (!gRecorder.file)
    {
        error = gError = "Could not create packet demo";
        return false;
    }
    gRecorder.tickRate = info.tickRate;
    std::vector<uint8_t> metadata;
    Put32(metadata, info.protocolVersion);
    Put32(metadata, info.tickRate);
    Put32(metadata, info.mapCrc);
    Put32(metadata, info.contentCrc);
    Put32(metadata, uint32_t(info.localSlot));
    Put32(metadata, uint32_t(info.mapName.size()));
    Put32(metadata, 0); // reserved, must remain zero for version 1
    metadata.insert(metadata.end(), info.mapName.begin(), info.mapName.end());
    std::vector<uint8_t> header(kMagic, kMagic + sizeof(kMagic));
    Put32(header, kNetDemoFormatVersion);
    Put32(header, uint32_t(metadata.size()));
    Put32(header, ~Checksum(metadata.data(), metadata.size()));
    if (!Write(gRecorder, header.data(), header.size()) || !Write(gRecorder, metadata.data(), metadata.size()))
    {
        error = gError;
        return false;
    }
    return true;
}

bool netDemoRecordPacket(uint32_t serverTick, const std::vector<uint8_t> &packet, bool resetClock)
{
    if (!gRecorder.file) return false;
    if (packet.empty() || packet.size() > kNetDemoMaxPacketBytes)
        return Fail(gRecorder, "Invalid packet demo payload length");
    if (!gRecorder.first)
    {
        uint32_t const delta = resetClock ? 0 : serverTick - gRecorder.lastTick;
        if (delta > 0x7fffffffu || gRecorder.elapsedTicks > std::numeric_limits<uint64_t>::max() - delta)
            return Fail(gRecorder, "Packet demo server time moved backwards");
        gRecorder.elapsedTicks += delta;
    }
    gRecorder.first = false;
    gRecorder.lastTick = serverTick;
    return WriteRecord(resetClock ? kRebase : kPacket, serverTick, packet);
}

void netDemoStopRecording()
{
    if (!gRecorder.file) return;
    if (!WriteRecord(kEnd, gRecorder.lastTick, {})) return;
    FILE *file = gRecorder.file;
    gRecorder = DemoState{};
    if (std::fclose(file)) gError = "Could not finish packet demo (disk full or I/O failure)";
}

bool netDemoIsRecording() { return gRecorder.file != nullptr; }

bool netDemoStartPlayback(const char *path, NetDemoInfo &info, std::string &error)
{
    error.clear();
    if (gRecorder.file || gPlayback.file)
    {
        error = "A packet demo is already recording or playing";
        return false;
    }
    gError.clear();
    if (!path || !*path || !(gPlayback.file = std::fopen(path, "rb")))
    {
        error = gError = "Could not open packet demo";
        return false;
    }
    uint8_t header[20];
    if (std::fread(header, 1, sizeof(header), gPlayback.file) != sizeof(header)
        || std::memcmp(header, kMagic, sizeof(kMagic)))
        Fail(gPlayback, "Not a packet demo or truncated header (use legacy playback for DEM/EDM files)");
    else if (Get32(header + 8) != kNetDemoFormatVersion)
        Fail(gPlayback, "Unsupported packet demo format version");
    else
    {
        uint32_t const size = Get32(header + 12);
        if (size < kMetadataFixedBytes + 1 || size > kMetadataFixedBytes + kMaxMapNameBytes)
            Fail(gPlayback, "Invalid packet demo metadata length");
        else
        {
            std::vector<uint8_t> metadata(size);
            if (std::fread(metadata.data(), 1, size, gPlayback.file) != size)
                Fail(gPlayback, "Truncated packet demo metadata");
            else if (~Checksum(metadata.data(), size) != Get32(header + 16))
                Fail(gPlayback, "Packet demo metadata checksum mismatch");
            else
            {
                NetDemoInfo parsed;
                parsed.protocolVersion = Get32(metadata.data());
                parsed.tickRate = Get32(metadata.data() + 4);
                parsed.mapCrc = Get32(metadata.data() + 8);
                parsed.contentCrc = Get32(metadata.data() + 12);
                uint32_t const slot = Get32(metadata.data() + 16);
                parsed.localSlot = slot == 0xffffffffu ? -1 : int32_t(slot);
                uint32_t const nameBytes = Get32(metadata.data() + 20);
                parsed.mapName.assign(reinterpret_cast<const char *>(metadata.data() + kMetadataFixedBytes), size - kMetadataFixedBytes);
                if (!parsed.protocolVersion || !parsed.tickRate || parsed.tickRate > 1000
                    || (slot != 0xffffffffu && slot >= 8) || nameBytes != parsed.mapName.size()
                    || parsed.mapName.find('\0') != std::string::npos || Get32(metadata.data() + 24) != 0)
                    Fail(gPlayback, "Invalid packet demo metadata");
                else
                {
                    gPlayback.tickRate = parsed.tickRate;
                    if (ReadPending())
                    {
                        info = parsed;
                        return true;
                    }
                }
            }
        }
    }
    error = gError;
    return false;
}

bool netDemoNextPacket(uint64_t elapsedMs, std::vector<uint8_t> &packet, uint32_t &serverTick)
{
    if (!gPlayback.file) return false;
    if (gPlayback.pending.empty() && !ReadPending()) return false;
    // Compare without multiplying an unbounded caller timestamp by tickRate.
    uint64_t const wholeSeconds = gPlayback.pendingElapsedTicks / gPlayback.tickRate;
    uint64_t const partialMs = (gPlayback.pendingElapsedTicks % gPlayback.tickRate) * 1000;
    uint64_t const remainder = (partialMs + gPlayback.tickRate - 1) / gPlayback.tickRate;
    uint64_t const dueMs = wholeSeconds > (std::numeric_limits<uint64_t>::max() - remainder) / 1000
        ? std::numeric_limits<uint64_t>::max() : wholeSeconds * 1000 + remainder;
    if (elapsedMs < dueMs) return false;
    packet = std::move(gPlayback.pending);
    gPlayback.pending.clear();
    serverTick = gPlayback.pendingTick;
    return true;
}

void netDemoStopPlayback() { Close(gPlayback); }
bool netDemoIsPlaying() { return gPlayback.file != nullptr; }
const std::string &netDemoLastError() { return gError; }
