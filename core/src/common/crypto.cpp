#include "chwp/common/crypto.hpp"

#include <cstring>

namespace chwp::crypto {

namespace {

// ---- SHA-1 / SHA-256

std::uint32_t rol(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
std::uint32_t ror(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// 64 바이트 블록으로 나눠 끝 채움(0x80, 0…, 비트 길이 64비트 빅 엔디언)까지 부른다.
template <typename Block>
void eachBlock(ByteView data, Block&& block) {
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    std::size_t at = 0;
    for (; at + 64 <= data.size(); at += 64) block(data.data() + at);
    std::uint8_t tail[128] = {};
    const std::size_t left = data.size() - at;
    if (left) std::memcpy(tail, data.data() + at, left);
    tail[left] = 0x80;
    const std::size_t total = left + 1 + 8 <= 64 ? 64 : 128;
    for (int i = 0; i < 8; ++i) tail[total - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    block(tail);
    if (total == 128) block(tail + 64);
}

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

void putBe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// ---- AES

struct AesTables {
    std::uint8_t sbox[256];
    std::uint8_t inv[256];
    AesTables() {
        // GF(2^8) 역원과 아핀 변환으로 S-box 를 만든다.
        std::uint8_t p = 1;
        std::uint8_t q = 1;
        do {
            p = static_cast<std::uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= static_cast<std::uint8_t>(q << 1);
            q ^= static_cast<std::uint8_t>(q << 2);
            q ^= static_cast<std::uint8_t>(q << 4);
            if (q & 0x80) q ^= 0x09;
            const auto rotl8 = [](std::uint8_t x, int s) { return static_cast<std::uint8_t>((x << s) | (x >> (8 - s))); };
            const std::uint8_t x = static_cast<std::uint8_t>(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
            sbox[p] = static_cast<std::uint8_t>(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[sbox[i]] = static_cast<std::uint8_t>(i);
    }
};

const AesTables& tables() {
    static const AesTables t;
    return t;
}

std::uint8_t xtime(std::uint8_t x) { return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

std::uint8_t mul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

}  // namespace

Sha1Digest sha1(ByteView data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    eachBlock(data, [&](const std::uint8_t* b) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = be32(b + i * 4);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) f = (bb & c) | (~bb & d), k = 0x5A827999;
            else if (i < 40) f = bb ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDC;
            else f = bb ^ c ^ d, k = 0xCA62C1D6;
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(bb, 30);
            bb = a;
            a = t;
        }
        h[0] += a;
        h[1] += bb;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    });
    Sha1Digest out{};
    for (int i = 0; i < 5; ++i) putBe32(out.data() + i * 4, h[i]);
    return out;
}

Sha256Digest sha256(ByteView data) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    eachBlock(data, [&](const std::uint8_t* b) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = be32(b + i * 4);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t v[8];
        std::memcpy(v, h, sizeof v);
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25);
            const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t t1 = v[7] + s1 + ch + kSha256K[i] + w[i];
            const std::uint32_t s0 = ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22);
            const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const std::uint32_t t2 = s0 + maj;
            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + t1;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) h[i] += v[i];
    });
    Sha256Digest out{};
    for (int i = 0; i < 8; ++i) putBe32(out.data() + i * 4, h[i]);
    return out;
}

