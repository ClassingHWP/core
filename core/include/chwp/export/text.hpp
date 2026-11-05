#pragma once

#include <string>

#include "chwp/model/document.hpp"

namespace chwp {

struct TextOptions {
    bool includeHeaderFooter = false;
    bool includeNotes = true;      // 각주/미주를 [^...] 로 본문에 넣는다
    bool includeShapeText = true;  // 글상자/도형 안 글
    bool includeEquations = true;  // 수식 스크립트
    bool includeCaptions = true;   // 표/그림 캡션 (위·왼쪽 캡션은 개체 앞, 그 외는 뒤)
};

// 표는 행마다 한 줄, 셀은 탭으로 구분한다.
std::string documentToText(const model::Document& doc, const TextOptions& options = {});

// 글자 폭(장평·자간·공백 0.5em)과 문단 폭, 개체 바깥 여백으로 줄을 나누고 쪽을 가른다.
std::vector<std::string> documentToPages(const model::Document& doc,
                                         const TextOptions& options = {});

std::string paragraphsToText(const std::vector<model::Paragraph>& paragraphs,
                             const TextOptions& options = {});

}  // namespace chwp
