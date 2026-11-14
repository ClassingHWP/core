#include "chwp/hwp/records.hpp"

#include <array>
#include <cstring>

#include "chwp/common/crypto.hpp"
#include "chwp/common/unicode.hpp"

#include "chwp/container/zip.hpp"

namespace chwp::hwp {

const char* tagName(std::uint16_t t) {
    switch (t) {
        case tag::DocumentProperties: return "DOCUMENT_PROPERTIES";
        case tag::IdMappings: return "ID_MAPPINGS";
        case tag::BinData: return "BIN_DATA";
        case tag::FaceName: return "FACE_NAME";
        case tag::BorderFill: return "BORDER_FILL";
        case tag::CharShape: return "CHAR_SHAPE";
        case tag::TabDef: return "TAB_DEF";
        case tag::Numbering: return "NUMBERING";
        case tag::Bullet: return "BULLET";
        case tag::ParaShape: return "PARA_SHAPE";
        case tag::Style: return "STYLE";
        case tag::DocData: return "DOC_DATA";
        case tag::DistributeDocData: return "DISTRIBUTE_DOC_DATA";
        case tag::CompatibleDocument: return "COMPATIBLE_DOCUMENT";
        case tag::LayoutCompatibility: return "LAYOUT_COMPATIBILITY";
        case tag::TrackChange: return "TRACKCHANGE";
        case tag::MemoShape: return "MEMO_SHAPE";
        case tag::ForbiddenChar: return "FORBIDDEN_CHAR";
        case tag::TrackChangeContent: return "TRACK_CHANGE";
        case tag::TrackChangeAuthor: return "TRACK_CHANGE_AUTHOR";
        case tag::ParaHeader: return "PARA_HEADER";
        case tag::ParaText: return "PARA_TEXT";
        case tag::ParaCharShape: return "PARA_CHAR_SHAPE";
        case tag::ParaLineSeg: return "PARA_LINE_SEG";
        case tag::ParaRangeTag: return "PARA_RANGE_TAG";
        case tag::CtrlHeader: return "CTRL_HEADER";
        case tag::ListHeader: return "LIST_HEADER";
        case tag::PageDef: return "PAGE_DEF";
        case tag::FootnoteShape: return "FOOTNOTE_SHAPE";
        case tag::PageBorderFill: return "PAGE_BORDER_FILL";
        case tag::ShapeComponent: return "SHAPE_COMPONENT";
        case tag::Table: return "TABLE";
        case tag::ShapeComponentLine: return "SHAPE_COMPONENT_LINE";
        case tag::ShapeComponentRectangle: return "SHAPE_COMPONENT_RECTANGLE";
        case tag::ShapeComponentEllipse: return "SHAPE_COMPONENT_ELLIPSE";
        case tag::ShapeComponentArc: return "SHAPE_COMPONENT_ARC";
        case tag::ShapeComponentPolygon: return "SHAPE_COMPONENT_POLYGON";
        case tag::ShapeComponentCurve: return "SHAPE_COMPONENT_CURVE";
        case tag::ShapeComponentOle: return "SHAPE_COMPONENT_OLE";
        case tag::ShapeComponentPicture: return "SHAPE_COMPONENT_PICTURE";
        case tag::ShapeComponentContainer: return "SHAPE_COMPONENT_CONTAINER";
        case tag::CtrlData: return "CTRL_DATA";
        case tag::EqEdit: return "EQEDIT";
        case tag::ShapeComponentTextArt: return "SHAPE_COMPONENT_TEXTART";
        case tag::FormObject: return "FORM_OBJECT";
        case tag::MemoList: return "MEMO_LIST";
        case tag::ChartData: return "CHART_DATA";
        case tag::VideoData: return "VIDEO_DATA";
        case tag::ShapeComponentUnknown: return "SHAPE_COMPONENT_UNKNOWN";
        default: return "UNKNOWN";
    }
}

bool parseRecords(ByteView stream, std::vector<Record>& out, std::string* error) {
    out.clear();
    ByteReader r(stream);
    while (!r.atEnd()) {
        const std::size_t offset = r.position();
        const std::uint32_t header = r.u32();
        std::uint32_t size = header >> 20;
        if (size == 0xFFF) size = r.u32();
        if (!r.ok() || size > r.remaining()) {
            if (error) *error = "truncated record at offset " + std::to_string(offset);
            return false;
        }
        Record rec;
        rec.tag = static_cast<std::uint16_t>(header & 0x3FF);
        rec.level = static_cast<std::uint16_t>((header >> 10) & 0x3FF);
        rec.offset = offset;
        rec.data = r.bytes(size);
        out.push_back(rec);
    }
    return true;
}

RecordTree RecordTree::build(const std::vector<Record>& records) {
    RecordTree tree;
    tree.nodes.reserve(records.size() + 1);
    tree.nodes.emplace_back();

    // (노드 인덱스, 레벨). 가상 루트는 레벨 -1
    std::vector<std::pair<std::size_t, int>> stack{{0, -1}};
    for (const auto& rec : records) {
        const int level = rec.level;
        while (stack.size() > 1 && stack.back().second >= level) stack.pop_back();
        const std::size_t index = tree.nodes.size();
        Node node;
        node.record = &rec;
        tree.nodes.push_back(std::move(node));
        tree.nodes[stack.back().first].children.push_back(index);
        stack.emplace_back(index, level);
    }
    return tree;
}

std::string ctrlIdToString(std::uint32_t id) {
    std::string s(4, ' ');
    s[0] = static_cast<char>((id >> 24) & 0xFF);
    s[1] = static_cast<char>((id >> 16) & 0xFF);
    s[2] = static_cast<char>((id >> 8) & 0xFF);
    s[3] = static_cast<char>(id & 0xFF);
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '?';
    }
    return s;
}