Sha1Digest hmacSha1(ByteView key, ByteView data) {
    std::uint8_t k[64] = {};
    if (key.size() > 64) {
        const Sha1Digest d = sha1(key);
        std::memcpy(k, d.data(), d.size());
    } else if (!key.empty()) {
        std::memcpy(k, key.data(), key.size());
    }
    Bytes inner(64 + data.size());
    for (int i = 0; i < 64; ++i) inner[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(k[i] ^ 0x36);
    if (!data.empty()) std::memcpy(inner.data() + 64, data.data(), data.size());
    const Sha1Digest ih = sha1(inner);
    std::uint8_t outer[64 + 20];
    for (int i = 0; i < 64; ++i) outer[i] = static_cast<std::uint8_t>(k[i] ^ 0x5C);
    std::memcpy(outer + 64, ih.data(), ih.size());
    return sha1(ByteView(outer, sizeof outer));
}

Bytes pbkdf2Sha1(ByteView password, ByteView salt, std::uint32_t iterations, std::size_t length) {
    Bytes out;
    out.reserve(length);
    for (std::uint32_t block = 1; out.size() < length; ++block) {
        Bytes s(salt.begin(), salt.end());
        s.push_back(static_cast<std::uint8_t>(block >> 24));
        s.push_back(static_cast<std::uint8_t>(block >> 16));
        s.push_back(static_cast<std::uint8_t>(block >> 8));
        s.push_back(static_cast<std::uint8_t>(block));
        Sha1Digest u = hmacSha1(password, s);
        Sha1Digest t = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            u = hmacSha1(password, u);
            for (std::size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        for (std::size_t j = 0; j < t.size() && out.size() < length; ++j) out.push_back(t[j]);
    }
    return out;
}

Aes::Aes(ByteView key) {
    const int nk = static_cast<int>(key.size() / 4);
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) return;
    rounds_ = nk + 6;
    const AesTables& t = tables();
    const int total = 4 * (rounds_ + 1);
    std::uint8_t* w = roundKeys_.data();
    std::memcpy(w, key.data(), key.size());
    std::uint8_t rcon = 1;
    for (int i = nk; i < total; ++i) {
        std::uint8_t tmp[4];
        std::memcpy(tmp, w + (i - 1) * 4, 4);
        if (i % nk == 0) {
            const std::uint8_t first = tmp[0];
            tmp[0] = static_cast<std::uint8_t>(t.sbox[tmp[1]] ^ rcon);
            tmp[1] = t.sbox[tmp[2]];
            tmp[2] = t.sbox[tmp[3]];
            tmp[3] = t.sbox[first];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (std::uint8_t& b : tmp) b = t.sbox[b];
        }
        for (int j = 0; j < 4; ++j) w[i * 4 + j] = static_cast<std::uint8_t>(w[(i - nk) * 4 + j] ^ tmp[j]);
    }
}

void Aes::encryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const AesTables& t = tables();
    std::uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = static_cast<std::uint8_t>(in[i] ^ roundKeys_[static_cast<std::size_t>(i)]);
    for (int round = 1; round <= rounds_; ++round) {
        std::uint8_t r[16];
        // SubBytes + ShiftRows
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) r[c * 4 + row] = t.sbox[s[((c + row) % 4) * 4 + row]];
        }
        if (round != rounds_) {
            for (int c = 0; c < 4; ++c) {
                std::uint8_t* col = r + c * 4;
                const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                const std::uint8_t all = static_cast<std::uint8_t>(a0 ^ a1 ^ a2 ^ a3);
                col[0] = static_cast<std::uint8_t>(a0 ^ all ^ xtime(static_cast<std::uint8_t>(a0 ^ a1)));
                col[1] = static_cast<std::uint8_t>(a1 ^ all ^ xtime(static_cast<std::uint8_t>(a1 ^ a2)));
                col[2] = static_cast<std::uint8_t>(a2 ^ all ^ xtime(static_cast<std::uint8_t>(a2 ^ a3)));
                col[3] = static_cast<std::uint8_t>(a3 ^ all ^ xtime(static_cast<std::uint8_t>(a3 ^ a0)));
            }
        }
        const std::uint8_t* k = roundKeys_.data() + round * 16;
        for (int i = 0; i < 16; ++i) s[i] = static_cast<std::uint8_t>(r[i] ^ k[i]);
    }
    std::memcpy(out, s, 16);
}

