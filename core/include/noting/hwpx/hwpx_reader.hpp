#pragma once

// HWPX (OWPML, KS X 6101) 파서. ZIP 컨테이너 + XML.

#include "noting/common/bytes.hpp"
#include "noting/parse_result.hpp"

namespace noting::hwpx {

ParseResult parse(ByteView data, const ParseOptions& options = {});

}  // namespace noting::hwpx
