// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/secret_store.h"

#include "core/platform.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Half the key is fixed and half comes from a file written once per install,
// so a state file lifted onto another machine decrypts to nothing.
constexpr unsigned char kSalt[16] = {
    0x42, 0x75, 0x74, 0x74, 0x65, 0x72, 0x61, 0x6E,
    0x64, 0x4A, 0x65, 0x6C, 0x6C, 0x79, 0x21, 0x00
};

std::string KeyPath() { return Platform::DataDir() + "/.keyfile"; }

// Sixteen bytes, generated on first use. Losing it means the stored tokens
// stop decrypting, which costs a sign-in and nothing more.
const unsigned char* InstallKey()
{
    static unsigned char key[16];
    static bool loaded = false;
    if (loaded) return key;
    loaded = true;

    FILE* fp = std::fopen(Platform::NativePath(KeyPath()).c_str(), "rb");
    if (fp) {
        const size_t got = std::fread(key, 1, sizeof(key), fp);
        std::fclose(fp);
        if (got == sizeof(key)) return key;
    }

    // Not a cryptographic source, and it does not need to be: this only has
    // to differ between installs.
    uint64_t seed = Platform::NowMs();
    seed ^= (uint64_t)(uintptr_t)&key;
    for (size_t i = 0; i < sizeof(key); ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        key[i] = (unsigned char)((seed >> 33) & 0xFF);
    }

    fp = std::fopen(Platform::NativePath(KeyPath()).c_str(), "wb");
    if (fp) { std::fwrite(key, 1, sizeof(key), fp); std::fclose(fp); }
    return key;
}

void MixedKey(unsigned char out[16])
{
    const unsigned char* install = InstallKey();
    for (int i = 0; i < 16; ++i) out[i] = (unsigned char)(kSalt[i] ^ install[i]);
}

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::vector<unsigned char>& in)
{
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned a = in[i];
        const unsigned b = (i + 1 < in.size()) ? in[i + 1] : 0;
        const unsigned c = (i + 2 < in.size()) ? in[i + 2] : 0;
        const unsigned v = (a << 16) | (b << 8) | c;
        out += kB64[(v >> 18) & 0x3F];
        out += kB64[(v >> 12) & 0x3F];
        out += (i + 1 < in.size()) ? kB64[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < in.size()) ? kB64[v & 0x3F]        : '=';
    }
    return out;
}

int B64Value(char ch)
{
    for (int i = 0; i < 64; ++i) if (kB64[i] == ch) return i;
    return -1;
}

std::vector<unsigned char> Base64Decode(const std::string& in)
{
    std::vector<unsigned char> out;
    int bits = 0, accumulated = 0;
    for (char ch : in) {
        if (ch == '=' || ch == '\n' || ch == '\r') continue;
        const int value = B64Value(ch);
        if (value < 0) return std::vector<unsigned char>();
        accumulated = (accumulated << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((accumulated >> bits) & 0xFF));
        }
    }
    return out;
}

// A keyed stream XORed over the bytes. Not a cipher anyone should trust with
// anything that matters, and not meant to be: the key ships with the binary,
// so this only has to stop a token being readable at a glance.
void Keystream(std::vector<unsigned char>& buffer)
{
    unsigned char key[16];
    MixedKey(key);

    uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 16; ++i) {
        state ^= (uint64_t)key[i] << ((i % 8) * 8);
        state *= 0xFF51AFD7ED558CCDULL;
        state ^= state >> 33;
    }

    for (size_t i = 0; i < buffer.size(); ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        buffer[i] = (unsigned char)(buffer[i] ^ (unsigned char)(state & 0xFF));
    }
}

}  // namespace

namespace Secret {

std::string Hide(const std::string& plain)
{
    if (plain.empty()) return "";

    std::vector<unsigned char> buffer(plain.begin(), plain.end());
    Keystream(buffer);
    return Base64Encode(buffer);
}

std::string Show(const std::string& hidden)
{
    if (hidden.empty()) return "";

    std::vector<unsigned char> buffer = Base64Decode(hidden);
    if (buffer.empty()) return "";

    Keystream(buffer);   // same stream both ways
    return std::string(buffer.begin(), buffer.end());
}

}  // namespace Secret
