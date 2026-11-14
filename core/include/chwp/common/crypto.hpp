#pragma once

// 문서 암호에 쓰는 작은 암호 함수들(AES·SHA-1·SHA-256·PBKDF2). 외부 라이브러리 없이 core 하나로 모든 플랫폼이 쓴다.

#include <array>
#include <cstdint>
#include <string_view>

#include "chwp/common/bytes.hpp"

namespace chwp::crypto {

using Sha1Digest = std::array<std::uint8_t, 20>;
using Sha256Digest = std::array<std::uint8_t, 32>;

Sha1Digest sha1(ByteView data);
Sha256Digest sha256(ByteView data);
Sha1Digest hmacSha1(ByteView key, ByteView data);
// PBKDF2-HMAC-SHA1 로 length 바이트 열쇠를 만든다.
Bytes pbkdf2Sha1(ByteView password, ByteView salt, std::uint32_t iterations, std::size_t length);

// AES 열쇠(16·24·32 바이트). 열쇠 길이가 맞지 않으면 ok() 가 거짓이다.
class Aes {
public:
    explicit Aes(ByteView key);
    [[nodiscard]] bool ok() const { return rounds_ != 0; }
    void encryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const;
    void decryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const;

private:
    std::array<std::uint8_t, 240> roundKeys_{};
    int rounds_ = 0;
};

// DES(8 바이트 열쇠, 각 바이트의 맨 아래 비트는 쓰지 않는다). 한글 HWP 문서 암호가 쓴다.
class Des {
public:
    explicit Des(ByteView key);
    [[nodiscard]] bool ok() const { return ok_; }
    void encryptBlock(const std::uint8_t in[8], std::uint8_t out[8]) const;
    void decryptBlock(const std::uint8_t in[8], std::uint8_t out[8]) const;

private:
    std::uint64_t process(std::uint64_t block, bool decrypt) const;
    std::array<std::uint64_t, 16> subKeys_{};
    bool ok_ = false;
};

// 8 바이트 단위만 다룬다. 남는 끝 조각은 버린다.
Bytes desEcbDecrypt(const Des& des, ByteView data);
Bytes desEcbEncrypt(const Des& des, ByteView data);

// 16 바이트 단위만 다룬다. 남는 끝 조각은 버린다(ECB·CBC 둘 다 채움은 부르는 쪽이 정한다).
Bytes aesEcbDecrypt(const Aes& aes, ByteView data);
Bytes aesEcbEncrypt(const Aes& aes, ByteView data);
Bytes aesCbcDecrypt(const Aes& aes, ByteView iv, ByteView data);
Bytes aesCbcEncrypt(const Aes& aes, ByteView iv, ByteView data);

// MS Visual C 의 srand()/rand() 와 같은 수열(한글 배포용 문서가 쓴다).
class MsvcRand {
public:
    explicit MsvcRand(std::uint32_t seed) : state_(seed) {}
    int next() {
        state_ = state_ * 214013u + 2531011u;
        return static_cast<int>((state_ >> 16) & 0x7FFF);
    }

private:
    std::uint32_t state_;
};

Bytes base64Decode(std::string_view text);
std::string base64Encode(ByteView data);

}  // namespace chwp::crypto
