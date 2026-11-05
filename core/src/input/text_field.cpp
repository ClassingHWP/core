#include "chwp/input/text_field.hpp"

#include <cstdint>

#include "chwp/common/unicode.hpp"

namespace chwp::input {

namespace {

std::u32string decode(const std::string& text) {
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        int extra = 0;
        char32_t cp = c;
        if (c >= 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else if (c >= 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if (c >= 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        }
        ++i;
        for (int k = 0; k < extra && i < text.size(); ++k, ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        }
        out.push_back(cp);
    }
    return out;
}

std::string encode(std::u32string_view text) {
    std::string out;
    for (const char32_t cp : text) appendUtf8(out, cp);
    return out;
}

bool singleCharacter(const std::string& key) { return !key.empty() && decode(key).size() == 1; }

// 입력기가 아직 조합 중일 수 있는 글자: 한글 음절과 낱자모. 아이패드 입력기는 입력칸의 앞 글자를 바꿔 조합한다.
bool mayCompose(char32_t c) {
    return (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x3131 && c <= 0x318E) || (c >= 0x1100 && c <= 0x11FF);
}

bool matches(FieldMatch want, bool value) { return want == FieldMatch::Any || (want == FieldMatch::Yes) == value; }

FieldRule ruleOn(FieldEventKind on) {
    FieldRule r;
    r.on = on;
    return r;
}

std::vector<FieldRule> buildRules() {
    std::vector<FieldRule> rules;
    FieldRule r = ruleOn(FieldEventKind::CompositionStart);
    r.composing = FieldSet::On;
    rules.push_back(r);
    r = ruleOn(FieldEventKind::CompositionUpdate);
    r.composing = FieldSet::On;
    rules.push_back(r);
    // 조합 끝은 그 자리에서 비운다. 답을 기다리면 그사이 시작된 다음 조합을 지워 키가 씹힌다.
    // 끝 글을 확정할지는 이 차례가 끝나야 안다. 바로 이어지는 조합이 있으면 그 시작이 먼저 온다.
    r = ruleOn(FieldEventKind::CompositionEnd);
    r.composing = FieldSet::Off;
    r.clearField = true;
    r.turnEnd = true;
    rules.push_back(r);
    // 줄 바꿈은 Enter 키로 온다. 입력칸에 들어간 줄 바꿈은 버린다.
    r = ruleOn(FieldEventKind::Input);
    r.anyInputType = false;
    r.inputType = FieldInputType::LineBreak;
    r.ime = FieldMatch::No;
    r.clearField = true;
    rules.push_back(r);
    // 조합 중 Enter 는 조합을 끝내고 줄을 나눈다. 입력기가 Enter 를 입력칸에 넣지 않게 막는다.
    r = ruleOn(FieldEventKind::KeyDown);
    r.key = "Enter";
    r.ime = FieldMatch::Yes;
    r.send = true;
    r.preventDefault = true;
    r.clearField = true;
    r.composing = FieldSet::Off;
    rules.push_back(r);
    // 입력기 키를 길게 눌러 되풀이된다. 입력기가 글자를 넣지 않으면(아이패드 한글) 코어가 대신 넣게 한다.
    r = ruleOn(FieldEventKind::KeyDown);
    r.repeat = FieldMatch::Yes;
    r.ime = FieldMatch::Yes;
    r.send = true;
    rules.push_back(r);
    // 입력칸에 글이 남아 있으면 지우기는 입력기가 한다. 아이패드 한글은 조합 사건 없이 입력칸 글을 바꿔 조합하므로
    // 남은 글자가 조합 중일 수 있고, 입력기만 마지막 낱자모를 뺄 수 있다(각 -> 가). 바뀐 글은 Input 으로 온다.
    r = ruleOn(FieldEventKind::KeyDown);
    r.key = "Backspace";
    r.ime = FieldMatch::No;
    r.hasText = FieldMatch::Yes;
    r.toField = true;
    rules.push_back(r);
    r = ruleOn(FieldEventKind::Reset);
    r.clearField = true;
    rules.push_back(r);
    // 붙여 넣기는 입력칸에 넣지 않는다(한 줄 입력칸은 줄바꿈을 지운다). 글은 코어가 문서에 바로 넣고,
    // 입력칸에 남은 글은 옛 자리 글이 되므로 비운다.
    r = ruleOn(FieldEventKind::Paste);
    r.preventDefault = true;
    r.clearField = true;
    rules.push_back(r);
    // 조합 중에 입력칸을 놓으면 플랫폼 입력기의 조합도 버린다.
    r = ruleOn(FieldEventKind::Reset);
    r.endComposition = FieldMatch::Yes;
    r.ime = FieldMatch::Yes;
    r.blur = true;
    r.composing = FieldSet::Off;
    rules.push_back(r);
    return rules;
}

}  // namespace

const std::vector<FieldRule>& fieldRules() {
    static const std::vector<FieldRule> rules = buildRules();
    return rules;
}

FieldActions fieldActions(const FieldEvent& event, bool composing) {
    FieldActions out;
    const bool ime = event.isComposing || composing || event.imeKey;
    for (const FieldRule& r : fieldRules()) {
        if (r.on != event.kind) continue;
        if (!r.anyInputType && r.inputType != event.inputType) continue;
        if (!r.key.empty() && r.key != event.data) continue;
        if (!matches(r.ime, ime) || !matches(r.repeat, event.repeat)) continue;
        if (!matches(r.endComposition, event.endComposition) || !matches(r.hasText, event.hasText)) continue;
        out.send = out.send || r.send;
        out.preventDefault = out.preventDefault || r.preventDefault;
        out.clearField = out.clearField || r.clearField;
        out.blur = out.blur || r.blur;
        out.turnEnd = out.turnEnd || r.turnEnd;
        out.toField = out.toField || r.toField;
        if (r.composing != FieldSet::Keep) out.composing = r.composing;
    }
    return out;
}

void TextField::requestWake(FieldReply& reply, FieldWakeSlot slot, int ms, int& id) {
    id = nextWake_++;
    reply.wake = FieldWake::After;
    reply.wakeAfterMs = ms;
    reply.wakeId = id;
    reply.wakeSlot = slot;
}

void TextField::clear() {
    text_.clear();
    pendingValue_.clear();
    syncPending_ = false;
    tailComposing_ = false;
    // 걸어 둔 깨우기는 번호를 바꿔 무르게 한다.
    syncWake_ = 0;
}

// 앞에서부터 같은 데까지는 그대로다. 그 뒤로 지워진 글자 수와 새로 들어온 글을 넘긴다.
void TextField::sync(const std::string& value, FieldReply& reply) {
    syncPending_ = false;
    syncWake_ = 0;
    const std::u32string after = decode(value);
    std::size_t same = 0;
    while (same < text_.size() && same < after.size() && text_[same] == after[same]) ++same;
    // 끝에 넣은 한글 글자는 입력기가 아직 조합 중이다.
    // 지우기만 왔으면 지운 글자가 조합 중이었을 때만 남은 끝 글자를 다시 조합 글로 넣는다:
    // 조합 글자를 낱자모까지 다 지우면 입력기는 앞 글자를 이어 조합한다(교육과 에서 과를 다 지우면 육).
    // 띄어쓰기처럼 조합이 끝난 뒤 지운 것이면 앞 글자는 조합 중이 아니다.
    const bool inserted = same < after.size();
    const bool deletedOnly = !inserted && same < text_.size();
    const bool tail = !after.empty() && mayCompose(after.back()) && (inserted || (deletedOnly && tailComposing_));
    if (tail && deletedOnly) --same;
    if (inserted || deletedOnly) tailComposing_ = tail;
    const int removed = static_cast<int>(text_.size() - same);
    const std::string added = encode(std::u32string_view(after).substr(same));
    text_ = after;
    if (removed > 0 || !added.empty()) reply.edits.push_back(FieldEdit{FieldEditKind::Replace, added, removed, false, tail});
}

FieldReply TextField::handle(const FieldEvent& event) {
    FieldReply reply;
    const bool ime = event.isComposing || composing_ || event.imeKey;
    const bool wasComposing = composing_;
    // 셸이 사건 그 자리에서 표대로 한 일을 코어 상태에도 같은 차례로 한다.
    const FieldActions act = fieldActions(event, composing_);
    if (act.composing != FieldSet::Keep) composing_ = act.composing == FieldSet::On;
    if (act.clearField) {
        // 미뤄 둔 지우기는 비우기 전에 넘긴다. 지우기 바로 뒤 편집 키가 오면 입력칸을 놓는다.
        if (syncPending_) sync(pendingValue_, reply);
        clear();
    }
    // 입력기가 글을 넣거나 조합을 시작하면 길게 누른 키를 대신 넣지 않는다.
    if (event.kind == FieldEventKind::Input || event.kind == FieldEventKind::CompositionStart ||
        event.kind == FieldEventKind::CompositionUpdate) {
        repeatKey_.clear();
    }
    switch (event.kind) {
        case FieldEventKind::CompositionStart:
            reply.edits.push_back(FieldEdit{FieldEditKind::ImeStart, {}, 0, false, false});
            break;
        case FieldEventKind::CompositionUpdate:
            reply.edits.push_back(FieldEdit{FieldEditKind::ImeUpdate, event.data, 0, false, false});
            break;
        case FieldEventKind::CompositionEnd:
            if (!wasComposing) break;
            reply.edits.push_back(FieldEdit{FieldEditKind::ImeEnd, event.data, 0, false, false});
            settlePending_ = true;
            break;
        case FieldEventKind::TurnEnd:
            if (!settlePending_) break;
            settlePending_ = false;
            reply.edits.push_back(FieldEdit{FieldEditKind::ImeSettle, {}, 0, false, false});
            break;
        case FieldEventKind::Input:
            if (ime || event.inputType == FieldInputType::Composition) break;
            if (event.inputType == FieldInputType::LineBreak) break;
            // 아이패드 입력기는 앞 글자를 바꿀 때 지우기와 넣기를 따로 보낸다. 지우기만 온 때는 잠깐 기다려 넣기와 함께 넘긴다.
            if (event.inputType == FieldInputType::DeleteBackward && !syncPending_) {
                syncPending_ = true;
                pendingValue_ = event.value;
                requestWake(reply, FieldWakeSlot::Sync, 0, syncWake_);
                break;
            }
            sync(event.value, reply);
            break;
        case FieldEventKind::KeyDown:
            // 길게 누른 키가 되풀이되는데 입력기가 글자를 넣지 않으면(아이패드 한글), 그 글자를 입력칸에 넣는다.
            if (event.repeat && event.imeKey && !wasComposing && singleCharacter(event.data)) {
                repeatKey_ = event.data;
                requestWake(reply, FieldWakeSlot::Repeat, kFieldRepeatWaitMs, repeatWake_);
                break;
            }
            if (ime && event.data == "Enter") reply.edits.push_back(FieldEdit{FieldEditKind::Enter, {}, 0, event.shift, false});
            break;
        case FieldEventKind::Wake:
            if (event.wakeId == 0) break;
            if (event.wakeId == syncWake_) {
                if (syncPending_) sync(pendingValue_, reply);
            } else if (event.wakeId == repeatWake_) {
                repeatWake_ = 0;
                if (!repeatKey_.empty()) {
                    reply.insertIntoField = repeatKey_;
                    repeatKey_.clear();
                }
            }
            break;
        case FieldEventKind::Reset:
            repeatKey_.clear();
            break;
        case FieldEventKind::Paste:
            repeatKey_.clear();
            if (!event.data.empty()) reply.edits.push_back(FieldEdit{FieldEditKind::Paste, event.data, 0, false, false});
            break;
    }
    return reply;
}

}  // namespace chwp::input
