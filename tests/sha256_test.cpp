// Tests gc::sha256 (src/sha256.h) against the FIPS 180-4 examples and against Windows CNG.
#include "../src/sha256.h"
#include "../src/runtime_crypto.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"

// Lowercase hex of a digest.
static std::string hex(const std::array<unsigned char, 32>& d) { return gc::request_hex(d.data(), 32); }

// The same digest from Windows CNG, for comparison.
static std::array<unsigned char, 32> cng(const unsigned char* data, size_t size) {
    std::array<unsigned char, 32> out{};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    if (ok && size) ok = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0;
    if (ok) ok = BCryptFinishHash(hash, out.data(), 32, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) ++failures;
    return out;
}

int main() {
    // FIPS 180-4 examples: the empty message, "abc" and a two-block message.
    CHECK(hex(gc::sha256("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(gc::sha256("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(gc::sha256(two, strlen(two))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A million 'a', fed 1000 bytes at a time.
    gc::Sha256 million;
    const std::string thousand(1000, 'a');
    for (int i = 0; i < 1000; ++i) million.update(thousand.data(), thousand.size());
    CHECK(hex(million.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // A finished hasher starts over.
    million.update("abc", 3);
    CHECK(hex(million.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Lengths 0 to 300 against CNG, in one piece and, up to 140 bytes, split in two at every point.
    std::vector<unsigned char> data(300);
    unsigned seed = 12345;
    for (auto& b : data) {
        seed = seed * 1103515245u + 12345u;
        b = static_cast<unsigned char>(seed >> 16);
    }
    for (size_t size = 0; size <= data.size(); ++size) {
        const auto expected = cng(data.data(), size);
        CHECK(gc::sha256(data.data(), size) == expected);
        if (size <= 140) {
            for (size_t cut = 0; cut <= size; ++cut) {
                gc::Sha256 split;
                split.update(data.data(), cut);
                split.update(data.data() + cut, size - cut);
                CHECK(split.finish() == expected);
            }
        }
    }
    // request_hash returns true and the same digest.
    std::array<unsigned char, 32> digest{};
    CHECK(gc::request_hash(reinterpret_cast<const unsigned char*>("abc"), 3, digest) &&
          hex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    if (failures) {
        std::printf("sha256_test FAILED (%d)\n", failures);
        return 1;
    }
    std::puts("sha256_test PASS: FIPS vectors, reuse after finish, split updates and CNG agreement");
    return 0;
}
