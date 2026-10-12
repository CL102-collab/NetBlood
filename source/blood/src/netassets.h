// SPDX-License-Identifier: GPL-2.0-or-later
// Content identity and bounded manifest primitives; no engine or platform types.
#pragma once
#include "netprotocol.h"
#include <array>
#include <algorithm>
#include <map>
#include <utility>

namespace netassets
{
using Digest = std::array<uint8_t, 16>;

// RFC 1321 MD5, used only to identify files for the user, not authenticate them.
class MD5
{
    uint32_t state_[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint64_t bytes_ = 0;
    uint8_t buffer_[64] = {};
    size_t buffered_ = 0;

    static uint32_t rotate(uint32_t value, unsigned bits)
    { return (value << bits) | (value >> (32 - bits)); }

    void block(const uint8_t *data)
    {
        static const uint32_t constants[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
            0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
            0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
            0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
            0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
            0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
            0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
            0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
            0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
            0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
            0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
            0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
            0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
            0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
        };
        static const unsigned shifts[64] = {
            7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
            5,9,14,20, 5,9,14,20, 5,9,14,20, 5,9,14,20,
            4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
            6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
        };
        uint32_t words[16];
        for (unsigned i = 0; i < 16; ++i)
            words[i] = uint32_t(data[i * 4]) | (uint32_t(data[i * 4 + 1]) << 8) |
                       (uint32_t(data[i * 4 + 2]) << 16) | (uint32_t(data[i * 4 + 3]) << 24);
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        for (unsigned i = 0; i < 64; ++i)
        {
            uint32_t f;
            unsigned word;
            if (i < 16)      { f = (b & c) | (~b & d); word = i; }
            else if (i < 32) { f = (d & b) | (~d & c); word = (5 * i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d; word = (3 * i + 5) & 15; }
            else            { f = c ^ (b | ~d); word = (7 * i) & 15; }
            const uint32_t previousD = d;
            d = c;
            c = b;
            b += rotate(a + f + constants[i] + words[word], shifts[i]);
            a = previousD;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
    }

public:
    void update(const void *data, size_t size)
    {
        if (!size)
            return;
        const auto *bytes = static_cast<const uint8_t *>(data);
        bytes_ += uint64_t(size);
        if (buffered_)
        {
            const size_t copy = std::min(size, sizeof(buffer_) - buffered_);
            std::memcpy(buffer_ + buffered_, bytes, copy);
            buffered_ += copy;
            bytes += copy;
            size -= copy;
            if (buffered_ == sizeof(buffer_))
            {
                block(buffer_);
                buffered_ = 0;
            }
        }
        while (size >= sizeof(buffer_))
        {
            block(bytes);
            bytes += sizeof(buffer_);
            size -= sizeof(buffer_);
        }
        if (size)
        {
            std::memcpy(buffer_, bytes, size);
            buffered_ = size;
        }
    }

    // Finishing a copy permits repeated inspection and subsequent updates.
    Digest finish() const
    {
        MD5 final = *this;
        const uint64_t bits = bytes_ * uint64_t(8);
        uint8_t padding[64] = {0x80};
        final.update(padding, buffered_ < 56 ? 56 - buffered_ : 120 - buffered_);
        uint8_t length[8];
        for (unsigned i = 0; i < 8; ++i)
            length[i] = uint8_t(bits >> (8 * i));
        final.update(length, sizeof(length));
        Digest result;
        for (unsigned i = 0; i < result.size(); ++i)
            result[i] = uint8_t(final.state_[i / 4] >> (8 * (i % 4)));
        return result;
    }
};

inline std::string hex(const Digest &digest)
{
    static const char alphabet[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (size_t i = 0; i < digest.size(); ++i)
    {
        result[2 * i] = alphabet[digest[i] >> 4];
        result[2 * i + 1] = alphabet[digest[i] & 15];
    }
    return result;
}

struct Asset
{
    std::string key, name;
    uint64_t size = 0;
    Digest md5 = {};
};

constexpr uint32_t ManifestVersion = 1;
constexpr size_t MaxAssets = 4096;
constexpr size_t MaxAssetKey = 512;
constexpr size_t MaxAssetName = 512;
constexpr size_t MaxManifestBytes = 2u * 1024u * 1024u;

// Manifest names are diagnostic text, never paths passed to the filesystem.
// Disallow control bytes (including console newlines and NUL) while retaining
// ordinary UTF-8 names. The inventory producer strips host directory prefixes.
inline bool validText(const std::string &text, size_t maximum)
{
    if (text.empty() || text.size() > maximum)
        return false;
    for (unsigned char c : text)
        if (c < 32 || c == 127)
            return false;
    return true;
}

inline bool encodeManifest(const std::vector<Asset> &assets, std::vector<uint8_t> &bytes,
                           std::string &error)
{
    if (assets.size() > MaxAssets)
    {
        error = "too many required assets";
        return false;
    }
    std::map<std::string, bool> keys;
    size_t length = 8;
    for (const auto &asset : assets)
    {
        if (!validText(asset.key, MaxAssetKey) || !validText(asset.name, MaxAssetName))
        {
            error = "invalid asset name or identity";
            return false;
        }
        if (!keys.emplace(asset.key, true).second)
        {
            error = "duplicate asset identity";
            return false;
        }
        length += 2 + asset.key.size() + 2 + asset.name.size() + 8 + asset.md5.size();
        if (length > MaxManifestBytes)
        {
            error = "asset manifest is too large";
            return false;
        }
    }
    netwire::Writer writer;
    writer.data.reserve(length);
    writer.u32(ManifestVersion);
    writer.u32(uint32_t(assets.size()));
    for (const auto &asset : assets)
    {
        writer.text(asset.key, MaxAssetKey);
        writer.text(asset.name, MaxAssetName);
        writer.u32(uint32_t(asset.size));
        writer.u32(uint32_t(asset.size >> 32));
        writer.bytes(asset.md5.data(), asset.md5.size());
    }
    bytes.swap(writer.data);
    error.clear();
    return true;
}

inline bool decodeManifest(netwire::Reader &reader, std::vector<Asset> &assets, std::string &error)
{
    if (reader.pos > reader.size || reader.size - reader.pos > MaxManifestBytes)
    {
        error = "asset manifest is too large";
        return false;
    }
    if (reader.u32() != ManifestVersion)
    {
        error = "unsupported asset manifest version";
        return false;
    }
    const uint32_t count = reader.u32();
    if (!reader.ok || count > MaxAssets)
    {
        error = "invalid required asset count";
        return false;
    }
    std::vector<Asset> candidate;
    candidate.reserve(count);
    std::map<std::string, bool> keys;
    for (uint32_t i = 0; i < count; ++i)
    {
        Asset asset;
        asset.key = reader.text(MaxAssetKey);
        asset.name = reader.text(MaxAssetName);
        const uint32_t low = reader.u32();
        asset.size = low | (uint64_t(reader.u32()) << 32);
        if (!reader.bytes(asset.md5.data(), asset.md5.size()) || !reader.ok ||
            !validText(asset.key, MaxAssetKey) || !validText(asset.name, MaxAssetName))
        {
            error = "invalid or truncated asset entry";
            return false;
        }
        if (!keys.emplace(asset.key, true).second)
        {
            error = "duplicate asset identity";
            return false;
        }
        candidate.push_back(std::move(asset));
    }
    if (!reader.end())
    {
        error = "trailing or truncated asset manifest data";
        return false;
    }
    assets.swap(candidate);
    error.clear();
    return true;
}

enum IssueKind { Missing, Mismatch, NotLoaded };
struct Issue
{
    Asset required;
    IssueKind kind = Missing;
    Asset local; // Populated for Mismatch or NotLoaded (inventory-specific).
};

inline std::vector<Issue> compareManifest(const std::vector<Asset> &required,
                                         const std::vector<Asset> &local)
{
    std::map<std::string, const Asset *> byKey;
    for (const auto &asset : local)
        byKey.emplace(asset.key, &asset);
    std::vector<Issue> issues;
    for (const auto &asset : required)
    {
        const auto found = byKey.find(asset.key);
        if (found != byKey.end() && asset.size == found->second->size && asset.md5 == found->second->md5)
            continue;
        Issue issue;
        issue.required = asset;
        if (found != byKey.end())
        {
            issue.kind = Mismatch;
            issue.local = *found->second;
        }
        issues.push_back(std::move(issue));
    }
    return issues;
}
}
