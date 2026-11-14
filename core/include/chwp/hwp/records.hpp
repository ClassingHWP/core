#pragma once

// HWP 5.0 레코드 계층. DocInfo / BodyText/SectionN 스트림은 태그 레코드의 나열이다.
//   레코드 헤더(32bit) = TagID(10) | Level(10) | Size(12), Size == 0xFFF 면 뒤 4바이트가 실제 크기.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "chwp/common/bytes.hpp"

namespace chwp::hwp {

namespace tag {
inline constexpr std::uint16_t kBegin = 0x10;

// DocInfo
inline constexpr std::uint16_t DocumentProperties = kBegin + 0;
inline constexpr std::uint16_t IdMappings = kBegin + 1;
inline constexpr std::uint16_t BinData = kBegin + 2;
inline constexpr std::uint16_t FaceName = kBegin + 3;
inline constexpr std::uint16_t BorderFill = kBegin + 4;
inline constexpr std::uint16_t CharShape = kBegin + 5;
inline constexpr std::uint16_t TabDef = kBegin + 6;
inline constexpr std::uint16_t Numbering = kBegin + 7;
inline constexpr std::uint16_t Bullet = kBegin + 8;
inline constexpr std::uint16_t ParaShape = kBegin + 9;
inline constexpr std::uint16_t Style = kBegin + 10;
inline constexpr std::uint16_t DocData = kBegin + 11;
inline constexpr std::uint16_t DistributeDocData = kBegin + 12;
inline constexpr std::uint16_t CompatibleDocument = kBegin + 14;
inline constexpr std::uint16_t LayoutCompatibility = kBegin + 15;
inline constexpr std::uint16_t TrackChange = kBegin + 16;
inline constexpr std::uint16_t MemoShape = kBegin + 76;
inline constexpr std::uint16_t ForbiddenChar = kBegin + 78;
inline constexpr std::uint16_t TrackChangeContent = kBegin + 80;
inline constexpr std::uint16_t TrackChangeAuthor = kBegin + 81;

// BodyText
inline constexpr std::uint16_t ParaHeader = kBegin + 50;
inline constexpr std::uint16_t ParaText = kBegin + 51;
inline constexpr std::uint16_t ParaCharShape = kBegin + 52;
inline constexpr std::uint16_t ParaLineSeg = kBegin + 53;
inline constexpr std::uint16_t ParaRangeTag = kBegin + 54;
inline constexpr std::uint16_t CtrlHeader = kBegin + 55;
inline constexpr std::uint16_t ListHeader = kBegin + 56;
inline constexpr std::uint16_t PageDef = kBegin + 57;
inline constexpr std::uint16_t FootnoteShape = kBegin + 58;
inline constexpr std::uint16_t PageBorderFill = kBegin + 59;
inline constexpr std::uint16_t ShapeComponent = kBegin + 60;
inline constexpr std::uint16_t Table = kBegin + 61;
inline constexpr std::uint16_t ShapeComponentLine = kBegin + 62;
inline constexpr std::uint16_t ShapeComponentRectangle = kBegin + 63;
inline constexpr std::uint16_t ShapeComponentEllipse = kBegin + 64;
inline constexpr std::uint16_t ShapeComponentArc = kBegin + 65;
inline constexpr std::uint16_t ShapeComponentPolygon = kBegin + 66;
inline constexpr std::uint16_t ShapeComponentCurve = kBegin + 67;
inline constexpr std::uint16_t ShapeComponentOle = kBegin + 68;
inline constexpr std::uint16_t ShapeComponentPicture = kBegin + 69;
inline constexpr std::uint16_t ShapeComponentContainer = kBegin + 70;
inline constexpr std::uint16_t CtrlData = kBegin + 71;
inline constexpr std::uint16_t EqEdit = kBegin + 72;
inline constexpr std::uint16_t ShapeComponentTextArt = kBegin + 74;
inline constexpr std::uint16_t FormObject = kBegin + 75;
inline constexpr std::uint16_t MemoList = kBegin + 77;
inline constexpr std::uint16_t ChartData = kBegin + 79;
inline constexpr std::uint16_t VideoData = kBegin + 82;
inline constexpr std::uint16_t ShapeComponentUnknown = kBegin + 99;
}  // namespace tag

// 구역 정의(secd) 속성 중 모델이 푸는 비트: 머리말·꼬리말·바탕쪽·쪽 번호 감추기, 글자 방향, 원고지 정서법,
// 바탕쪽 있음(29). 나머지 비트는 읽은 그대로 다시 쓴다(Control::sectionAttrKeep).
// 한글은 바탕쪽이 있는데 29 비트가 없으면 파일을 열지 않는다.
// 한글 2024 에서 확인한 구역 정의 속성 비트: 0·1·2·5 첫 쪽 머리말·꼬리말·바탕쪽·쪽 번호 감춤, 3·4 첫 쪽 테두리·배경 감춤,
// 8·9 첫 쪽만 테두리·배경, 16–18 글자 방향, 19 빈 줄 감춤, 20–21 시작 쪽 번호 종류, 22 원고지,
// 24 줄 번호 보이기, 29·30·31 양쪽·짝수·홀수 바탕쪽 있음(그 차례로 구역 정의 아래 리스트 머리가 온다).
inline constexpr std::uint32_t kSecdKnownBits = 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x100u | 0x200u | (0x7u << 16) |
                                                 (1u << 19) | (0x3u << 20) | (1u << 22) | (1u << 24) | (0x7u << 29);

// 개체 공통 속성 중 모델이 푸는 비트: 글자처럼, 세로·가로 기준과 정렬, 본문 제한, 겹침, 폭·높이 기준, 배치,
// 좌우 배치, 번호 범주(26~27), 캡션 있음(29). 나머지 비트는 읽은 그대로 다시 쓴다(Placement::attrKeep).
// 한글은 캡션이 있는데 29 비트가 없거나, 번호 범주가 캡션 번호와 맞지 않으면 파일을 열지 않는다.
inline constexpr std::uint32_t kObjectKnownBits = 0x1u | (0x1FFFFu << 3) | (0x7Fu << 21) | (1u << 29);

const char* tagName(std::uint16_t tagId);

struct Record {
    std::uint16_t tag = 0;
    std::uint16_t level = 0;
    std::size_t offset = 0;  // 스트림 내 헤더 위치
    ByteView data;
};

// stream 은 반환된 Record 들이 쓰이는 동안 유효해야 한다.
bool parseRecords(ByteView stream, std::vector<Record>& out, std::string* error = nullptr);

// Level 로 만든 레코드 트리. 인덱스 0 은 가상 루트.
struct RecordTree {
    struct Node {
        const Record* record = nullptr;
        std::vector<std::size_t> children;
    };
    std::vector<Node> nodes;

