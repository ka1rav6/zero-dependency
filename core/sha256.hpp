#pragma once

// core/sha256.hpp
//
// SHA-256 (FIPS 180-4), in-tree.
//
// ## Why this exists when core/hash.hpp already hashes
//
// They answer different questions and must not be confused. core/hash.hpp is
// FNV-1a and its own comment says so plainly: "Neither is a security boundary
// ... If either ever becomes a trust decision, this is the wrong function and
// the comment should stop you." A cache key only has to be stable.
//
// Design doc §6.3 step 3 and §12 Q3 need the other kind: the registry records a
// checksum per plugin version, and `kap plugin install` refuses a payload whose
// bytes do not match. That *is* a trust decision — it is the only thing
// standing between a user and a tampered plugin — so it needs a hash where
// producing a second input with the same digest is infeasible. FNV-1a is
// trivially forgeable; SHA-256 is not.
//
// §9 bans linking OpenSSL, not hashing. SHA-256 is about a hundred lines of
// arithmetic with a published test vector set, which is exactly the kind of
// thing the zero-dependency rule expects to be written in-tree rather than
// dragged in as a library.
//
// ## Scope
//
// This is a plain, constant-shape implementation: no SIMD, no assembly, no
// hardware intrinsics. Plugin payloads are kilobytes and are hashed once at
// install time, so a faster implementation would buy nothing and cost the
// ability to read this file and check it against the specification.
//
// The comparison helper is *not* constant-time and does not need to be: both
// digests here are public values (the index's and the payload's), so there is
// no secret whose timing could leak.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kap
{
namespace sha256
{

// A raw digest: 256 bits.
using Digest = std::array<std::uint8_t, 32>;

namespace detail
{

// The first 32 bits of the fractional parts of the cube roots of the first 64
// primes (FIPS 180-4 §4.2.2). Written out rather than computed so the table
// can be diffed against the specification by eye.
inline constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

// The initial hash value: the first 32 bits of the fractional parts of the
// square roots of the first eight primes (FIPS 180-4 §5.3.3).
inline constexpr std::array<std::uint32_t, 8> kInitialState = {0x6a09e667u,
                                                               0xbb67ae85u,
                                                               0x3c6ef372u,
                                                               0xa54ff53au,
                                                               0x510e527fu,
                                                               0x9b05688cu,
                                                               0x1f83d9abu,
                                                               0x5be0cd19u};

inline constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits)
{
    // The `& 31` is not decoration: shifting a 32-bit value by 32 is undefined
    // behaviour in C++, and every call site here passes a constant below 32,
    // but the mask makes the function safe to reuse without re-deriving that.
    return (value >> (bits & 31)) | (value << ((32 - bits) & 31));
}

// One 64-byte block (FIPS 180-4 §6.2.2).
inline void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block)
{
    std::array<std::uint32_t, 64> schedule{};

    // The first sixteen words are the block itself, big-endian.
    for (std::size_t index = 0; index < 16; ++index) {
        schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                          (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                          (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                          static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    // The remaining forty-eight are derived from them.
    for (std::size_t index = 16; index < 64; ++index) {
        const std::uint32_t s0 = rotate_right(schedule[index - 15], 7) ^
                                 rotate_right(schedule[index - 15], 18) ^
                                 (schedule[index - 15] >> 3);
        const std::uint32_t s1 = rotate_right(schedule[index - 2], 17) ^
                                 rotate_right(schedule[index - 2], 19) ^
                                 (schedule[index - 2] >> 10);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t index = 0; index < 64; ++index) {
        const std::uint32_t S1     = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t temp1  = h + S1 + choice + kRoundConstants[index] + schedule[index];
        const std::uint32_t S0     = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2    = S0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace detail

// An incremental hasher, so a large file can be hashed without being held in
// memory all at once.
class Hasher
{
public:
    void update(std::string_view data)
    {
        for (const char byte : data) {
            buffer_[buffered_++] = static_cast<std::uint8_t>(byte);
            if (buffered_ == buffer_.size()) {
                detail::compress(state_, buffer_.data());
                buffered_ = 0;
            }
        }
        length_ += data.size();
    }

    // Finish and return the digest. The hasher is left unusable afterwards
    // (calling update() again would produce nonsense), which is why this is
    // rvalue-friendly rather than resettable: one Hasher, one digest.
    Digest finish()
    {
        // Padding (FIPS 180-4 §5.1.1): append 0x80, then zeros, then the
        // message length in bits as a 64-bit big-endian integer, so that the
        // total is a multiple of 64 bytes.
        const std::uint64_t bit_length = static_cast<std::uint64_t>(length_) * 8;

        buffer_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            // No room for the length in this block: fill it and start another.
            while (buffered_ < buffer_.size())
                buffer_[buffered_++] = 0;
            detail::compress(state_, buffer_.data());
            buffered_ = 0;
        }
        while (buffered_ < 56)
            buffer_[buffered_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8)
            buffer_[buffered_++] = static_cast<std::uint8_t>((bit_length >> shift) & 0xFF);
        detail::compress(state_, buffer_.data());

        Digest digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            digest[word * 4]     = static_cast<std::uint8_t>((state_[word] >> 24) & 0xFF);
            digest[word * 4 + 1] = static_cast<std::uint8_t>((state_[word] >> 16) & 0xFF);
            digest[word * 4 + 2] = static_cast<std::uint8_t>((state_[word] >> 8) & 0xFF);
            digest[word * 4 + 3] = static_cast<std::uint8_t>(state_[word] & 0xFF);
        }
        return digest;
    }

private:
    std::array<std::uint32_t, 8> state_ = detail::kInitialState;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t                  buffered_ = 0;
    std::size_t                  length_   = 0;
};

// The digest of `data`, as 64 lowercase hex characters — the form a registry
// index stores and a person can compare by eye.
inline std::string hex(std::string_view data)
{
    Hasher hasher;
    hasher.update(data);
    const Digest digest = hasher.finish();

    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out;
    out.reserve(64);
    for (const std::uint8_t byte : digest) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

} // namespace sha256
} // namespace kap
