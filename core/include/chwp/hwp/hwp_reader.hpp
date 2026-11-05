#pragma once

// HWP 5.0 (CFB 바이너리) 파서.
// 규격: 한컴 「한글 문서 파일 형식 5.0」

#include "chwp/common/bytes.hpp"
#include "chwp/parse_result.hpp"

namespace chwp::hwp {

ParseResult parse(ByteView data, const ParseOptions& options = {});

}  // namespace chwp::hwp