    static RecordTree build(const std::vector<Record>& records);
    [[nodiscard]] const Node& root() const { return nodes.front(); }
    [[nodiscard]] const Node& at(std::size_t index) const { return nodes[index]; }
};

// MAKE_4CHID('t','b','l',' ') 형태의 컨트롤 ID
constexpr std::uint32_t makeCtrlId(char a, char b, char c, char d) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}
std::string ctrlIdToString(std::uint32_t id);

// 스트림 압축 해제 (raw deflate)
bool decompressStream(ByteView input, Bytes& out);

// 배포용 문서(한글 문서 파일 형식 - 배포용 문서 1.2). 본문 스트림(ViewText/Section*)은 "배포용 문서 데이터"
// 레코드(태그 28, 256 바이트, 암호화하지 않음)로 시작하고, 그 뒤는 (압축한) 레코드들을 AES-128 ECB 로 암호화한 것이다.
// 데이터의 첫 4 바이트가 씨앗(seed)이고, MS Visual C rand() 로 만든 256 바이트 배열과 XOR 하면
// (seed & 0x0F) + 4 자리에 80 바이트 해시 코드(첫 16 바이트가 열쇠)와 2 바이트 옵션(복사·인쇄 막기)이 나온다.
inline constexpr std::uint16_t kDistributeNoCopy = 0x01;
inline constexpr std::uint16_t kDistributeNoPrint = 0x02;
struct DistributeInfo {
    bool ok = false;
    std::uint8_t key[16] = {};
    std::uint16_t options = 0;
};
DistributeInfo decodeDistributeData(ByteView data256);
// 스트림 하나를 푼다. head 를 주면 256 바이트 배포용 문서 데이터를 담는다.
bool decryptDistributedStream(ByteView raw, bool compressed, Bytes& out, Bytes* head = nullptr,
                              DistributeInfo* info = nullptr);
// 레코드 바이트를 배포용 스트림으로 만든다(head 는 읽을 때 받은 256 바이트).
bool encryptDistributedStream(ByteView records, bool compressed, ByteView head, Bytes& out);

// 문서 암호(FileHeader 1 번 비트). 한글 2024 가 쓰는 방식이다: 암호를 UTF-16LE 로 바꾼 바이트를 7 바이트에 차례로
// XOR 하고 왼쪽으로 1 비트 돌려 접은 뒤, 56 비트를 8 바이트 DES 열쇠(각 바이트 맨 아래 비트는 비움)로 편다.
// 잠긴 스트림 = DES-ECB(평문) + 서명 38 A4 CE 59 + 평문의 CRC32(LE). 평문은 원래 내용을 raw deflate 해 0 으로 8 바이트까지
// 채우고, 원래 내용의 CRC32 와 길이를 u64(LE)로 하나씩 붙인 것이다.
// FileHeader·HwpSummaryInformation·DocOptions/_LinkDoc 는 잠그지 않는다. CRC 가 맞지 않으면 틀린 암호다.
Bytes passwordKey(std::string_view password);
bool isPasswordStream(ByteView raw);
// 잠긴 스트림을 풀어 원래(압축 풀린) 내용을 준다. 틀린 암호면 거짓이다.
bool decryptPasswordStream(ByteView raw, ByteView key, Bytes& out);
bool encryptPasswordStream(ByteView content, ByteView key, Bytes& out);

}  // namespace chwp::hwp
