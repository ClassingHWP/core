#pragma once

#include <string>
#include <vector>

#include "chwp/model/document.hpp"

namespace chwp {

struct ParseOptions {
    bool loadBinData = false;  // 그림 등 바이너리 데이터를 BinItem::data 에 싣는다
    bool keepLineSegs = true;  // 조판 캐시(LineSeg) 보관
    std::string password;      // 문서 암호(암호 걸린 문서를 열 때)
};

// 예외를 쓰지 않는다 (Emscripten 기본 설정에서 예외를 잡을 수 없음).
// ok 가 false 면 error 에 사유가 있고, document 는 파싱된 부분까지만 담긴다.
struct ParseResult {
    bool ok = false;
    std::string error;
    // 암호 걸린 문서인데 암호가 없으면 needPassword, 준 암호가 틀리면 needPassword 와 wrongPassword 가 참이다.
    bool needPassword = false;
    bool wrongPassword = false;
    std::vector<std::string> warnings;
    model::Document document;
};

}  // namespace chwp
