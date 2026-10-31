#pragma once

// model::Document 를 HWPX (OWPML ZIP 패키지) 바이트로 직렬화한다.

#include <string>

#include "noting/common/bytes.hpp"
#include "noting/model/document.hpp"

namespace noting::hwpx {

struct WriteOptions {
    // BinItem::data 가 비어 있을 때 원본 컨테이너(HWP 또는 HWPX)에서 바이너리를 추출하기 위한 원본 바이트.
    ByteView sourceData{};
};

bool write(const model::Document& doc, Bytes& out, const WriteOptions& options = {},
           std::string* error = nullptr);

}  // namespace noting::hwpx
