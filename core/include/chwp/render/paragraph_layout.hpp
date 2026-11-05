#pragma once

// 문단 배치 표. 문단 id(model::Paragraph::id)를 열쇠로 그 문단의 줄 배치와 머리 번호를 든다.
// 줄 배치는 한글 lineSeg 와 같은 값이라, 저장할 때 그대로 lineSeg 가 된다. 여러 쪽·여러 단에 나뉜 문단도 표는 하나다.
//
// 줄 하나의 배치(글자, 글자 표, 줄 상자, lineSeg)는 줄 노드가 든 LineContent 이고, 이 표는 그것을 나눠 든다.
// 표는 쪽 나무(Node)에서 모아 채운다(collect).
//
// 표에 드는 것은 문서 문단뿐이다. 머리말·꼬리말·바탕쪽·각주처럼 쪽마다 번호를 넣어 다시 짜는 줄은 쪽별 사본이라
// 그 쪽 노드에만 있고 표에 들지 않는다. 같은 문단을 다음 쪽에 다시 찍은 사본(쪽을 넘는 표를 든 문단, 반복 제목줄)도
// 처음 놓인 것만 든다.
//
// 잠그는 차례:
// - 일꾼은 쪽 채우기 잠금(PageFillGuard) 안에서만 줄 내용을 고친다(fillDeferredPage). collect 도 같은 잠금 안에서 모은다.
// - 줄 내용은 고칠 때 나눠 든 것이면 먼저 떼어 낸다(Node::editContent). 표가 든 줄 내용은 일꾼이 고치지 않고
//   새 사본에 쓰므로, 표를 읽는 쪽은 잠그지 않아도 모은 때의 값을 그대로 본다.

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/scene.hpp"

namespace chwp::render {

// 문단 안 한 줄의 배치.
struct LineLayout {
    std::int32_t start = 0;  // 문단 글자 번호(개체·각주 1칸)
    std::int32_t end = 0;
    // 한글 lineSeg 와 같은 값. vertPos 는 단(또는 칸·글상자) 맨 위에서 잰 자리이고,
    // textPos 는 아직 우리 글자 번호다. 저장할 때 한글 단위로 바꾼다.
    model::LineSeg seg;
    // 줄 글자와 글자 표. 줄 노드와 같은 것을 나눠 든다(복사하지 않는다).
    std::shared_ptr<const LineContent> content;

    // 줄 내용은 값으로 견준다.
    bool operator==(const LineLayout& other) const;
};

// 한 문단의 배치.
struct ParagraphLayout {
    std::vector<LineLayout> lines;              // 문단 차례. 쪽·단을 넘어도 이어진다
    std::shared_ptr<const HeadingRun> heading;  // 첫 줄 앞 머리 번호 글자. 그림 머리는 줄 자식 개체다
    std::int32_t height = 0;                    // 쪽·단에 놓인 조각 높이의 합
    // 값이 바뀔 때마다 새로 받는 판 번호. 같은 문단이라도 판이 같으면 배치가 같다. 0 은 판이 없음이다.
    std::uint32_t version = 0;

    // 판 번호는 빼고 견준다.
    [[nodiscard]] bool sameLayout(const ParagraphLayout& other) const;
};

// 문단 id 를 열쇠로 하는 문단 배치 저장소.
class ParagraphLayouts {
public:
    [[nodiscard]] const ParagraphLayout* find(std::uint32_t id) const;
    // 넣는다. 그 문단의 지금 값과 같으면 판을 그대로 두고, 다르거나 처음이면 새 판을 준다. 0 번 문단은 받지 않는다.
    const ParagraphLayout* put(std::uint32_t id, ParagraphLayout layout);
    void erase(std::uint32_t id) { byId_.erase(id); }
    void clear() { byId_.clear(); }
    [[nodiscard]] std::size_t size() const { return byId_.size(); }

    // 쪽 나무의 본문(머리말·꼬리말·각주·바탕쪽 사본은 빼고, 칸·글상자 안 문단은 넣고)에서 모아 통째로 바꾼다.
    // 나무에 없는 문단은 지운다. 값이 그대로인 문단은 판을 지킨다.
    void collect(const std::vector<Node>& pages);

private:
    std::unordered_map<std::uint32_t, ParagraphLayout> byId_;
};

// 저장할 문서 사본에 쓴다. lineSeg 가 없는 문단에 문단 배치 표의 줄을 lineSeg 로 적는다. 적은 문단 수를 돌려준다.
int fillMissingLineSegs(model::Document& doc, const ParagraphLayouts& layouts);

}  // namespace chwp::render