bool decompressStream(ByteView input, Bytes& out) {
    return zip::inflateRaw(input, out);
}

namespace {

constexpr std::uint16_t kTagDistributeDocData = 28;

// 씨앗으로 만든 256 바이트 배열: 홀수 번째 rand() & 0xFF 를 짝수 번째 (rand() & 0x0F) + 1 번 채운다.
std::array<std::uint8_t, 256> distributeMask(std::uint32_t seed) {
    std::array<std::uint8_t, 256> mask{};
    crypto::MsvcRand rnd(seed);
    std::size_t at = 0;
    while (at < mask.size()) {
        const auto value = static_cast<std::uint8_t>(rnd.next() & 0xFF);
        const int count = (rnd.next() & 0x0F) + 1;
        for (int i = 0; i < count && at < mask.size(); ++i) mask[at++] = value;
    }
    return mask;
}

}  // namespace

DistributeInfo decodeDistributeData(ByteView data) {
    DistributeInfo info;
    if (data.size() < 256) return info;
    const std::uint32_t seed = static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
                               (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
    const std::array<std::uint8_t, 256> mask = distributeMask(seed);
    std::uint8_t plain[256];
    for (std::size_t i = 0; i < 256; ++i) plain[i] = static_cast<std::uint8_t>(data[i] ^ mask[i]);
    const std::size_t offset = (seed & 0x0F) + 4;
    std::memcpy(info.key, plain + offset, 16);
    info.options = static_cast<std::uint16_t>(plain[offset + 80] | (plain[offset + 81] << 8));
    info.ok = true;
    return info;
}

bool decryptDistributedStream(ByteView raw, bool compressed, Bytes& out, Bytes* head, DistributeInfo* infoOut) {
    out.clear();
    if (raw.size() < 4 + 256) return false;
    const std::uint32_t header = static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8) |
                                 (static_cast<std::uint32_t>(raw[2]) << 16) | (static_cast<std::uint32_t>(raw[3]) << 24);
    if ((header & 0x3FF) != kTagDistributeDocData || (header >> 20) != 256) return false;
    const ByteView data = raw.subspan(4, 256);
    const DistributeInfo info = decodeDistributeData(data);
    if (!info.ok) return false;
    if (head) head->assign(data.begin(), data.end());
    if (infoOut) *infoOut = info;
    const crypto::Aes aes(ByteView(info.key, 16));
    const Bytes plain = crypto::aesEcbDecrypt(aes, raw.subspan(4 + 256));
    if (!compressed) {
        out = plain;
        return true;
    }
    return decompressStream(plain, out) && !out.empty();
}

