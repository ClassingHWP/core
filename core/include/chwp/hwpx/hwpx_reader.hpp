#pragma once

// HWPX (OWPML, KS X 6101) 파서. ZIP 컨테이너 + XML.

#include "chwp/common/bytes.hpp"
#include "chwp/parse_result.hpp"

namespace chwp::hwpx {

ParseResult parse(ByteView data, const ParseOptions& options = {});

}  // namespace chwp::hwpx
