#pragma once

#include <string>

#include "chwp/common/bytes.hpp"
#include "chwp/model/document.hpp"

namespace chwp {

struct ExportOptions {
    model::SourceFormat format = model::SourceFormat::Hwpx;
    // 원본 파일 바이트 (BinItem.data가 비어 있을 때 원본 컨테이너에서 바이너리 추출용)
    ByteView sourceData{};
    // HWP 5.0 내보내기 시 DocInfo/Section/BinData 스트림 압축 여부
    bool compress = true;
};

struct ExportResult {
    bool ok = false;
    Bytes data;
    std::string error;
};

// model::Document 를 HWP 5.0 또는 HWPX 바이트로 직렬화한다.
ExportResult writeDocument(const model::Document& doc, const ExportOptions& options = {});

bool writeDocument(const model::Document& doc, model::SourceFormat format, Bytes& out,
                   ByteView sourceData = {}, std::string* error = nullptr);

}  // namespace chwp
