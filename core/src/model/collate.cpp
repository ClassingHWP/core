#include "chwp/model/collate.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "chwp/container/zip.hpp"

namespace chwp::model {

namespace collate_detail {
extern const unsigned char kWindowsCollation[];
extern const std::size_t kWindowsCollationSize;
extern const std::size_t kWindowsCollationRawSize;
}  // namespace collate_detail

namespace {

// 글자 하나의 키 조각. 바이트는 pool 안의 자리다.
struct Piece {
    std::uint32_t p1 = 0;   // 1차(0x30003) 기본 무게
    std::uint32_t p2 = 0;   // 2·3차(문자열 정렬) 기본 무게
    std::uint32_t d = 0;    // 발음 무게(단위마다 하나)
    std::uint32_t c = 0;    // 대소폭 무게(0x1001)
    std::uint32_t c3 = 0;   // 대소폭 무게(0x1000)
    std::uint8_t p1Len = 0;
    std::uint8_t p2Len = 0;
    std::uint8_t dLen = 0;
    std::uint8_t cLen = 0;
    std::uint8_t flags = 0;  // 1 p2 다름, 2 1차 특수, 4 2차 특수, 8 가나, 16 되풀이 표시
    std::uint8_t repDia = 0;
    std::uint16_t s1 = 0;    // 낱말 정렬 특수 무게(1차)
    std::uint16_t s2 = 0;
    std::array<std::uint8_t, 3> kana{};  // 작은 글자·가타카나·반각 값
    std::array<std::uint8_t, 2> vowel{};  // 가나: 뒤 장음 표시가 쓰는 모음 무게(FF FF 면 없음)
    // 되풀이·장음 표시: 가나 뒤에서 모음 무게를 쓰는가, 넷째 목록 값, 반각 값(FF 면 앞 가나 것)
    std::uint8_t markVowel = 0;
    std::uint8_t markList = 3;
    std::uint8_t markWidth = 0xFF;
};

struct Jamo {
    // 옛한글 낱자 번호(없으면 -1). 첫소리 125, 가운뎃소리 95, 끝소리 137.
    std::vector<std::int16_t> lIndex, vIndex, tIndex;
    std::vector<std::uint16_t> lList, vList, tList;
    // 첫+가 합침(현대 짝 제외): [l][v] → AW 2 + X + Y. 합치지 않으면 has 가 거짓.
    std::vector<std::array<std::uint8_t, 4>> lv;
    std::vector<bool> lvHas;
    // 끝소리가 붙을 때 AW 가 바뀌는 곳: (l, v(없으면 0), t) → AW.
    std::unordered_map<std::uint64_t, std::array<std::uint8_t, 2>> lt;
    // 현대 첫+가 + 옛 끝: [l(19)][v(21)] → X, Y 와 옛 끝소리마다 AW(0 이면 합치지 않음).
    std::vector<std::uint8_t> lvt;
    std::vector<std::int16_t> oldTIndex;  // 끝소리 번호 → 옛 끝소리 번호
    std::vector<std::uint8_t> rep;        // 낱자 번호(L,V,T 차례) → 되풀이 표시 무게
};

struct Table {
    std::vector<Piece> pieces;
    std::vector<std::uint8_t> pool;
    Jamo jamo;
    bool ok = false;
};

constexpr std::uint64_t ltKey(std::uint32_t l, std::uint32_t v, std::uint32_t t) {
    return (static_cast<std::uint64_t>(l) << 32) | (static_cast<std::uint64_t>(v) << 16) | t;
}

bool isL(char16_t c) { return (c >= 0x1100 && c <= 0x115F) || (c >= 0xA960 && c <= 0xA97C); }
bool isV(char16_t c) { return (c >= 0x1160 && c <= 0x11A7) || (c >= 0xD7B0 && c <= 0xD7C6); }
bool isT(char16_t c) { return (c >= 0x11A8 && c <= 0x11FF) || (c >= 0xD7CB && c <= 0xD7FB); }
bool modernL(char16_t c) { return c >= 0x1100 && c <= 0x1112; }
bool modernV(char16_t c) { return c >= 0x1161 && c <= 0x1175; }
bool modernT(char16_t c) { return c >= 0x11A8 && c <= 0x11C2; }

const Table& table() {
    static Table t;
    static std::once_flag once;
    std::call_once(once, [] {
        Bytes raw;
        const ByteView packed(collate_detail::kWindowsCollation, collate_detail::kWindowsCollationSize);
        if (!zip::inflateRaw(packed, raw) || raw.size() != collate_detail::kWindowsCollationRawSize ||
            std::memcmp(raw.data(), "CHWPCOL1", 8) != 0) {
            return;
        }
        std::size_t at = 8;
        const std::uint8_t* flags = raw.data() + at;
        at += 0x10000;
        const auto stream = [&](std::size_t& size) {
            std::uint32_t n = 0;
            std::memcpy(&n, raw.data() + at, 4);
            at += 4;
            const std::uint8_t* p = raw.data() + at;
            at += n;
            size = n;
            return p;
        };
        std::size_t n = 0;
        const std::uint8_t* p1len = stream(n);
        const std::uint8_t* p1 = stream(n);
        const std::uint8_t* p2 = stream(n);
        const std::uint8_t* s1 = stream(n);
        const std::uint8_t* s2 = stream(n);
        const std::uint8_t* u = stream(n);
        const std::uint8_t* d = stream(n);
        const std::uint8_t* clen = stream(n);
        const std::uint8_t* c = stream(n);
        const std::uint8_t* kana = stream(n);
        const std::uint8_t* rep = stream(n);
        const std::uint8_t* c3 = stream(n);
        const std::uint8_t* kv = stream(n);
        const std::uint8_t* rk = stream(n);
        t.pieces.resize(0x10000);
        std::size_t o1 = 0, o2 = 0, os1 = 0, os2 = 0, od = 0, oc = 0, ok = 0, orep = 0, okv = 0, ork = 0;
        const auto put = [&](const std::uint8_t* src, std::size_t len) {
            const auto off = static_cast<std::uint32_t>(t.pool.size());
            t.pool.insert(t.pool.end(), src, src + len);
            return off;
        };
        for (std::size_t cp = 0; cp < 0x10000; ++cp) {
            Piece& pc = t.pieces[cp];
            pc.flags = flags[cp];
            pc.p1Len = p1len[cp];
            pc.p1 = put(p1 + o1, pc.p1Len);
            o1 += pc.p1Len;
            if (pc.flags & 1) {
                pc.p2Len = p2[o2];
                pc.p2 = put(p2 + o2 + 1, pc.p2Len);
                o2 += 1 + pc.p2Len;
            } else {
                pc.p2 = pc.p1;
                pc.p2Len = pc.p1Len;
            }
            if (pc.flags & 2) {
                pc.s1 = static_cast<std::uint16_t>((s1[os1] << 8) | s1[os1 + 1]);
                os1 += 2;
            }
            if (pc.flags & 4) {
                pc.s2 = static_cast<std::uint16_t>((s2[os2] << 8) | s2[os2 + 1]);
                os2 += 2;
            }
            pc.dLen = u[cp];
            pc.d = put(d + od, pc.dLen);
            od += pc.dLen;
            pc.cLen = clen[cp];
            pc.c = put(c + oc, pc.cLen);
            pc.c3 = put(c3 + oc, pc.cLen);
            oc += pc.cLen;
            if (pc.flags & 8) {
                pc.kana = {kana[ok], kana[ok + 1], kana[ok + 2]};
                ok += 3;
                pc.vowel = {kv[okv], kv[okv + 1]};
                okv += 2;
            }
            if (pc.flags & 16) {
                pc.repDia = rep[orep++];
                pc.markVowel = rk[ork];
                pc.markList = rk[ork + 1];
                pc.markWidth = rk[ork + 2];
                ork += 3;
            }
        }
        Jamo& j = t.jamo;
        for (std::uint16_t cp = 0x1100; cp <= 0x115F; ++cp) j.lList.push_back(cp);
        for (std::uint16_t cp = 0xA960; cp <= 0xA97C; ++cp) j.lList.push_back(cp);
        for (std::uint16_t cp = 0x1160; cp <= 0x11A7; ++cp) j.vList.push_back(cp);
        for (std::uint16_t cp = 0xD7B0; cp <= 0xD7C6; ++cp) j.vList.push_back(cp);
        for (std::uint16_t cp = 0x11A8; cp <= 0x11FF; ++cp) j.tList.push_back(cp);
        for (std::uint16_t cp = 0xD7CB; cp <= 0xD7FB; ++cp) j.tList.push_back(cp);
        j.lIndex.assign(0x10000, -1);
        j.vIndex.assign(0x10000, -1);
        j.tIndex.assign(0x10000, -1);
        for (std::size_t i = 0; i < j.lList.size(); ++i) j.lIndex[j.lList[i]] = static_cast<std::int16_t>(i);
        for (std::size_t i = 0; i < j.vList.size(); ++i) j.vIndex[j.vList[i]] = static_cast<std::int16_t>(i);
        for (std::size_t i = 0; i < j.tList.size(); ++i) j.tIndex[j.tList[i]] = static_cast<std::int16_t>(i);
        std::int16_t old = 0;
        j.oldTIndex.assign(j.tList.size(), -1);
        for (std::size_t i = 0; i < j.tList.size(); ++i) {
            if (!modernT(j.tList[i])) j.oldTIndex[i] = old++;
        }
        const std::uint8_t* lv = stream(n);
        const std::size_t lvSize = n;
        j.lv.assign(j.lList.size() * j.vList.size(), {});
        j.lvHas.assign(j.lList.size() * j.vList.size(), false);
        std::size_t olv = 0;
        for (std::size_t li = 0; li < j.lList.size(); ++li) {
            for (std::size_t vi = 0; vi < j.vList.size(); ++vi) {
                if (modernL(j.lList[li]) && modernV(j.vList[vi])) continue;
                if (olv + 4 > lvSize) return;
                j.lv[li * j.vList.size() + vi] = {lv[olv], lv[olv + 1], lv[olv + 2], lv[olv + 3]};
                j.lvHas[li * j.vList.size() + vi] = true;
                olv += 4;
            }
        }
        std::uint32_t nlt = 0;
        std::memcpy(&nlt, raw.data() + at, 4);
        at += 4;
        for (std::uint32_t i = 0; i < nlt; ++i, at += 8) {
            std::uint16_t l = 0, v = 0, tt = 0;
            std::memcpy(&l, raw.data() + at, 2);
            std::memcpy(&v, raw.data() + at + 2, 2);
            std::memcpy(&tt, raw.data() + at + 4, 2);
            j.lt[ltKey(l, v, tt)] = {raw[at + 6], raw[at + 7]};
        }
        const std::uint8_t* lvt = stream(n);
        j.lvt.assign(lvt, lvt + n);
        std::uint32_t nrep = 0;
        std::memcpy(&nrep, raw.data() + at, 4);
        at += 4;
        j.rep.assign(j.lList.size() + j.vList.size() + j.tList.size(), 0);
        for (std::uint32_t i = 0; i < nrep && i < j.rep.size(); ++i, at += 3) j.rep[i] = raw[at + 2];
        t.ok = at <= raw.size();
    });
    return t;
}

// 낱자 번호(L, V, T 차례로 이은 번호). 되풀이 표시 표가 이 차례다.
int jamoOrder(const Jamo& j, char16_t c) {
    if (j.lIndex[c] >= 0) return j.lIndex[c];
    if (j.vIndex[c] >= 0) return static_cast<int>(j.lList.size()) + j.vIndex[c];
    if (j.tIndex[c] >= 0) return static_cast<int>(j.lList.size() + j.vList.size()) + j.tIndex[c];
    return -1;
}

}  // namespace

std::string windowsSortKey(std::u16string_view s, CollateFlags flags) {
    const Table& tb = table();
    if (!tb.ok) return {};
    const bool first = flags == CollateFlags::IgnoreCaseAccentKanaWidth;
    const Jamo& jm = tb.jamo;
    const std::uint8_t* pool = tb.pool.data();
    std::string prim;
    std::vector<std::uint8_t> dia;
    std::vector<std::uint8_t> cas;
    std::string special;
    int kanaFirst = 0;
    std::vector<std::array<std::uint8_t, 4>> kana;  // 작은·가타카나·반각·넷째 목록
    // 바로 앞 단위가 가나(또는 가나 뒤 표시)면 그 넷째 칸 값과 모음을 정할 가나
    bool kanaBefore = false;
    std::array<std::uint8_t, 3> kanaLast{};
    char16_t kanaVowelOf = 0;
    std::string last;  // 되풀이 표시가 따라 쓸 앞 무게
    int pending = -1;  // 앞 글자 없는 결합 표시 무게 합
    char16_t lastJamo = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const char16_t cp = s[i++];
        const Piece& pc = tb.pieces[cp];
        std::string pr = first ? std::string(reinterpret_cast<const char*>(pool + pc.p1), pc.p1Len)
                               : std::string(reinterpret_cast<const char*>(pool + pc.p2), pc.p2Len);
        if (isL(cp) || isV(cp)) {
            // 옛한글: 첫소리+가운뎃소리(둘 다 현대 낱자가 아니면)는 한 요소이고, 바로 뒤 끝소리는 그 요소의 끝 무게다.
            // 현대 첫소리+가운뎃소리 뒤에 옛 끝소리가 오면 셋이 한 요소다.
            std::string el = pr;
            bool done = false;
            char16_t srcV = 0;
            if (isL(cp) && modernL(cp) && i + 1 < s.size() && modernV(s[i]) && isT(s[i + 1]) && !modernT(s[i + 1])) {
                const int li = cp - 0x1100;
                const int vi = s[i] - 0x1161;
                const std::uint8_t* rec = jm.lvt.data() + (li * 21 + vi) * (2 + 2 * 110);
                const int oi = jm.oldTIndex[static_cast<std::size_t>(jm.tIndex[s[i + 1]])];
                const std::uint8_t a0 = rec[2 + oi * 2];
                const std::uint8_t a1 = rec[2 + oi * 2 + 1];
                if (a0 || a1) {
                    const char16_t t = s[i + 1];
                    const Piece& tp = tb.pieces[t];
                    el = std::string{static_cast<char>(a0), static_cast<char>(a1), '\xff', static_cast<char>(rec[0]),
                                     '\xff', static_cast<char>(rec[1]), '\xff',
                                     static_cast<char>(pool[tp.p1 + tp.p1Len - 1])};
                    i += 2;
                    done = true;
                }
            }
            if (!done && isL(cp) && i < s.size() && jm.vIndex[s[i]] >= 0) {
                const std::size_t k = static_cast<std::size_t>(jm.lIndex[cp]) * jm.vList.size() + static_cast<std::size_t>(jm.vIndex[s[i]]);
                if (jm.lvHas[k]) {
                    const auto& e = jm.lv[k];
                    srcV = s[i];
                    el = std::string{static_cast<char>(e[0]), static_cast<char>(e[1]), '\xff', static_cast<char>(e[2]),
                                     '\xff', static_cast<char>(e[3]), '\xff', '\x02'};
                    ++i;
                }
            }
            if (!done && i < s.size() && isT(s[i]) && el.size() == 8) {
                const char16_t t = s[i];
                const Piece& tp = tb.pieces[t];
                el[7] = static_cast<char>(pool[tp.p1 + tp.p1Len - 1]);
                if (isL(cp)) {
                    if (const auto it = jm.lt.find(ltKey(cp, srcV, t)); it != jm.lt.end()) {
                        el[0] = static_cast<char>(it->second[0]);
                        el[1] = static_cast<char>(it->second[1]);
                    }
                }
                ++i;
            }
            pr = el;
        }
        const bool isSpecial = first ? (pc.flags & 2) != 0 : (pc.flags & 4) != 0;
        if (isSpecial) {
            // 낱말 정렬에서 빠지는 글자(- ' 등)는 특수 칸에 앞 단위 수로 자리를 적는다
            const std::uint16_t w = first ? pc.s1 : pc.s2;
            const unsigned pos = 0xFFFFu - static_cast<unsigned>(prim.size() / 2);
            special += static_cast<char>((pos >> 8) & 0xFF);
            special += static_cast<char>(pos & 0xFF);
            special += static_cast<char>(w >> 8);
            special += static_cast<char>(w & 0xFF);
            last.assign(reinterpret_cast<const char*>(pool + pc.p2), pc.p2Len);
            lastJamo = 0;
            continue;
        }
        const bool repeat = pr.size() == 2 && static_cast<std::uint8_t>(pr[0]) == 0xFF && static_cast<std::uint8_t>(pr[1]) == 0xFF;
        const bool hadLast = !last.empty() || lastJamo != 0;
        const bool kanaMark = repeat && kanaBefore;
        if (repeat && lastJamo) {
            const int o = jamoOrder(jm, lastJamo);
            pr = std::string{'\x04', static_cast<char>(o >= 0 ? jm.rep[static_cast<std::size_t>(o)] : 0)};
        } else if (kanaMark && pc.markVowel && tb.pieces[kanaVowelOf].vowel[0] != 0xFF) {
            // 가나 뒤 장음 표시는 그 가나의 모음(아·이·우·에·오 행) 무게다
            const auto& v = tb.pieces[kanaVowelOf].vowel;
            pr = std::string{static_cast<char>(v[0]), static_cast<char>(v[1])};
        } else if (repeat && !last.empty()) {
            pr = last;
        }
        std::vector<std::uint8_t> d(pool + pc.d, pool + pc.d + pc.dLen);
        const std::uint32_t cOff = flags == CollateFlags::String ? pc.c3 : pc.c;
        std::vector<std::uint8_t> c(pool + cOff, pool + cOff + pc.cLen);
        if (pr.empty() && pc.dLen == 1) {
            // 결합 표시: 앞 단위의 발음 무게에 더한다. 앞 단위가 없으면 모아 두었다가 따로 한 칸을 차지한다
            if (!dia.empty()) {
                dia.back() = static_cast<std::uint8_t>(dia.back() + d[0]);
            } else {
                pending = (pending < 0 ? 0 : pending) + d[0];
            }
            continue;
        }
        if (isL(cp) || isV(cp) || isT(cp)) {
            lastJamo = s[i - 1];
        } else if (!pr.empty() && !repeat) {
            lastJamo = 0;
        }
        if (pr.empty() && d.empty() && last.size() == 3) last[2] = '\0';
        prim += pr;
        if (!pr.empty() && !(pr.size() == 2 && static_cast<std::uint8_t>(pr[0]) == 0xFF && static_cast<std::uint8_t>(pr[1]) == 0xFF)) {
            // 되풀이 표시는 따라 쓸 무게를 바꾸지 않는다(가나 장음 뒤 ゝ 도 앞 가나 무게다)
            if (!repeat) last = (pr.size() == 2 || pr.size() == 3) ? pr : std::string();
        }
        if (repeat) {
            d = hadLast ? std::vector<std::uint8_t>{pc.repDia} : std::vector<std::uint8_t>{};
            c = hadLast ? std::vector<std::uint8_t>{2} : std::vector<std::uint8_t>{};
        }
        if (pending >= 0) {
            dia.push_back(static_cast<std::uint8_t>(pending & 0xFF));
            pending = -1;
        }
        dia.insert(dia.end(), d.begin(), d.end());
        cas.insert(cas.end(), c.begin(), c.end());
        if (first && !pr.empty() && static_cast<std::uint8_t>(pr[0]) == 0x22) ++kanaFirst;
        if (kanaMark) {
            // 가나 뒤 되풀이·장음 표시: 작은·가타카나 값은 앞 가나 것, 반각은 표시마다(장음은 자기 값), 넷째 목록 값은 표시 것
            const std::array<std::uint8_t, 3> v{kanaLast[0], kanaLast[1],
                                                 pc.markWidth == 0xFF ? kanaLast[2] : pc.markWidth};
            if (!first) kana.push_back({v[0], v[1], v[2], pc.markList});
        } else if (pc.flags & 8) {
            if (!first) kana.push_back({pc.kana[0], pc.kana[1], pc.kana[2], 3});
            kanaBefore = true;
            kanaLast = pc.kana;
            kanaVowelOf = cp;
        } else if (!pr.empty()) {
            kanaBefore = false;
        }
    }
    if (pending >= 0) dia.push_back(static_cast<std::uint8_t>(pending & 0xFF));
    std::string key = prim;
    key += '\x01';
    if (!first) {
        while (!dia.empty() && dia.back() <= 2) dia.pop_back();
        key.append(dia.begin(), dia.end());
    }
    key += '\x01';
    if (!first) {
        while (!cas.empty() && cas.back() <= 2) cas.pop_back();
        key.append(cas.begin(), cas.end());
    }
    key += '\x01';
    if (!first && !kana.empty()) {
        // 작은 글자 목록 FF 넷째 목록 02 가타카나 목록 FF 반각 목록 FF. 목록 끝의 기본값은 뺀다.
        static constexpr std::uint8_t kDefaults[4] = {0xC6, 0xE4, 0xC5, 0x03};
        std::string lists[4];
        for (int j = 0; j < 4; ++j) {
            for (const auto& k : kana) lists[j] += static_cast<char>(k[static_cast<std::size_t>(j)]);
            while (!lists[j].empty() && static_cast<std::uint8_t>(lists[j].back()) == kDefaults[j]) lists[j].pop_back();
        }
        key += lists[0];
        key += '\xff';
        key += lists[3];
        key += '\x02';
        key += lists[1];
        key += '\xff';
        key += lists[2];
        key += '\xff';
    }
    if (first && kanaFirst) {
        key += "\xff\x02";
        key.append(static_cast<std::size_t>(kanaFirst), '\xc4');
        key += '\xff';
        key.append(static_cast<std::size_t>(kanaFirst), '\xc4');
        key += '\xff';
    }
    key += '\x01';
    key += special;
    key += '\0';
    return key;
}

int windowsCompare(std::u16string_view a, std::u16string_view b, CollateFlags flags) {
    const std::string ka = windowsSortKey(a, flags);
    const std::string kb = windowsSortKey(b, flags);
    const int r = std::memcmp(ka.data(), kb.data(), std::min(ka.size(), kb.size()));
    if (r) return r < 0 ? -1 : 1;
    return ka.size() < kb.size() ? -1 : (ka.size() > kb.size() ? 1 : 0);
}

}  // namespace chwp::model
