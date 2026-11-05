#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chwp/common/bytes.hpp"
#include "chwp/model/document.hpp"
#include "chwp/render/edit_state.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"

namespace chwp::app {

// 막힌 글꼴마다 사용자가 고른 바꿀 글꼴(기본 짝과 다른 것만). 편집기와 agent 가 함께 쓴다.
// 저장은 셸이 이 JSON 으로 한다({"막힌 글꼴": "바꿀 글꼴"}).
void setLicenseSwapPreferences(const std::string& json);
std::string licenseSwapPreferences();

// 문서 하나: 원본 파일, 문서 모델, 조판한 쪽, 편집 상태(커서·되돌리기).
// 편집기 세션(화면이 붙은 문서)과 agent 문서(화면 없는 문서)가 같은 열기·조판·편집·저장을 이 객체로 탄다.
// 화면·카메라·입력·배경 일꾼은 여기 없고 세션이 갖는다.
class DocSession {
public:
    // 저장된 줄 높이(lineSeg)가 있으면 그 값을 쓴다. 편집기·agent 가 같은 값으로 연다.
    static constexpr bool kUseStoredLineHeight = true;
    // 받은 글꼴에도 없는 글자의 대체 글꼴은 이만큼만 되묻는다(대체에도 차례가 있다: 문서 대체 → 기본 → 함초롬).
    static constexpr int kFontGapRounds = 3;

    Bytes file;
    model::Document doc;
    std::vector<render::Node> pages;
    // 저장된 줄 높이(lineSeg)를 빌려 조판하는가. 열 때 정하고 편집도 같은 값으로 다시 놓는다.
    bool storedLineHeight = false;
    // 연 파일의 형식. 빈 문서는 hwpx 다.
    model::SourceFormat sourceFormat = model::SourceFormat::Unknown;
    render::EditState edit;

    // 원본을 읽는다. 쪽은 아직 놓지 않는다(글꼴을 받은 뒤 layout).
    bool open(Bytes bytes, std::string* error = nullptr, std::vector<std::string>* warnings = nullptr);
    // 빈 문서로 시작한다.
    void openBlank();
    // 문서를 닫고 편집 상태를 비운다.
    void close();
    // 쪽을 처음부터 놓는다. lineSeg 면 저장된 줄 높이를 빌린다. 인자가 없으면 core 기본값이다.
    void layout(bool lineSeg);
    void layout() { layout(kUseStoredLineHeight); }
    // 문서 전체가 바뀐 뒤(글꼴 이름 바꾸기 등) 같은 조판 설정으로 다시 놓는다.
    void relayoutAll();
    // 저장한다. 고친 문단과 lineSeg 없이 들어온 문단은 조판 결과를 lineSeg 로 적는다(문서 자체는 그대로).
    std::optional<Bytes> save(model::SourceFormat format, std::string* error = nullptr) const;
    // 저장할 형식. format 이 "hwp"/"hwpx" 면 그것, 비었으면 이름 끝(.hwp/.hwpx), 그것도 없으면 연 파일의 형식.
    // format 이 그 밖의 글이면 nullopt.
    std::optional<model::SourceFormat> saveFormatFor(const std::string& name, const std::string& format = {}) const;