namespace {
constexpr std::uint8_t kPasswordSignature[4] = {0x38, 0xA4, 0xCE, 0x59};
}

Bytes passwordKey(std::string_view password) {
    const std::u16string wide = utf8ToUtf16(password);
    std::uint8_t fold[7] = {};
    std::size_t at = 0;
    const auto add = [&](std::uint8_t b) {
        const auto v = static_cast<std::uint8_t>(b ^ fold[at % 7]);
        fold[at % 7] = static_cast<std::uint8_t>((v << 1) | (v >> 7));
        ++at;
    };
    for (const char16_t ch : wide) {
        add(static_cast<std::uint8_t>(ch & 0xFF));
        add(static_cast<std::uint8_t>(ch >> 8));
    }
    // 56 비트(앞 바이트의 높은 비트부터)를 바이트마다 7 비트씩 높은 자리에 채운다.
    Bytes key(8, 0);
    int bit = 0;
    for (int pos = 0; pos < 64; ++pos) {
        if (pos % 8 == 7) continue;
        const int value = (fold[bit / 8] >> (7 - bit % 8)) & 1;
        key[static_cast<std::size_t>(pos / 8)] |= static_cast<std::uint8_t>(value << (7 - pos % 8));
        ++bit;
    }
    return key;
}

bool isPasswordStream(ByteView raw) {
    return raw.size() >= 8 && (raw.size() - 8) % 8 == 0 &&
           std::memcmp(raw.data() + raw.size() - 8, kPasswordSignature, 4) == 0;
}

bool decryptPasswordStream(ByteView raw, ByteView key, Bytes& out) {
    out.clear();
    if (!isPasswordStream(raw)) return false;
    const crypto::Des des(key);
    if (!des.ok()) return false;
    const Bytes plain = crypto::desEcbDecrypt(des, raw.subspan(0, raw.size() - 8));
    const std::uint8_t* tail = raw.data() + raw.size() - 4;
    const std::uint32_t stored = static_cast<std::uint32_t>(tail[0]) | (static_cast<std::uint32_t>(tail[1]) << 8) |
                                 (static_cast<std::uint32_t>(tail[2]) << 16) | (static_cast<std::uint32_t>(tail[3]) << 24);
    if (zip::crc32(plain) != stored) return false;
    return decompressStream(plain, out);
}

bool encryptPasswordStream(ByteView content, ByteView key, Bytes& out) {
    out.clear();
    Bytes plain;
    if (!zip::deflateRaw(content, plain)) return false;
    plain.resize((plain.size() + 7) / 8 * 8, 0);
    // 한글은 deflate 뒤에 원문의 CRC32 와 길이를 8 바이트씩(LE) 붙여 둔다. 없으면 한글이 본문을 버린다.
    const std::uint64_t contentCrc = zip::crc32(content);
    const std::uint64_t contentSize = content.size();
    for (int i = 0; i < 8; ++i) plain.push_back(static_cast<std::uint8_t>(contentCrc >> (8 * i)));
    for (int i = 0; i < 8; ++i) plain.push_back(static_cast<std::uint8_t>(contentSize >> (8 * i)));
    const crypto::Des des(key);
    if (!des.ok()) return false;
    out = crypto::desEcbEncrypt(des, plain);
    out.insert(out.end(), kPasswordSignature, kPasswordSignature + 4);
    const std::uint32_t crc = zip::crc32(plain);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
    return true;
}

bool encryptDistributedStream(ByteView records, bool compressed, ByteView head, Bytes& out) {
    out.clear();
    const DistributeInfo info = decodeDistributeData(head);
    if (!info.ok) return false;
    Bytes body;
    if (compressed) {
        if (!zip::deflateRaw(records, body)) return false;
    } else {
        body.assign(records.begin(), records.end());
    }
    body.resize((body.size() + 15) / 16 * 16, 0);
    const crypto::Aes aes(ByteView(info.key, 16));
    const Bytes cipher = crypto::aesEcbEncrypt(aes, body);
    const std::uint32_t header = kTagDistributeDocData | (256u << 20);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(header >> (8 * i)));
    out.insert(out.end(), head.begin(), head.begin() + 256);
    out.insert(out.end(), cipher.begin(), cipher.end());
    return true;
}

}  // namespace chwp::hwp
