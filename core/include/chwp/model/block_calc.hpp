#pragma once

// 블록 계산(도구 > 블록 계산: 블록 합계·블록 평균). 한글을 따르지 않는 우리 규칙이다(한글은 괄호 안을 버린다):
//   - 빈칸(빈칸·탭·줄 바꿈·문단 끝·NBSP·전각 빈칸)으로 나눈 낱말 가운데 수로만 된 낱말을 센다.
//     수는 앞 부호(+, -, −) 하나, 숫자, 세 자리 쉼표, 소수점 하나와 소수 숫자다(".5" 도 된다).
//   - 낱말 끝의 문장 부호(쉼표·쌍반점·쌍점·느낌표·물음표, 마침표 하나)는 떼고 읽는다("1, 2, 3." 은 1·2·3).
//   - 괄호로 감싼 수 "(1,000)" 은 회계 표기라 -1000 이다. 괄호 안에 부호가 있으면 수가 아니다.
//   - 글자가 붙은 낱말("3개", "1조", "50%", "₩1,000")은 수가 아니다.
//   - 합계는 읽은 수 가운데 소수 자리가 가장 긴 만큼, 평균은 소수 넷째 자리까지 반올림하고 끝 0 은 지운다.
//     읽은 수에 세 자리 쉼표가 있었으면 결과에도 쉼표를 찍는다.

#include <string>
#include <string_view>

namespace chwp::model {

struct BlockNumbers {
    int count = 0;
    double sum = 0;
    int decimals = 0;     // 읽은 수의 가장 긴 소수 자리
    bool commas = false;  // 세 자리 쉼표를 쓴 수가 있었다
};

BlockNumbers blockNumbers(std::string_view text);
// 수 하나가 우리 규칙의 수인지 읽는다. 아니면 거짓.
bool blockNumberWord(std::string_view word, double& value, int& decimals, bool& commas);
std::string blockSumText(const BlockNumbers& numbers);
// 수가 없으면 빈 글.
std::string blockAverageText(const BlockNumbers& numbers);

}  // namespace chwp::model
