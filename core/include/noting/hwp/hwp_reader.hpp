#pragma once

// HWP 5.0 (CFB 바이너리) 파서.
// 규격: 한컴 「한글 문서 파일 형식 5.0」

#include "noting/common/bytes.hpp"
#include "noting/parse_result.hpp"

namespace noting::hwp {

ParseResult parse(ByteView data, const ParseOptions& options = {});

}  // namespace noting::hwp
