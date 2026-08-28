#pragma once

// core/hash.hpp
//
// FNV-1a, the non-cryptographic hash design doc §9 nominates for cache keys
// ("In-tree FNV-1a or similar. No OpenSSL requirement").
//
// Two callers: the KPL AST cache keys its files on the plugin's absolute
// source path (§5.14), and the detection cache will key on the matched marker
// files (§3.2 step 5). Neither is a security boundary — a cache key only has
// to be stable and collision-resistant enough that two different inputs
// practically never share a file name. If either ever becomes a trust
// decision, this is the wrong function and the comment should stop you.
//
// FNV-1a is chosen over anything larger because it is eight lines, has no
// tables, and produces identical output on every platform and compiler, which
// is what makes a cache file written by one build readable by the next.

#include <cstdint>
#include <string>
#include <string_view>

namespace kap
{
namespace hash
{

// The 64-bit FNV-1a parameters, from the reference specification.
inline constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnvPrime       = 1099511628211ULL;

// Hash `data`. Deterministic across runs and platforms: no seeding, no
// address-dependent state.
inline std::uint64_t fnv1a64(std::string_view data)
{
    std::uint64_t value = kFnvOffsetBasis;
    for (const char byte : data) {
        // XOR *then* multiply — that ordering is what distinguishes FNV-1a
        // from FNV-1 and gives it its better avalanche behaviour.
        value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        value *= kFnvPrime;
    }
    return value;
}

// The same hash rendered as 16 lowercase hex digits, for use in a file name.
// Fixed width, so cache file names sort and align predictably.
inline std::string hex64(std::uint64_t value)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out(16, '0');
    for (int index = 15; index >= 0; --index) {
        out[static_cast<std::size_t>(index)] = kDigits[value & 0xF];
        value >>= 4;
    }
    return out;
}

} // namespace hash
} // namespace kap
