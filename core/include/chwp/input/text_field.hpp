#pragma once

#include <string>
#include <vector>

namespace chwp::input {

// 글을 받는 입력칸 하나를 해석한다. 셸은 플랫폼의 입력칸(웹 <input>, 네이티브 텍스트 뷰 등) 사건을 FieldEvent 로
// 바꿔 넘기고, 돌아온 FieldReply 대로 입력칸에 글자를 넣거나 깨우기 타이머를 건다.
// 문서에 무엇을 넣고 지울지(FieldEdit), 입력칸을 언제 비울지, 입력기 조합의 차례는 여기서 정한다.
//
// 입력칸에 친 글은 비우지 않고 남겨 두고, 바뀐 만큼(뒤에서 지운 글자 수, 새 글)만 문서에 넘긴다.
// 아이패드 입력기는 조합 사건 없이 입력칸 앞 글자를 바꿔 조합하고, 조합 상태를 입력기 속에만 둔다.
// 스크립트가 입력칸 글을 바꾸면 그 상태가 깨져 키가 씹히거나 지우기가 낱자모 대신 글자째 지운다.
// 그래서 입력칸은 셸이 놓을 때(Reset: 다른 곳 누름, 입력칸 밖 편집)와 규칙 표가 정한 때만 비운다.
//
// 셸이 사건 그 자리에서 해야 하는 일(기본 동작 막기, 입력칸 비우기, 조합 상태)은 fieldRules() 표로 미리 준다.
// 코어가 다른 스레드에 있으면 답을 기다리는 사이 다음 조합이 시작될 수 있어서다. 셸은 표대로 하고 사건을 넘긴다.
// 코어도 같은 표로 제 상태를 바꾸므로, 셸과 코어가 보는 입력칸 글은 같은 사건 차례에서 같이 비워진다.

enum class FieldEventKind {
    CompositionStart,
    CompositionUpdate,
    CompositionEnd,
    Input,        // 입력칸 글이 바뀌었다(조합 중 바뀜 포함)
    KeyDown,      // 맞는 규칙이 send 일 때만 넘긴다. 키마다 왕복하지 않게 한다
    TurnEnd,      // 규칙이 부탁한, 사건 처리 차례가 끝난 바로 뒤(웹 queueMicrotask)
    Wake,         // FieldReply 가 부탁한 깨우기
    Reset,        // 셸이 입력칸을 놓았다(다른 곳 누름, 편집 키, 대화 상자)
    Paste,        // 붙여 넣기. data 는 클립보드 글 그대로(줄바꿈 포함). 한 줄 입력칸은 줄바꿈을 지우므로 셸이 입력칸에 넣지 않는다
};

enum class FieldInputType {
    InsertText,
    DeleteBackward,
    LineBreak,     // 줄 바꿈은 Enter 키로 따로 온다
    Composition,   // 조합 중 글 넣기·지우기
    Other,
};

struct FieldEvent {
    FieldEventKind kind = FieldEventKind::Input;
    std::string data;        // 조합 글, KeyDown 의 키 이름("a", "Enter"), Paste 의 글
    FieldInputType inputType = FieldInputType::Other;
    std::string value;       // Input: 바뀐 뒤 입력칸 전체 글
    bool isComposing = false;
    bool imeKey = false;     // KeyDown: 입력기가 가로챈 키(웹 keyCode 229)
    bool repeat = false;     // KeyDown: 길게 눌러 되풀이된 키
    bool hasText = false;    // KeyDown: 입력칸에 글이 남아 있다
    bool shift = false;
    int wakeId = 0;          // Wake: 부탁할 때 받은 번호
    bool endComposition = false;  // Reset: 조합 중이던 글을 버린다
};

// 규칙의 조건. Any 는 보지 않는다.
enum class FieldMatch { Any, Yes, No };
// 규칙이 바꿀 조합 상태. Keep 은 그대로 둔다.
enum class FieldSet { Keep, On, Off };

// 사건 하나에 셸이 그 자리에서 할 일. 맞는 규칙은 모두 적용한다.
struct FieldRule {
    FieldEventKind on = FieldEventKind::Input;
    FieldInputType inputType = FieldInputType::Other;
    bool anyInputType = true;              // false 면 Input 사건의 inputType 이 같아야 한다
    std::string key;                       // KeyDown: 비면 아무 키
    FieldMatch ime = FieldMatch::Any;      // 조합 중(isComposing, 입력칸 조합 중, 입력기 키)
    FieldMatch repeat = FieldMatch::Any;   // KeyDown 의 repeat
    FieldMatch endComposition = FieldMatch::Any;  // Reset 의 endComposition
    FieldMatch hasText = FieldMatch::Any;  // KeyDown 의 hasText
    bool send = false;                     // KeyDown 을 코어에 넘긴다
    bool preventDefault = false;
    bool clearField = false;
    bool blur = false;                     // 입력칸을 놓아 플랫폼 입력기 조합을 버린다
    FieldSet composing = FieldSet::Keep;
    bool turnEnd = false;                  // 이 사건 처리 차례가 끝나면 TurnEnd 사건을 보낸다
    bool toField = false;                  // KeyDown: 편집 키로 쓰지 않고 입력칸(입력기)에 맡긴다
};

const std::vector<FieldRule>& fieldRules();

struct FieldActions {
    bool send = false;
    bool preventDefault = false;
    bool clearField = false;
    bool blur = false;
    FieldSet composing = FieldSet::Keep;
    bool turnEnd = false;
    bool toField = false;
};
// composing 은 사건 앞 입력칸 조합 상태다.
FieldActions fieldActions(const FieldEvent& event, bool composing);

enum class FieldEditKind {
    ImeStart,
    ImeUpdate,
    ImeEnd,
    ImeSettle,
    Replace,  // 뒤에서 removed 글자를 지우고 text 를 넣는다(removed 가 0 이면 넣기만)
    Enter,    // 조합 중에 친 Enter
    Paste,    // 붙여 넣은 글. 줄 끝은 문단 나눔이다(DocSession::insertPlainText)
};

struct FieldEdit {
    FieldEditKind kind = FieldEditKind::Replace;
    std::string text;
    int removed = 0;
    bool shift = false;
    // Replace: text 의 끝 글자는 입력기가 아직 조합 중이다. 조합 사건 없이 입력칸 글을 바꿔 조합하는
    // 입력기(아이패드 한글)라 그 글자를 조합 글로 넣는다. 다음 바꾸기가 이 글자를 지우거나 뒤에 이어 친다.
    bool composeTail = false;
};

enum class FieldWake {
    None,
    After,  // wakeAfterMs 뒤(웹 setTimeout)
};

// 깨우기 자리. 셸은 자리마다 타이머 하나만 두고, 같은 자리 새 부탁은 앞 부탁을 바꾼다.
enum class FieldWakeSlot { Sync, Repeat };

struct FieldReply {
    std::vector<FieldEdit> edits;
    // 입력칸 캐럿 자리에 넣을 글. 넣은 뒤 셸은 Input 사건(InsertText)으로 바뀐 글을 알린다.
    std::string insertIntoField;
    FieldWake wake = FieldWake::None;
    int wakeAfterMs = 0;
    int wakeId = 0;
    FieldWakeSlot wakeSlot = FieldWakeSlot::Sync;
};

class TextField {
public:
    FieldReply handle(const FieldEvent& event);
    bool composing() const { return composing_; }

private:
    void requestWake(FieldReply& reply, FieldWakeSlot slot, int ms, int& id);
    void clear();
    void sync(const std::string& value, FieldReply& reply);

    std::u32string text_;       // 입력칸에 남겨 둔 글
    std::string pendingValue_;  // 지우기만 와서 미뤄 둔 입력칸 글
    bool syncPending_ = false;
    // 입력칸 끝 글자를 입력기가 조합 중이라고 본다. 입력기는 알려 주지 않아 받은 사건 차례로 따라간다:
    // 한글을 끝에 넣으면 조합 중, 한글 아닌 글자(띄어쓰기 등)를 넣거나 입력칸을 놓으면 끝.
    bool tailComposing_ = false;
    bool composing_ = false;
    bool settlePending_ = false;  // 조합 끝이 왔고 TurnEnd 에 확정한다
    std::string repeatKey_;
    int nextWake_ = 1;
    int syncWake_ = 0;
    int repeatWake_ = 0;
};

// 길게 누른 키를 대신 넣기 전에 입력기를 기다리는 시간.
inline constexpr int kFieldRepeatWaitMs = 30;

}  // namespace chwp::input
