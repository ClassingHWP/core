#pragma once

#include <string>
#include <vector>

#include "noting/model/document.hpp"

namespace noting {

struct ParseOptions {
    bool loadBinData = false;  // 그림 등 바이너리 데이터를 BinItem::data 에 싣는다
    bool keepLineSegs = true;  // 조판 캐시(LineSeg) 보관
};

// 예외를 쓰지 않는다 (Emscripten 기본 설정에서 예외를 잡을 수 없음).
// ok 가 false 면 error 에 사유가 있고, document 는 파싱된 부분까지만 담긴다.
struct ParseResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    model::Document document;
};

}  // namespace noting
