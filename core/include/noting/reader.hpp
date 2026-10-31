#pragma once

#include "noting/common/bytes.hpp"
#include "noting/parse_result.hpp"

namespace noting {

// 시그니처로 형식을 판별한다 (CFB → HWP, ZIP → HWPX).
model::SourceFormat detectFormat(ByteView data);

// 형식을 자동 판별해 파싱한다. 파일 입출력은 하지 않으므로 호출 측이 바이트를 넘긴다.
ParseResult parseDocument(ByteView data, const ParseOptions& options = {});

const char* formatName(model::SourceFormat format);

// 컨테이너에서 그림 바이트 하나를 읽는다. HWP 는 스트림을 풀고, HWPX 는 zip 항목을 읽는다.
bool readEmbeddedBin(ByteView file, const model::Document& doc, const model::BinItem& item, Bytes& out);
void releaseEmbeddedBinCache();

}  // namespace noting
