#pragma once

// hwp_reader 내부 도우미. 샘플 파일 없이 단위 테스트할 수 있도록 분리했다.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "noting/common/bytes.hpp"
#include "noting/model/document.hpp"

namespace noting::hwp::detail {

// 파라미터 셋 아이템 ID: 책갈피/필드 이름
constexpr std::uint16_t kParamItemName = 0x4000;

// CTRL_DATA 의 파라미터 셋에서 문자열(PIT_BSTR) 아이템 값을 찾는다.
// itemId 가 없으면 처음 나오는 문자열 아이템을 돌려준다.
// 실제 파일은 아이템 개수 뒤에 규격에 없는 2바이트가 있다.
std::optional<std::string> findParameterString(ByteView data,
                                               std::optional<std::uint16_t> itemId);

// 캡션 LIST_HEADER 레코드에서 캡션 속성을 읽는다 (내용 문단은 제외).
model::Caption readCaptionHeader(ByteView data);

// 문단 텍스트의 확장 제어 문자를 CTRL_HEADER 레코드에 대응시킨다.
// 제어 문자에 들어 있는 ctrlId 와 같은, 아직 쓰지 않은 컨트롤을 앞에서부터 고르고,
// 같은 것이 없으면 남은 컨트롤 중 첫 번째를 쓴다.
class ControlMatcher {
public:
    explicit ControlMatcher(std::vector<std::uint32_t> ctrlIds);

    std::optional<std::size_t> match(std::optional<std::uint32_t> ctrlId);
    std::vector<std::size_t> remaining() const;
    std::size_t mismatches() const { return mismatches_; }

private:
    std::vector<std::uint32_t> ids_;
    std::vector<bool> used_;
    std::size_t mismatches_ = 0;
};

}  // namespace noting::hwp::detail
