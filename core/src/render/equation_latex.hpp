#pragma once

#include <string>
#include <string_view>

namespace noting::render {

// 한글 수식 스크립트를 LaTeX 수학 모드 문자열로 바꾼다. docs/spec/equation.md 의 문법.
std::string hangulEquationToLatex(std::string_view script);

}  // namespace noting::render
