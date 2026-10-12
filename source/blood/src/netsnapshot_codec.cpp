// SPDX-License-Identifier: GPL-2.0-or-later
#include "netsnapshot.h"
#include "lz4.h"

#include <algorithm>
#include <utility>

namespace
{
constexpr uint32_t kMagic = 0x315a534e; // NSZ1
constexpr size_t kHeaderSize = 28;
constexpr uint32_t kDelta = 1, kCompressed = 2;

void put(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (unsigned i = 0; i != 4; ++i) bytes.push_back(uint8_t(value >> (8*i)));
}

uint32_t get(const std::vector<uint8_t>& bytes, size_t pos)
{
    uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) value |= uint32_t(bytes[pos+i]) << (8*i);
    return value;
}

std::vector<uint8_t> compress(const std::vector<uint8_t>& input, bool& compressed)
{
    std::vector<uint8_t> output(LZ4_compressBound(int(input.size())));
    int const length = LZ4_compress_default(reinterpret_cast<const char*>(input.data()),
        reinterpret_cast<char*>(output.data()), int(input.size()), int(output.size()));
    compressed = length > 0 && size_t(length) < input.size();
    if (compressed) output.resize(size_t(length));
    else output = input;
    return output;
}
}

uint32_t netSnapshotHash(const std::vector<uint8_t>& state)
{
    // Corruption/baseline guard, not authentication. The transport validates
    // connection identity and the protocol before handing data to this codec.
    uint32_t hash = 2166136261u;
    for (uint8_t byte : state) hash = (hash ^ byte) * 16777619u;
    return hash;
}

bool netSnapshotPack(const std::vector<uint8_t>& state, const std::vector<uint8_t>* baseline,
                     std::vector<uint8_t>& wire, bool& usedDelta, std::string& error)
{
    usedDelta = false;
    if (state.empty() || state.size() > kNetSnapshotMaxBytes ||
        (baseline && (baseline->empty() || baseline->size() > kNetSnapshotMaxBytes)))
    {
        error = "snapshot or baseline length is outside the protocol limit";
        return false;
    }
    bool compressed = false;
    auto payload = compress(state, compressed);
    if (baseline)
    {
        auto difference = state;
        for (size_t i = 0, n = std::min(state.size(), baseline->size()); i != n; ++i)
            difference[i] ^= (*baseline)[i];
        bool deltaCompressed = false;
        auto delta = compress(difference, deltaCompressed);
        if (delta.size() < payload.size())
        {
            payload.swap(delta);
            compressed = deltaCompressed;
            usedDelta = true;
        }
    }
    std::vector<uint8_t> result;
    result.reserve(kHeaderSize + payload.size());
    put(result, kMagic);
    put(result, kNetSnapshotVersion);
    put(result, (usedDelta ? kDelta : 0) | (compressed ? kCompressed : 0));
    put(result, uint32_t(state.size()));
    put(result, usedDelta ? uint32_t(baseline->size()) : 0);
    put(result, netSnapshotHash(state));
    put(result, usedDelta ? netSnapshotHash(*baseline) : 0);
    result.insert(result.end(), payload.begin(), payload.end());
    wire.swap(result);
    error.clear();
    return true;
}

bool netSnapshotUnpack(const std::vector<uint8_t>& wire, const std::vector<uint8_t>* baseline,
                       std::vector<uint8_t>& state, std::string& error)
{
    if (wire.size() <= kHeaderSize || wire.size() > kNetSnapshotMaxBytes + kHeaderSize ||
        get(wire, 0) != kMagic || get(wire, 4) != kNetSnapshotVersion)
    {
        error = "invalid snapshot compression header";
        return false;
    }
    uint32_t const flags = get(wire, 8), length = get(wire, 12), baseLength = get(wire, 16);
    if ((flags & ~(kDelta | kCompressed)) || !length || length > kNetSnapshotMaxBytes)
    {
        error = "invalid snapshot compression flags or length";
        return false;
    }
    if (flags & kDelta)
    {
        if (!baseline || baseline->size() != baseLength || !baseLength ||
            baseLength > kNetSnapshotMaxBytes || netSnapshotHash(*baseline) != get(wire, 24))
        {
            error = "snapshot delta requires its acknowledged baseline";
            return false;
        }
    }
    else if (baseLength || get(wire, 24))
    {
        error = "full snapshot unexpectedly references a baseline";
        return false;
    }
    std::vector<uint8_t> result(length);
    size_t const payloadLength = wire.size() - kHeaderSize;
    if (flags & kCompressed)
    {
        int const actual = LZ4_decompress_safe(reinterpret_cast<const char*>(wire.data()+kHeaderSize),
            reinterpret_cast<char*>(result.data()), int(payloadLength), int(length));
        if (actual != int(length))
        {
            error = "truncated or invalid compressed snapshot";
            return false;
        }
    }
    else
    {
        if (payloadLength != length)
        {
            error = "snapshot payload length does not match its header";
            return false;
        }
        std::copy(wire.begin()+kHeaderSize, wire.end(), result.begin());
    }
    if (flags & kDelta)
        for (size_t i = 0, n = std::min(result.size(), baseline->size()); i != n; ++i)
            result[i] ^= (*baseline)[i];
    if (netSnapshotHash(result) != get(wire, 20))
    {
        error = "snapshot checksum mismatch";
        return false;
    }
    state.swap(result);
    error.clear();
    return true;
}
