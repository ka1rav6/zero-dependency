// tests/test_sha256.cpp
//
// Unit tests for core/sha256.hpp.
//
// A hash implementation is either exactly right or completely wrong, and it is
// very easy to write one that is wrong only for inputs of certain lengths —
// the padding rules in FIPS 180-4 §5.1.1 have two branches and the second one
// (a message that leaves no room for the length field in its final block) is
// the one people forget. Every test below is a published test vector or a
// length that exercises a specific padding boundary.

#include "core/sha256.hpp"
#include "harness.hpp"

#include <string>

KAP_TEST("sha256 matches the FIPS 180-4 test vectors")
{
    // The two single-block examples from the specification's appendix.
    KAP_ASSERT_EQ(kap::sha256::hex("abc"),
                  std::string("ba7816bf8f01cfea414140de5dae2223"
                              "b00361a396177a9cb410ff61f20015ad"));
    KAP_ASSERT_EQ(kap::sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                  std::string("248d6a61d20638b8e5c026930c3e6039"
                              "a33ce45964ff2167f6ecedd419db06c1"));
});

KAP_TEST("sha256 of the empty string is the published value")
{
    // The easiest case to get wrong: with no data at all, the padding block is
    // the *entire* message.
    KAP_ASSERT_EQ(kap::sha256::hex(""),
                  std::string("e3b0c44298fc1c149afbf4c8996fb924"
                              "27ae41e4649b934ca495991b7852b855"));
});

KAP_TEST("sha256 handles the padding boundaries around 55, 56, and 64 bytes")
{
    // 55 bytes: the length field just fits in the same block.
    // 56 bytes: it does not, so a second block is needed — the branch most
    //           implementations get wrong.
    // 64 bytes: exactly one full block plus a whole block of padding.
    KAP_ASSERT_EQ(kap::sha256::hex(std::string(55, 'a')),
                  std::string("9f4390f8d30c2dd92ec9f095b65e2b9a"
                              "e9b0a925a5258e241c9f1e910f734318"));
    KAP_ASSERT_EQ(kap::sha256::hex(std::string(56, 'a')),
                  std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f"
                              "590ce20f1bde7090ef7970686ec6738a"));
    KAP_ASSERT_EQ(kap::sha256::hex(std::string(64, 'a')),
                  std::string("ffe054fe7ae0cb6dc65c3af9b61d5209"
                              "f439851db43d0ba5997337df154668eb"));
});

KAP_TEST("sha256 of a million 'a' characters matches the long-message vector")
{
    // The specification's third example. Also the only test here that spans
    // many blocks, which is what catches an error in the message-schedule
    // carry between them.
    KAP_ASSERT_EQ(kap::sha256::hex(std::string(1000000, 'a')),
                  std::string("cdc76e5c9914fb9281a1c7e284d73e67"
                              "f1809a48a497200e046d39ccc7112cd0"));
});

KAP_TEST("an incremental hash equals a one-shot hash of the same bytes")
{
    // The install pipeline hashes a payload it reads in pieces, so the two
    // paths must agree exactly.
    const std::string whole =
        "the quick brown fox jumps over the lazy dog, repeatedly and at length";

    kap::sha256::Hasher hasher;
    for (std::size_t offset = 0; offset < whole.size(); offset += 7)
        hasher.update(std::string_view(whole).substr(offset, 7));
    const kap::sha256::Digest incremental = hasher.finish();

    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           rendered;
    for (const std::uint8_t byte : incremental) {
        rendered.push_back(kDigits[byte >> 4]);
        rendered.push_back(kDigits[byte & 0x0F]);
    }
    KAP_ASSERT_EQ(rendered, kap::sha256::hex(whole));
});

KAP_TEST("a one-bit difference changes the digest completely")
{
    // The property the checksum check depends on: a tampered payload cannot
    // keep its digest.
    const std::string a = kap::sha256::hex("plugin payload v1");
    const std::string b = kap::sha256::hex("plugin payload v2");
    KAP_ASSERT_NE(a, b);
    KAP_ASSERT_EQ(a.size(), static_cast<std::size_t>(64));
});

KAP_TEST("sha256 hashes bytes, not text, so embedded NULs count")
{
    const std::string with_nul("a\0b", 3);
    KAP_ASSERT_EQ(with_nul.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_NE(kap::sha256::hex(with_nul), kap::sha256::hex("ab"));
});
