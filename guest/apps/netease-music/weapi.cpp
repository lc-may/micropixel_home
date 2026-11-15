// SPDX-License-Identifier: Apache-2.0
// Protocol adapted from api-enhanced (MIT); see THIRD_PARTY_NOTICES.md.
#include "weapi.hpp"

#include <vector>
namespace netease {
namespace {
uint8_t Mul(uint8_t a, uint8_t b) {
    uint8_t out = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (b & 1) out ^= a;
        bool top = a & 128;
        a <<= 1;
        if (top) a ^= 0x1b;
        b >>= 1;
    }
    return out;
}
uint8_t Sbox(uint8_t a) {
    // Inversion in GF(2^8), followed by the AES affine transform.
    uint8_t inverse = 1, base = a;
    unsigned exponent = 254;
    while (exponent) {
        if (exponent & 1) inverse = Mul(inverse, base);
        base = Mul(base, base);
        exponent >>= 1;
    }
    auto rotate = [](uint8_t x, unsigned n) { return uint8_t((x << n) | (x >> (8 - n))); };
    return inverse ^ rotate(inverse, 1) ^ rotate(inverse, 2) ^ rotate(inverse, 3) ^ rotate(inverse, 4) ^ 0x63;
}
using Big = std::array<uint32_t, 32>;
// Public modulus; no private keys or account tokens are embedded.
constexpr Big modulus = {0x0a22b8e7U, 0xce0462dbU, 0xc6935b3eU, 0x6b8e289dU, 0x52741d54U, 0x7b97ddefU, 0x5d3e8204U,
                         0x13cfe487U, 0x10b424d8U, 0x32575cceU, 0x6bee2559U, 0x3685b7a4U, 0x5c4f7f0cU, 0x4af6c9d0U,
                         0xc9387011U, 0xbda92557U, 0x4e0312ecU, 0x69528010U, 0x6135fccfU, 0x4ee341f5U, 0x417629ecU,
                         0x5aa76d2eU, 0x876aea8aU, 0x2b3ab17aU, 0xb7b72515U, 0x8ace615bU, 0x152b5ff6U, 0xf22677ecU,
                         0x2901477dU, 0x2dbc3566U, 0x259df864U, 0xe0b509f6U};
bool Greater(const Big& a, const Big& b) {
    for (int i = 31; i >= 0; --i)
        if (a[i] != b[i]) return a[i] > b[i];
    return true;
}
Big Add(const Big& a, const Big& b) {
    Big out{};
    uint64_t carry = 0;
    for (unsigned i = 0; i < 32; ++i) {
        carry += uint64_t(a[i]) + b[i];
        out[i] = carry;
        carry >>= 32;
    }
    if (carry || Greater(out, modulus)) {
        uint64_t borrow = 0;
        for (unsigned i = 0; i < 32; ++i) {
            uint64_t rhs = uint64_t(modulus[i]) + borrow;
            uint64_t lhs = out[i];
            out[i] = lhs - rhs;
            borrow = lhs < rhs;
        }
    }
    return out;
}
Big Product(Big a, const Big& b) {
    Big out{};
    for (unsigned i = 0; i < 1024; ++i) {
        if ((b[i / 32] >> (i % 32)) & 1) out = Add(out, a);
        a = Add(a, a);
    }
    return out;
}
std::string Encode(std::string_view s) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
}  // namespace
std::string AesBase64(std::string_view text, const std::array<uint8_t, 16>& key) {
    std::array<uint8_t, 256> box{};
    for (unsigned i = 0; i < 256; ++i) box[i] = Sbox(i);
    std::array<uint8_t, 176> expanded{};
    for (unsigned i = 0; i < 16; ++i) expanded[i] = key[i];
    uint8_t rc = 1;
    for (unsigned i = 16; i < 176; i += 4) {
        std::array<uint8_t, 4> t{};
        for (unsigned j = 0; j < 4; ++j) t[j] = expanded[i - 4 + j];
        if (i % 16 == 0) {
            auto a = t[0];
            t[0] = box[t[1]] ^ rc;
            t[1] = box[t[2]];
            t[2] = box[t[3]];
            t[3] = box[a];
            rc = Mul(rc, 2);
        }
        for (unsigned j = 0; j < 4; ++j) expanded[i + j] = expanded[i - 16 + j] ^ t[j];
    }
    std::vector<uint8_t> bytes(text.begin(), text.end());
    uint8_t padding = 16 - bytes.size() % 16;
    bytes.insert(bytes.end(), padding, padding);
    std::array<uint8_t, 16> chain{'0', '1', '0', '2', '0', '3', '0', '4', '0', '5', '0', '6', '0', '7', '0', '8'};
    for (size_t at = 0; at < bytes.size(); at += 16) {
        std::array<uint8_t, 16> state{};
        for (unsigned i = 0; i < 16; ++i) state[i] = bytes[at + i] ^ chain[i] ^ expanded[i];
        for (unsigned round = 1; round <= 10; ++round) {
            auto before = state;
            for (unsigned col = 0; col < 4; ++col)
                for (unsigned row = 0; row < 4; ++row) state[col * 4 + row] = box[before[((col + row) % 4) * 4 + row]];
            if (round != 10)
                for (unsigned col = 0; col < 4; ++col) {
                    auto p = col * 4;
                    auto a = state[p], b = state[p + 1], c = state[p + 2], d = state[p + 3];
                    state[p] = Mul(a, 2) ^ Mul(b, 3) ^ c ^ d;
                    state[p + 1] = a ^ Mul(b, 2) ^ Mul(c, 3) ^ d;
                    state[p + 2] = a ^ b ^ Mul(c, 2) ^ Mul(d, 3);
                    state[p + 3] = Mul(a, 3) ^ b ^ c ^ Mul(d, 2);
                }
            for (unsigned i = 0; i < 16; ++i) state[i] ^= expanded[round * 16 + i];
        }
        for (unsigned i = 0; i < 16; ++i) chain[i] = bytes[at + i] = state[i];
    }
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t n = uint32_t(bytes[i]) << 16;
        bool b = i + 1 < bytes.size(), c = i + 2 < bytes.size();
        if (b) n |= uint32_t(bytes[i + 1]) << 8;
        if (c) n |= bytes[i + 2];
        out += alphabet[n >> 18];
        out += alphabet[(n >> 12) & 63];
        out += b ? alphabet[(n >> 6) & 63] : '=';
        out += c ? alphabet[n & 63] : '=';
    }
    return out;
}
std::string RsaSecret(const std::array<uint8_t, 16>& key) {
    Big base{};
    for (unsigned i = 0; i < 16; ++i) base[i / 4] |= uint32_t(key[i]) << (8 * (i % 4));
    auto result = base;
    for (unsigned i = 0; i < 16; ++i) result = Product(result, result);
    result = Product(result, base);
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (int i = 127; i >= 0; --i) {
        uint8_t c = result[i / 4] >> (8 * (i % 4));
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}
std::string Weapi(std::string_view json, micropixel::Random random) {
    std::array<uint8_t, 16> key{};
    constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (auto& c : key) c = alphabet[random.Below(62U)];
    constexpr std::array<uint8_t, 16> first{'0', 'C', 'o', 'J', 'U', 'm', '6', 'Q',
                                            'y', 'w', '8', 'W', '8', 'j', 'u', 'd'};
    auto params = AesBase64(AesBase64(json, first), key);
    return "params=" + Encode(params) + "&encSecKey=" + RsaSecret(key);
}
}  // namespace netease
