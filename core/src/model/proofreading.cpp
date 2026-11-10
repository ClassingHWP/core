#include "chwp/model/proofreading.hpp"

#include <cstdlib>

namespace chwp::model {

namespace {

struct ProofEntry {
    ProofKind kind;
    const char* ctrlId;
    const char* hwpxType;
    const char* name;
};

// id 는 HWP 5.0 규격(표 154), HWPX 이름은 한글 2024 가 저장한 HWPX 의 글이다. 지움표·뺌표·붙임표·줄 붙임표·톱니표·
// 생각표·칭찬표·줄표·고침표·오른·왼 자리 옮김표·띄움 등(%sig)·메모 고침표는 한글이 저장한 HWP·HWPX 로 짝을 확인했다.
constexpr ProofEntry kProofs[] = {
    {ProofKind::Delete, "%%*d", "PROOFREADING_MARKS_DELETE", "지움표"},
    {ProofKind::Attach, "%%*a", "PROOFREADING_MARKS_ATTACH", "붙임표"},
    {ProofKind::Clipping, "%%*C", "PROOFREADING_MARKS_CLIPPING", "뺌표"},
    {ProofKind::Sawtooth, "%%*S", "PROOFREADING_MARKS_SAWTOOTH", "톱니표"},
    {ProofKind::Thinking, "%%*T", "PROOFREADING_MARKS_THINKING", "생각표"},
    {ProofKind::Praise, "%%*P", "PROOFREADING_MARKS_PRAISE", "칭찬표"},
    {ProofKind::Line, "%%*L", "PROOFREADING_MARKS_LINE", "줄표"},
    {ProofKind::SimpleChange, "%%*c", "PROOFREADING_MARKS_SIMPLECHANGE", "고침표"},
    {ProofKind::Hyperlink, "%%*h", "PROOFREADING_MARKS_HYPERLINK", "자료 연결"},
    {ProofKind::LineAttach, "%%*A", "PROOFREADING_MARKS_LINEATTACH", "줄 붙임표"},
    {ProofKind::LineLink, "%%*i", "PROOFREADING_MARKS_LINELINK", "줄 이음표"},
    {ProofKind::LineTransfer, "%%*t", "PROOFREADING_MARKS_LINETRANSFER", "줄 서로 바꿈표"},
    {ProofKind::RightMove, "%%*r", "PROOFREADING_MARKS_RIGHTMOVE", "오른 자리 옮김표"},
    {ProofKind::LeftMove, "%%*l", "PROOFREADING_MARKS_LEFTMOVE", "왼 자리 옮김표"},
    {ProofKind::Transfer, "%%*n", "PROOFREADING_MARKS_TRANSFER", "자리 바꿈표"},
    {ProofKind::SimpleInsert, "%%*e", "PROOFREADING_MARKS_SIMPLEINSERT", "넣음표"},
    {ProofKind::Split, "%spl", "PROOFREADING_MARKS_SPLIT", "교정 부호"},
    {ProofKind::Change, "%%mr", "PROOFREADING_MARKS_CHANGE", "메모 고침표"},
    {ProofKind::Sign, "%sig", "PROOFREADING_MARKS_SIGN", "교정 부호"},
};

}  // namespace

ProofKind proofKindOfId(std::string_view ctrlId) {
    for (const ProofEntry& e : kProofs) {
        if (ctrlId == e.ctrlId) return e.kind;
    }
    return ProofKind::None;
}

ProofKind proofKindOf(const Control& control) {
    if (control.kind != ControlKind::Field) return ProofKind::None;
    return proofKindOfId(control.ctrlId);
}

std::string proofCtrlIdOfType(std::string_view hwpxType) {
    for (const ProofEntry& e : kProofs) {
        if (hwpxType == e.hwpxType) return e.ctrlId;
    }
    return {};
}

const char* proofHwpxType(std::string_view ctrlId) {
    for (const ProofEntry& e : kProofs) {
        if (ctrlId == e.ctrlId) return e.hwpxType;
    }
    return nullptr;
}

std::uint32_t ctrlIdNumber(std::string_view ctrlId) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(i < ctrlId.size() ? ctrlId[i] : ' ');
    }
    return value;
}

int proofSignNumber(const std::string& command) {
    // "$RevisionSign;1;" 또는 "$RevisionSign?글;15;". 마지막 두 ';' 사이 수다.
    if (command.empty()) return 0;
    std::size_t end = command.size();
    if (command[end - 1] == ';') --end;
    const std::size_t start = command.rfind(';', end == 0 ? 0 : end - 1);
    if (start == std::string::npos || start + 1 >= end) return 0;
    return std::atoi(command.substr(start + 1, end - start - 1).c_str());
}

std::string proofText(const std::string& command) {
    const std::size_t q = command.find('?');
    if (q == std::string::npos) return {};
    const std::size_t semi = command.find(';', q + 1);
    return command.substr(q + 1, semi == std::string::npos ? std::string::npos : semi - q - 1);
}

bool proofMove(const std::string& command, std::int32_t& start, std::int32_t& gap) {
    const std::size_t first = command.find(';');
    if (first == std::string::npos) return false;
    const std::size_t second = command.find(';', first + 1);
    if (second == std::string::npos) return false;
    gap = static_cast<std::int32_t>(std::atol(command.substr(first + 1, second - first - 1).c_str()) / 2);
    start = static_cast<std::int32_t>(std::atol(command.c_str() + second + 1) / 2);
    return true;
}

std::string proofMoveCommand(bool right, double startPt, double gapPt) {
    const auto units = [](double pt) { return std::to_string(static_cast<long>(pt * 200.0 + (pt < 0 ? -0.5 : 0.5))); };
    return std::string(right ? "$RevisionRightMove;" : "$RevisionLeftMove;") + units(gapPt) + ";" + units(startPt) + ";";
}

std::string proofName(const Control& control) {
    const ProofKind kind = proofKindOf(control);
    if (kind == ProofKind::Sign) {
        switch (proofSignNumber(control.command)) {
            case 1: return "띄움표";
            case 2: return "줄 바꿈표";
            case 3: return "줄 비움표";
            case 14: return "부호 넣음표";
            case 15: return "넣음표";
            default: return "교정 부호";
        }
    }
    for (const ProofEntry& e : kProofs) {
        if (e.kind == kind) return e.name;
    }
    return {};
}

}  // namespace chwp::model