    // 편집 명령. 이 문서의 편집 상태에 대고 편집기와 같은 명령을 부른다.
    render::EditResult insertText(const std::string& text, bool composing = false);
    // 글을 있는 그대로 넣는다(붙여 넣기, agent 글 넣기). 줄 끝(LF, CR LF, CR)은 문단 나눔,
    // U+2028 은 문단 안 줄바꿈이다. 되돌리기는 한 칸이다. 편집 하나하나의 결과를 차례로 돌려준다.
    std::vector<render::EditResult> insertPlainText(const std::string& text);
    render::EditResult keyDown(render::EditKey key, bool shift = false, bool ctrl = false, bool alt = false);
    render::EditResult applyFormat(const render::FormatPatch& patch);
    render::EditResult insertTable(int rows, int cols);
    // 그림 넣기. 파일 바이트만 받고 종류·크기·자리는 core 가 정한다.
    render::EditResult insertPicture(const Bytes& image);
    // 편집 용지. whole 이면 모든 구역에 적용한다.
    render::EditResult applyPageSetup(const render::FormatFields& fields, bool whole);
    // 조합 중 글을 확정한다.
    render::EditResult commitComposition();
    // 포인터로 한 개체 편집(그린 도형 넣기, 개체 옮기기·크기 바꾸기, 표 테두리 끌기).
    render::EditResult insertDrawnShape();
    render::EditResult moveObject();
    render::EditResult resizeObject();
    render::EditResult resizeTableBorder();
    // 막힌 글꼴을 한꺼번에 바꾼다. 되돌리기 한 칸이다. chosen 에 없는 글꼴은 licenseSwapTargets 의 글꼴로 바꾼다.
    // 기본 짝과 다르게 고른 것은 기억하고(licenseSwapPreferences), 기본 짝을 고르면 잊는다.
    render::EditResult swapLicenseFonts(const std::vector<std::pair<std::string, std::string>>& chosen = {});
    // 막힌 글꼴마다 바꿀 글꼴: 전에 고른 글꼴이 아직 고를 수 있으면 그것, 아니면 기본 짝. {막힌 글꼴, 바꿀 글꼴}.
    std::vector<std::pair<std::string, std::string>> licenseSwapTargets() const;
    render::EditResult undo();
    render::EditResult redo();
    bool canUndo();
    bool canRedo();
    // 문서 상태: "clean"(연 그대로), "dirty"(고친 뒤 저장 전), "saved"(저장한 모습 그대로).
    // 되돌리기·다시 하기로 연(저장한) 모습에 돌아오면 다시 clean(saved)이다.
    std::string documentState();
    // 저장했다. 지금 모습이 저장한 모습이다.
    void markSaved();
    // begin ~ end 사이 편집을 되돌리기 한 칸으로 묶는다. begin 은 묶음 번호를 돌려준다.
    int beginEditGroup();
    void endEditGroup();
    // 맨 위 되돌리기 칸의 묶음 번호(없으면 0).
    int topEditGroup();
    // 다시 하기 기록만 버린다.
    void dropRedo();

    // 쪽 하나를 PNG 로 그린다. 화면과 같이 GPU(Graphite)로 먼저 굽고, 못 쓰면 CPU 로 그린다.
    // 칸은 frames 에 굽는다(편집기는 화면의 칸 캐시, agent 는 제 캐시). 나중에 채울 쪽은 먼저 채운다.
    render::PageImage renderPage(int index, int pixelHeight, render::FrameCache& frames);

    // 편집으로 새로 쓰게 된 글꼴 이름(아직 받지 않았을 수 있다). 가져가면 비운다.
    // 셸은 이 이름을 받아 넣고, 받은 뒤 문서를 다시 놓는 일은 core 가 한다.
    // 처음 고칠 때 치는 글자가 문서 글꼴에 없으면 쓸 대체 글꼴, 서식으로 고른 글꼴, 막힌 글꼴의 대역이 든다.
    std::vector<std::string> takeFontWants();
    // 받은 글꼴에 없는 글자의 대체 글꼴 한 차례. kFontGapRounds 차례가 지나면 빈 목록이라 셸의 되묻기가 끝난다.
    std::vector<std::string> fontGaps();
    // 글꼴이 들어왔다(받아 등록함). replace.json 으로 대신 받은 글꼴이면 문서의 글꼴 이름을 바꾸고,
    // 쪽이 놓여 있고 이 글꼴을 문서가 쓰면(문서 글꼴, 되묻던 글자 대체 글꼴) 문서를 다시 놓는다. 다시 놓았으면 참.
    // 편집기와 agent 가 같은 판단을 탄다. 다시 놓기 전에 그리기 쪽 글꼴 캐시도 비운다(render::clearGlyphWarm).
    bool fontsLanded(const std::vector<std::string>& faces);
    // 받지 못해 대체 글꼴로 재고 그리는 문서 글꼴 이름. 굵은 짝(" Bold", " 볼드")은 보통 글꼴까지 없을 때만 넣는다.
    std::vector<std::string> missingFonts() const;

    // 이 문서의 편집 상태를 잡는다. 커서 함수(setPointerCursor 등)를 직접 부를 때 쓴다.
    render::EditScope bind() { return render::EditScope(edit); }

private:
    void noteEdit(const render::EditResult& result, std::vector<std::string> faces = {});
    void wantFont(const std::string& face);

    std::vector<std::string> fontWants_;
    bool editedOnce_ = false;
    bool savedOnce_ = false;
    int gapRounds_ = 0;
    std::vector<std::string> gapWants_;  // 되물은 글자 대체 글꼴
};

}  // namespace chwp::app