void Aes::decryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const AesTables& t = tables();
    std::uint8_t s[16];
    const std::uint8_t* last = roundKeys_.data() + rounds_ * 16;
    for (int i = 0; i < 16; ++i) s[i] = static_cast<std::uint8_t>(in[i] ^ last[i]);
    for (int round = rounds_ - 1; round >= 0; --round) {
        std::uint8_t r[16];
        // InvShiftRows + InvSubBytes
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) r[((c + row) % 4) * 4 + row] = t.inv[s[c * 4 + row]];
        }
        const std::uint8_t* k = roundKeys_.data() + round * 16;
        for (int i = 0; i < 16; ++i) r[i] = static_cast<std::uint8_t>(r[i] ^ k[i]);
        if (round != 0) {
            for (int c = 0; c < 4; ++c) {
                std::uint8_t* col = r + c * 4;
                const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = static_cast<std::uint8_t>(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                col[1] = static_cast<std::uint8_t>(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                col[2] = static_cast<std::uint8_t>(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                col[3] = static_cast<std::uint8_t>(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
            }
        }
        std::memcpy(s, r, 16);
    }
    std::memcpy(out, s, 16);
}

Bytes aesEcbDecrypt(const Aes& aes, ByteView data) {
    Bytes out(data.size() / 16 * 16);
    for (std::size_t at = 0; at < out.size(); at += 16) aes.decryptBlock(data.data() + at, out.data() + at);
    return out;
}

Bytes aesEcbEncrypt(const Aes& aes, ByteView data) {
    Bytes out(data.size() / 16 * 16);
    for (std::size_t at = 0; at < out.size(); at += 16) aes.encryptBlock(data.data() + at, out.data() + at);
    return out;
}

Bytes aesCbcDecrypt(const Aes& aes, ByteView iv, ByteView data) {
    Bytes out(data.size() / 16 * 16);
    std::uint8_t prev[16] = {};
    if (iv.size() >= 16) std::memcpy(prev, iv.data(), 16);
    for (std::size_t at = 0; at < out.size(); at += 16) {
        aes.decryptBlock(data.data() + at, out.data() + at);
        for (int i = 0; i < 16; ++i) out[at + static_cast<std::size_t>(i)] ^= prev[i];
        std::memcpy(prev, data.data() + at, 16);
    }
    return out;
}

Bytes aesCbcEncrypt(const Aes& aes, ByteView iv, ByteView data) {
    Bytes out(data.size() / 16 * 16);
    std::uint8_t prev[16] = {};
    if (iv.size() >= 16) std::memcpy(prev, iv.data(), 16);
    for (std::size_t at = 0; at < out.size(); at += 16) {
        std::uint8_t block[16];
        for (int i = 0; i < 16; ++i) block[i] = static_cast<std::uint8_t>(data[at + static_cast<std::size_t>(i)] ^ prev[i]);
        aes.encryptBlock(block, out.data() + at);
        std::memcpy(prev, out.data() + at, 16);
    }
    return out;
}

// ---- DES (FIPS 46-3 표). 비트 번호는 표준처럼 왼쪽(최상위)부터 1 이다.

namespace {

constexpr std::uint8_t kIp[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                                  62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                  57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                                  61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
constexpr std::uint8_t kFp[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                                  38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                  36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                                  34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
constexpr std::uint8_t kE[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                                 12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                                 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
constexpr std::uint8_t kP[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                                 2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
constexpr std::uint8_t kPc1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
                                   10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
                                   63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
                                   14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
constexpr std::uint8_t kPc2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                   26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                   51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
constexpr std::uint8_t kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
constexpr std::uint8_t kS[8][64] = {
    {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11,
     9, 5, 3, 8, 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11,
     3, 14, 10, 0, 6, 13},
    {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10,
     6, 9, 11, 5, 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11,
     6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14,
     12, 11, 15, 1, 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4,
     15, 14, 3, 11, 5, 2, 12},
    {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12,
     1, 10, 14, 9, 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9,
     4, 5, 11, 12, 7, 2, 14},
    {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10,
     3, 9, 8, 6, 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6,
     15, 0, 9, 10, 4, 5, 3},
    {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14,
     0, 11, 3, 8, 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11,
     14, 1, 7, 6, 0, 8, 13},
    {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12,
     2, 15, 8, 6, 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9,
     5, 0, 15, 14, 2, 3, 12},
    {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11,
     0, 14, 9, 2, 7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15,
     12, 9, 0, 3, 5, 6, 11}};

// width 비트 값 in 에서 표가 가리키는 비트(왼쪽부터 1)를 차례로 모은다.
std::uint64_t permute(std::uint64_t in, int width, const std::uint8_t* table, int count) {
    std::uint64_t out = 0;
    for (int i = 0; i < count; ++i) out = (out << 1) | ((in >> (width - table[i])) & 1u);
    return out;
}

std::uint32_t rotl28(std::uint32_t v, int n) { return ((v << n) | (v >> (28 - n))) & 0x0FFFFFFFu; }

}  // namespace

Des::Des(ByteView key) {
    if (key.size() != 8) return;
    std::uint64_t k = 0;
    for (const std::uint8_t b : key) k = (k << 8) | b;
    const std::uint64_t cd = permute(k, 64, kPc1, 56);
    std::uint32_t c = static_cast<std::uint32_t>(cd >> 28) & 0x0FFFFFFFu;
    std::uint32_t d = static_cast<std::uint32_t>(cd) & 0x0FFFFFFFu;
    for (int i = 0; i < 16; ++i) {
        c = rotl28(c, kShifts[i]);
        d = rotl28(d, kShifts[i]);
        subKeys_[static_cast<std::size_t>(i)] = permute((static_cast<std::uint64_t>(c) << 28) | d, 56, kPc2, 48);
    }
    ok_ = true;
}

std::uint64_t Des::process(std::uint64_t block, bool decrypt) const {
    const std::uint64_t ip = permute(block, 64, kIp, 64);
    std::uint32_t l = static_cast<std::uint32_t>(ip >> 32);
    std::uint32_t r = static_cast<std::uint32_t>(ip);
    for (int round = 0; round < 16; ++round) {
        const std::uint64_t key = subKeys_[static_cast<std::size_t>(decrypt ? 15 - round : round)];
        const std::uint64_t x = permute(r, 32, kE, 48) ^ key;
        std::uint32_t s = 0;
        for (int box = 0; box < 8; ++box) {
            const int six = static_cast<int>((x >> (42 - 6 * box)) & 0x3F);
            const int row = ((six & 0x20) >> 4) | (six & 1);
            const int col = (six >> 1) & 0x0F;
            s = (s << 4) | kS[box][row * 16 + col];
        }
        const std::uint32_t f = static_cast<std::uint32_t>(permute(s, 32, kP, 32));
        const std::uint32_t next = l ^ f;
        l = r;
        r = next;
    }
    return permute((static_cast<std::uint64_t>(r) << 32) | l, 64, kFp, 64);
}

void Des::encryptBlock(const std::uint8_t in[8], std::uint8_t out[8]) const {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | in[i];
    v = process(v, false);
    for (int i = 7; i >= 0; --i, v >>= 8) out[i] = static_cast<std::uint8_t>(v);
}

void Des::decryptBlock(const std::uint8_t in[8], std::uint8_t out[8]) const {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | in[i];
    v = process(v, true);
    for (int i = 7; i >= 0; --i, v >>= 8) out[i] = static_cast<std::uint8_t>(v);
}

Bytes desEcbDecrypt(const Des& des, ByteView data) {
    Bytes out(data.size() / 8 * 8);
    for (std::size_t at = 0; at < out.size(); at += 8) des.decryptBlock(data.data() + at, out.data() + at);
    return out;
}

Bytes desEcbEncrypt(const Des& des, ByteView data) {
    Bytes out(data.size() / 8 * 8);
    for (std::size_t at = 0; at < out.size(); at += 8) des.encryptBlock(data.data() + at, out.data() + at);
    return out;
}

Bytes base64Decode(std::string_view text) {
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char ch : text) {
        int v = -1;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else if (ch == '=') break;
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(acc >> bits));
        }
    }
    return out;
}

std::string base64Encode(ByteView data) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i < data.size()) {
        std::uint32_t v = std::uint32_t{data[i]} << 16;
        if (i + 1 < data.size()) v |= std::uint32_t{data[i + 1]} << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < data.size() ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace chwp::crypto
