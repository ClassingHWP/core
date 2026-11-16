#pragma once

// model::Document 를 HWP 5.0 (MS-CFB OLE2 컨테이너 + 바이너리 레코드 스트림) 바이트로 직렬화한다.

#include <string>

#include "chwp/common/bytes.hpp"
#include "chwp/model/document.hpp"

namespace chwp::hwp {

struct WriteOptions {
    // BinItem::data 가 비어 있을 때 원본 컨테이너(HWP 또는 HWPX)에서 바이너리를 추출하기 위한 원본 바이트.
    ByteView sourceData{};
    // DocInfo / BodyText / BinData 스트림을 raw deflate 로 압축할지 여부 (기본: true).
    bool compress = true;
    // 첫 쪽 미리 보기 그림(PNG). 비어 있지 않으면 PrvImage 스트림으로 넣는다(한글이 문서 탐색기 미리 보기에 쓴다).
    Bytes previewPng;
};

bool write(const model::Document& doc, Bytes& out, const WriteOptions& options = {},
           std::string* error = nullptr);

}  // namespace chwp::hwp
