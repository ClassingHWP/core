#include "chwp/render/metafile_preview.hpp"

#include <cstring>
#include <string>

#include "chwp/container/cfb.hpp"

namespace chwp::render {

namespace {

std::uint16_t u16At(ByteView d, std::size_t at) {
    return at + 2 <= d.size() ? static_cast<std::uint16_t>(d[at] | (d[at + 1] << 8)) : 0;
}

std::uint32_t u32At(ByteView d, std::size_t at) {
    return at + 4 <= d.size() ? static_cast<std::uint32_t>(d[at]) | (static_cast<std::uint32_t>(d[at + 1]) << 8) |
                                    (static_cast<std::uint32_t>(d[at + 2]) << 16) |
                                    (static_cast<std::uint32_t>(d[at + 3]) << 24)
                              : 0;
}

void putU32(Bytes& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// DIB(BITMAPINFOHEADER + 색표 + 비트)를 BMP 파일로. 머리가 이상하면 거짓.
bool dibToBmp(ByteView dib, Bytes& bmp) {
    const std::uint32_t headerSize = u32At(dib, 0);
    if (headerSize < 12 || headerSize > dib.size()) return false;
    std::uint32_t paletteBytes = 0;
    if (headerSize >= 40) {
        const std::uint16_t bitCount = u16At(dib, 14);
        const std::uint32_t compression = u32At(dib, 16);
        std::uint32_t colors = u32At(dib, 32);
        if (colors == 0 && bitCount <= 8) colors = 1u << bitCount;
        paletteBytes = colors * 4;
        if (headerSize == 40 && (compression == 3 || compression == 6)) paletteBytes += compression == 3 ? 12 : 16;
    } else {
        const std::uint16_t bitCount = u16At(dib, 10);
        if (bitCount <= 8) paletteBytes = (1u << bitCount) * 3;
    }
    if (headerSize + paletteBytes > dib.size()) return false;
    bmp.clear();
    bmp.push_back('B');
    bmp.push_back('M');
    putU32(bmp, static_cast<std::uint32_t>(14 + dib.size()));
    putU32(bmp, 0);
    putU32(bmp, 14 + headerSize + paletteBytes);
    bmp.insert(bmp.end(), dib.begin(), dib.end());
    return true;
}

int dibBitCount(ByteView dib) {
    const std::uint32_t headerSize = u32At(dib, 0);
    return headerSize >= 40 ? u16At(dib, 14) : headerSize >= 12 ? u16At(dib, 10) : 0;
}

// 여러 비트맵 레코드 가운데 색이 가장 많은 것(1 비트 마스크보다 그림)을 고른다.
struct BestDib {
    ByteView dib;
    int bits = -1;
    void offer(ByteView candidate) {
        const int b = dibBitCount(candidate);
        if (b > bits && u32At(candidate, 0) >= 12) {
            dib = candidate;
            bits = b;
        }
    }
};

bool wmfBitmap(ByteView wmf, Bytes& bmp) {
    std::size_t at = 0;
    if (u32At(wmf, 0) == 0x9AC6CDD7u) at = 22;  // 자리 머리(placeable header)
    if (u16At(wmf, at + 2) != 9) return false;     // 표준 머리 크기(워드)
    at += 18;
    BestDib best;
    while (at + 6 <= wmf.size()) {
        const std::uint32_t words = u32At(wmf, at);
        const std::uint16_t fn = u16At(wmf, at + 4);
        if (words < 3 || at + static_cast<std::size_t>(words) * 2 > wmf.size()) break;
        const ByteView body = wmf.subspan(at + 6, static_cast<std::size_t>(words) * 2 - 6);
        if (fn == 0x0B41 && body.size() > 20 + 40) best.offer(body.subspan(20));        // META_DIBSTRETCHBLT
        else if (fn == 0x0F43 && body.size() > 22 + 40) best.offer(body.subspan(22));   // META_STRETCHDIB
        else if (fn == 0x0940 && body.size() > 16 + 40) best.offer(body.subspan(16));   // META_DIBBITBLT
        else if (fn == 0x0000) break;
        at += static_cast<std::size_t>(words) * 2;
    }
    if (best.bits < 0) return false;
    return dibToBmp(best.dib, bmp);
}

bool emfBitmap(ByteView emf, Bytes& bmp) {
    BestDib best;
    std::size_t at = 0;
    while (at + 8 <= emf.size()) {
        const std::uint32_t type = u32At(emf, at);
        const std::uint32_t size = u32At(emf, at + 4);
        if (size < 8 || at + size > emf.size()) break;
        const ByteView rec = emf.subspan(at, size);
        // EMR_STRETCHDIBITS(81): 색표·비트 자리 48·56. EMR_BITBLT(76)·EMR_STRETCHBLT(77): 84·92.
        std::size_t offBmi = 0, cbBmi = 0, offBits = 0, cbBits = 0;
        if (type == 81 && size >= 80) {
            offBmi = u32At(rec, 48);
            cbBmi = u32At(rec, 52);
            offBits = u32At(rec, 56);
            cbBits = u32At(rec, 60);
        } else if ((type == 76 || type == 77) && size >= 100) {
            offBmi = u32At(rec, 84);
            cbBmi = u32At(rec, 88);
            offBits = u32At(rec, 92);
            cbBits = u32At(rec, 96);
        }
        if (cbBmi >= 12 && cbBits > 0 && offBmi + cbBmi <= size && offBits + cbBits <= size && offBits == offBmi + cbBmi) {
            best.offer(rec.subspan(offBmi, cbBmi + cbBits));
        }
        if (type == 14) break;  // EMR_EOF
        at += size;
    }
    if (best.bits < 0) return false;
    return dibToBmp(best.dib, bmp);
}

// OLEPresentationStream(MS-OLEDS 2.3.4): 클립보드 형식, 대상 장치, aspect·lindex·advf·예약, 폭·높이, 크기, 자료.
bool presentationBitmap(ByteView pres, Bytes& bmp) {
    std::size_t at = 0;
    const std::uint32_t marker = u32At(pres, at);
    at += 4;
    std::uint32_t format = 0;
    if (marker == 0xFFFFFFFFu || marker == 0xFFFFFFFEu) {
        format = u32At(pres, at);
        at += 4;
    } else if (marker != 0) {
        at += marker;  // 이름 있는 형식(ANSI 글자)
    }
    const std::uint32_t target = u32At(pres, at);
    at += target >= 4 ? target : 4;
    at += 4 * 4 + 4 * 2;  // aspect, lindex, advf, reserved, width, height
    const std::uint32_t size = u32At(pres, at);
    at += 4;
    if (at + size > pres.size() || size == 0) return false;
    const ByteView data = pres.subspan(at, size);
    if (format == 8) return dibToBmp(data, bmp);   // CF_DIB
    if (format == 14) return emfBitmap(data, bmp);  // CF_ENHMETAFILE
    return metafileBitmap(data, bmp);               // CF_METAFILEPICT 등
}

}  // namespace

bool metafileBitmap(ByteView metafile, Bytes& bmp) {
    if (metafile.size() >= 44 && u32At(metafile, 0) == 1 && std::memcmp(metafile.data() + 40, " EMF", 4) == 0) {
        return emfBitmap(metafile, bmp);
    }
    return wmfBitmap(metafile, bmp);
}

bool olePreviewBitmap(ByteView ole, Bytes& bmp) {
    static constexpr std::uint8_t kSig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    std::size_t start = 0;
    if (ole.size() >= 12 && std::memcmp(ole.data() + 4, kSig, 8) == 0) start = 4;  // 한글이 붙인 길이
    else if (ole.size() < 8 || std::memcmp(ole.data(), kSig, 8) != 0) return false;
    cfb::CompoundFile file;
    if (!file.open(ole.subspan(start))) return false;
    for (const cfb::Entry& entry : file.entries()) {
        if (entry.type != cfb::EntryType::Stream || entry.name.size() < 8 || entry.name[0] != '\x02' ||
            entry.name.compare(1, 7, "OlePres") != 0) {
            continue;
        }
        Bytes pres;
        if (file.readStream(entry, pres) && presentationBitmap(pres, bmp)) return true;
    }
    return false;
}

}  // namespace chwp::render
