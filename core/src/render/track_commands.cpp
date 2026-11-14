// 변경 내용 추적(한글 2024 검토 > 변경 내용 추적). 한글 화면·파일로 잰 동작:
// - 기록 중(머리 trackchageConfig 1 번 비트)에 친 글은 "넣은 글", 지운 글은 지우지 않고 "지운 글"로 남는다.
//   넣은 글을 다시 지우면 정말 지운다. Delete 는 캐럿을 지운 글 뒤로, Backspace 는 앞으로 옮긴다.
//   Enter 로 나눈 문단은 앞 문단 끝이 "넣은 문단 끝"이다.
// - 같은 작성자의 같은 종류 바꿈은 머리 기록 하나(TcId)를 함께 쓰고, 이어진 글 조각마다 쌍 id 가 있다.
// - 화면(최종본 및 변경 내용): 넣은 글은 빨간 글자에 빨간 밑줄, 지운 글은 빨간 글자에 빨간 취소선이다.
// - 적용: 넣은 글은 그대로 두고 표시만 없애고, 지운 글은 정말 지운다. 취소는 그 반대다. 문단 끝도 같다(지운 문단 끝을
//   적용하거나 넣은 문단 끝을 취소하면 다음 문단과 합친다).
// - 다음·이전은 그 바뀜을 블록으로 고르고, 끝(처음)을 지나면 묻지 않고 처음(끝) 바뀜으로 돈다.
// - 적용·취소는 블록이면 블록을 그대로 두고, 아니면 캐럿 자리 바뀜 하나를 하고 캐럿을 그 맨 앞에 둔다. "적용 후 다음으로
//   이동"(검토 탭 큰 단추)은 그다음 바뀜을 고르고, 캐럿 자리에 바뀜이 없으면 다음 바뀜을 고르기만 한다.
//   쓰지 않게 된 바뀜 기록은 지우고 1 부터 다시 매긴다.
// 아직 없는 것: 보기 모드(원본·최종본), 글자 모양 바꿈 기록(파일에서 읽어 그대로 저장만 한다).

#include "chwp/render/track_commands.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

std::mutex& authorLock() {
    static std::mutex lock;
    return lock;
}
std::string& authorSlot() {
    static std::string name;
    return name;
}

std::string nowIso() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:00Z", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min);
    return buf;
}

std::uint32_t authorIdOf(model::Document& doc) {
    const std::string name = trackAuthorName();
    std::uint32_t maxId = 0;
    for (const model::TrackAuthor& a : doc.header.trackAuthors) {
        if (a.name == name) return a.id;
        maxId = std::max(maxId, a.id);
    }
    model::TrackAuthor author;
    author.id = maxId + 1;
    author.name = name;
    doc.header.trackAuthors.push_back(author);
    return author.id;
}

// 이 작성자의 type 기록(없으면 만든다).
std::uint32_t changeRecord(model::Document& doc, const char* type) {
    const std::uint32_t author = authorIdOf(doc);
    std::uint32_t maxId = 0;
    for (const model::TrackChange& tc : doc.header.trackChanges) {
        if (tc.type == type && tc.authorId == author) return tc.id;
        maxId = std::max(maxId, tc.id);
    }
    model::TrackChange tc;
    tc.id = maxId + 1;
    tc.type = type;
    tc.date = nowIso();
    tc.authorId = author;
    doc.header.trackChanges.push_back(tc);
    return tc.id;
}

std::uint32_t nextMarkId(model::Document& doc) {
    std::uint32_t maxId = 0;
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        maxId = std::max(maxId, para.endTrack.mark);
        for (const model::Run& run : para.runs) {
            for (const model::InlineItem& item : run.items) maxId = std::max(maxId, item.trackMark);
        }
    });
    return maxId + 1;
}

int itemChars(const model::InlineItem& item) {
    if (item.type == model::InlineItem::Type::Text) return static_cast<int>(utf8CodePointCount(item.text));
    return isCaretCharControl(item.control) ? 1 : 0;
}

// 글 조각 text 의 글자 번호 at 자리 바이트.
std::size_t byteAt(const std::string& text, int at) {
    std::size_t i = 0;
    for (int n = 0; n < at && i < text.size(); ++n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    }
    return i;
}

// [a, b) 글자 구간 경계에서 글 조각을 나눈다.
void splitAt(model::Paragraph& para, int a, int b) {
    for (model::Run& run : para.runs) {
        std::vector<model::InlineItem> out;
        out.reserve(run.items.size() + 2);
        int pos = 0;
        for (model::InlineItem& item : run.items) {
            const int len = itemChars(item);
            if (item.type != model::InlineItem::Type::Text || len <= 1) {
                pos += len;
                out.push_back(std::move(item));
                continue;
            }
            std::vector<int> cuts;
            for (const int cut : {a, b}) {
                if (cut > pos && cut < pos + len && (cuts.empty() || cuts.back() != cut - pos)) cuts.push_back(cut - pos);
            }
            int from = 0;
            for (const int cut : cuts) {
                model::InlineItem piece = item;
                piece.text = item.text.substr(byteAt(item.text, from), byteAt(item.text, cut) - byteAt(item.text, from));
                out.push_back(std::move(piece));
                from = cut;
            }
            model::InlineItem rest = item;
            rest.text = item.text.substr(byteAt(item.text, from));
            out.push_back(std::move(rest));
            pos += len;
        }
        run.items = std::move(out);
    }
}

// [a, b) 안의 글 조각마다 fn. fn 이 참이면 그 조각을 지운다. 지운 글자 수를 돌려준다.
int forRange(model::Paragraph& para, int a, int b, const std::function<bool(model::InlineItem&)>& fn) {
    if (b <= a) return 0;
    splitAt(para, a, b);
    int removed = 0;
    int pos = 0;
    for (model::Run& run : para.runs) {
        std::vector<model::InlineItem> keep;
        keep.reserve(run.items.size());
        for (model::InlineItem& item : run.items) {
            const int len = itemChars(item);
            const bool inside = len > 0 && pos >= a && pos + len <= b;
            pos += len;
            if (inside && fn(item)) {
                removed += len;
                continue;
            }
            keep.push_back(std::move(item));
        }
        run.items = std::move(keep);
    }
    para.lineSegs.clear();
    return removed;
}

// 글자 번호 at 바로 앞 글 조각(넣은 글을 이어 쓸 때 쌍 id 를 이어 받는다).
const model::InlineItem* itemBefore(const model::Paragraph& para, int at) {
    const model::InlineItem* found = nullptr;
    int pos = 0;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            const int len = itemChars(item);
            if (len > 0 && pos + len == at) found = &item;
            pos += len;
        }
    }
    return found;
}

// 문단 [a, b) 를 지운 글로 표시한다(넣은 글은 정말 지운다). 지운 글자 수를 돌려준다.
int markDeleted(model::Document& doc, model::Paragraph& para, int a, int b) {
    const std::uint32_t change = changeRecord(doc, "Delete");
    std::uint32_t mark = 0;
    return forRange(para, a, b, [&](model::InlineItem& item) {
        if (item.track == model::kTrackInsert) return true;
        if (item.track == model::kTrackDelete) return false;
        if (mark == 0) mark = nextMarkId(doc);
        item.track = model::kTrackDelete;
        item.trackChange = change;
        item.trackMark = mark;
        return false;
    });
}

EditRelayoutResult relayoutTracked(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult full;
    full.fullInvalidation = true;
    full.pageListChanged = true;
    return full;
}

// 바뀜 하나를 적용·취소한다. 지울 조각이면 참이다(조각의 표시는 여기서 지운다).
bool resolveItem(model::InlineItem& item, bool accept) {
    const bool removeIt = accept ? item.track == model::kTrackDelete : item.track == model::kTrackInsert;
    item.track = 0;
    item.trackChange = 0;
    item.trackMark = 0;
    return removeIt;
}

// story 문단들의 [pa:ca, pb:cb) 안 바뀜(또는 all 이면 모두)을 적용·취소한다. 바뀐 것이 있으면 참이다.
bool resolveIn(std::vector<model::Paragraph>& paras, int pa, int ca, int pb, int cb, bool accept, bool& merged) {
    bool any = false;
    for (int p = pb; p >= pa; --p) {
        model::Paragraph& para = paras[static_cast<std::size_t>(p)];
        const int a = p == pa ? ca : 0;
        const int b = p == pb ? cb : paragraphCharCount(para);
        bool touched = false;
        forRange(para, a, b, [&](model::InlineItem& item) {
            if (item.track == 0) return false;
            touched = true;
            return resolveItem(item, accept);
        });
        any = any || touched;
        // 문단 끝: 구간이 이 문단 끝을 지나면.
        const bool endInside = p < pb;
        if (endInside && para.endTrack.track != 0 && p + 1 < static_cast<int>(paras.size())) {
            const bool join = accept ? para.endTrack.track == model::kTrackDelete : para.endTrack.track == model::kTrackInsert;
            para.endTrack = model::ParaEndTrack{};
            any = true;
            if (join) {
                mergeParagraphs(para, paras[static_cast<std::size_t>(p + 1)]);
                paras.erase(paras.begin() + p + 1);
                merged = true;
            }
        }
    }
    return any;
}

// 캐럿 자리의 바뀜(같은 종류·쌍 id 로 이어진 조각) 글자 구간. 없으면 거짓.
bool changeAround(const model::Paragraph& para, int at, int& a, int& b) {
    struct Piece {
        int from;
        int to;
        std::uint8_t track;
        std::uint32_t mark;
    };
    std::vector<Piece> pieces;
    int pos = 0;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            const int len = itemChars(item);
            if (len > 0) pieces.push_back(Piece{pos, pos + len, item.track, item.trackMark});
            pos += len;
        }
    }
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const Piece& p = pieces[i];
        if (p.track == 0 || !(at >= p.from && at <= p.to)) continue;
        std::size_t lo = i;
        std::size_t hi = i;
        while (lo > 0 && pieces[lo - 1].track == p.track && pieces[lo - 1].mark == p.mark) --lo;
        while (hi + 1 < pieces.size() && pieces[hi + 1].track == p.track && pieces[hi + 1].mark == p.mark) ++hi;
        a = pieces[lo].from;
        b = pieces[hi].to;
        return true;
    }
    return false;
}

}  // namespace

bool trackRecording(const model::Document& doc) { return (doc.header.trackConfigFlags & 1u) != 0; }

void setTrackAuthorName(const std::string& name) {
    std::lock_guard<std::mutex> guard(authorLock());
    authorSlot() = name;
}

std::string trackAuthorName() {
    std::lock_guard<std::mutex> guard(authorLock());
    return authorSlot().empty() ? std::string("사용자") : authorSlot();
}

bool documentHasChanges(const model::Document& doc) {
    bool any = false;
    model::forEachParagraph(const_cast<model::Document&>(doc), [&](model::Paragraph& para) {
        if (para.endTrack.track != 0) any = true;
        for (const model::Run& run : para.runs) {
            for (const model::InlineItem& item : run.items) any = any || item.track != 0;
        }
    });
    return any;
}

EditResult editToggleTrackChanges(model::Document& doc, std::vector<Node>& pages) {
    doc.header.trackConfigFlags ^= 1u;
    const PointerCursor cur = pointerCursor();
    (void)pages;
    EditResult result = makeEditResult(doc, true, true, cur.active ? cur.story : 1, cur.active ? cur.paragraph : 0);
    return result;
}

// ---- 기록(undo.cpp 의 입력·지우기 입구가 부른다)

bool paint_detail::trackDeleteNow(model::Document& doc, std::vector<Node>& pages, EditKey key, bool useStoredLineHeight,
                                  EditResult& out) {
    if (!trackRecording(doc) || readOnly()) return false;
    if (key != EditKey::Delete && key != EditKey::Backspace) return false;
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return false;
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || paras->empty()) return false;
    PointerHold& hold = pointerHold();
    if (hold.composing) clearComposition(hold);
    int caretPara = cur.paragraph;
    int caretChar = 0;
    if (cur.hasSelection) {
        const int pa = cur.startParagraph;
        const int pb = cur.endParagraph;
        const int ca = offsetToCharIndex(doc, pages, cur.story, pa, cur.startOffset);
        const int cb = offsetToCharIndex(doc, pages, cur.story, pb, cur.endOffset);
        int removedLast = 0;
        for (int p = pa; p <= pb; ++p) {
            model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
            const int a = p == pa ? ca : 0;
            const int b = p == pb ? cb : paragraphCharCount(para);
            const int removed = markDeleted(doc, para, a, b);
            if (p == pb) removedLast = removed;
            if (p < pb && para.endTrack.track != model::kTrackDelete) {
                para.endTrack = model::ParaEndTrack{model::kTrackDelete, changeRecord(doc, "Delete"), nextMarkId(doc)};
            }
        }
        // Delete 는 지운 글 뒤, Backspace 는 앞이다.
        caretPara = key == EditKey::Delete ? pb : pa;
        caretChar = key == EditKey::Delete ? cb - (pa == pb ? removedLast : 0) : ca;
        if (key == EditKey::Delete && pa != pb) caretChar = cb - removedLast;
    } else {
        model::Paragraph& para = (*paras)[static_cast<std::size_t>(cur.paragraph)];
        const int at = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
        const int count = paragraphCharCount(para);
        if (key == EditKey::Delete) {
            if (at >= count) {
                // 문단 끝: 지운 문단 끝으로 표시하고 다음 문단 처음으로 간다.
                if (cur.paragraph + 1 >= static_cast<int>(paras->size())) return false;
                if (para.endTrack.track == model::kTrackInsert) return false;  // 넣은 문단 끝은 정말 합친다
                para.endTrack = model::ParaEndTrack{model::kTrackDelete, changeRecord(doc, "Delete"), nextMarkId(doc)};
                caretPara = cur.paragraph + 1;
                caretChar = 0;
            } else {
                const int removed = markDeleted(doc, para, at, at + 1);
                caretChar = removed > 0 ? at : at + 1;
            }
        } else {
            if (at <= 0) {
                if (cur.paragraph == 0) return false;
                model::Paragraph& prev = (*paras)[static_cast<std::size_t>(cur.paragraph - 1)];
                if (prev.endTrack.track == model::kTrackInsert) return false;
                prev.endTrack = model::ParaEndTrack{model::kTrackDelete, changeRecord(doc, "Delete"), nextMarkId(doc)};
                caretPara = cur.paragraph - 1;
                caretChar = paragraphCharCount(prev);
            } else {
                markDeleted(doc, para, at - 1, at);
                caretChar = at - 1;
            }
        }
    }
    const EditRelayoutResult relayout = relayoutTracked(doc, pages, useStoredLineHeight);
    hold.preferredX = -1;
    setPointerCursor(doc, pages,
                     model::CursorPos{cur.story, caretPara, charIndexToOffset(doc, pages, cur.story, caretPara, caretChar)});
    out = makeEditResult(doc, true, true, cur.story, caretPara, &relayout);
    return true;
}

void paint_detail::trackMarkInserted(model::Document& doc, std::vector<Node>& pages, int story, int paraFrom,
                                     int charFrom, bool splitParagraphs) {
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    const PointerCursor cur = pointerCursor();
    if (!paras || !cur.active || cur.story != story) return;
    PointerHold& hold = pointerHold();
    // 조합 중이면 조합 글 끝까지다.
    int paraTo = cur.paragraph;
    int charTo = hold.composing && hold.compStory == story ? hold.compEnd
                                                         : offsetToCharIndex(doc, pages, story, cur.paragraph, cur.offset);
    if (hold.composing && hold.compStory == story) paraTo = hold.compPara;
    if (paraTo < paraFrom || paraTo >= static_cast<int>(paras->size())) return;
    const bool record = trackRecording(doc);
    const std::uint32_t change = record ? changeRecord(doc, "Insert") : 0;
    for (int p = paraFrom; p <= paraTo; ++p) {
        model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
        const int a = p == paraFrom ? charFrom : 0;
        const int b = p == paraTo ? charTo : paragraphCharCount(para);
        std::uint32_t mark = 0;
        if (record) {
            const model::InlineItem* before = itemBefore(para, a);
            mark = before && before->track == model::kTrackInsert && before->trackChange == change ? before->trackMark
                                                                                                 : nextMarkId(doc);
        }
        forRange(para, a, b, [&](model::InlineItem& item) {
            item.track = record ? model::kTrackInsert : 0;
            item.trackChange = change;
            item.trackMark = mark;
            return false;
        });
        if (record && splitParagraphs && p < paraTo) {
            para.endTrack = model::ParaEndTrack{model::kTrackInsert, change, nextMarkId(doc)};
        }
    }
}

// ---- 적용·취소·이동

namespace {

// 바뀜 기록 가운데 문서가 더 쓰지 않는 것을 지우고 1 부터 다시 매긴다(한글은 적용·취소 뒤 이렇게 저장한다).
void pruneTrackRecords(model::Document& doc) {
    std::vector<std::uint32_t> used;
    const auto use = [&](std::uint32_t id) {
        if (id != 0 && std::find(used.begin(), used.end(), id) == used.end()) used.push_back(id);
    };
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        if (para.endTrack.track != 0) use(para.endTrack.change);
        for (const model::Run& run : para.runs) {
            use(run.charTrackChange);
            for (const model::InlineItem& item : run.items) {
                if (item.track != 0) use(item.trackChange);
            }
        }
    });
    std::vector<model::TrackChange> kept;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> remap;
    for (model::TrackChange tc : doc.header.trackChanges) {
        if (std::find(used.begin(), used.end(), tc.id) == used.end()) continue;
        const std::uint32_t next = static_cast<std::uint32_t>(kept.size() + 1);
        remap.emplace_back(tc.id, next);
        tc.id = next;
        kept.push_back(std::move(tc));
    }
    const auto mapped = [&](std::uint32_t id) {
        for (const auto& [from, to] : remap) {
            if (from == id) return to;
        }
        return id;
    };
    doc.header.trackChanges = std::move(kept);
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        if (para.endTrack.track != 0) para.endTrack.change = mapped(para.endTrack.change);
        for (model::Run& run : para.runs) {
            if (run.charTrackChange != 0) run.charTrackChange = mapped(run.charTrackChange);
            for (model::InlineItem& item : run.items) {
                if (item.track != 0) item.trackChange = mapped(item.trackChange);
            }
        }
    });
}

// 글 묶음의 바뀜 조각(같은 종류·쌍 id 로 이어진 글자 구간)을 문서 차례로.
struct ChangeSpan {
    int paragraph = 0;
    int from = 0;
    int to = 0;
};
std::vector<ChangeSpan> changeSpans(const std::vector<model::Paragraph>& paras) {
    std::vector<ChangeSpan> out;
    for (std::size_t p = 0; p < paras.size(); ++p) {
        int pos = 0;
        std::uint8_t lastTrack = 0;
        std::uint32_t lastMark = 0;
        for (const model::Run& run : paras[p].runs) {
            for (const model::InlineItem& item : run.items) {
                const int len = itemChars(item);
                if (len <= 0) continue;
                if (item.track != 0) {
                    const bool joins = !out.empty() && out.back().paragraph == static_cast<int>(p) && out.back().to == pos &&
                                       lastTrack == item.track && lastMark == item.trackMark;
                    if (joins) out.back().to = pos + len;
                    else out.push_back(ChangeSpan{static_cast<int>(p), pos, pos + len});
                }
                lastTrack = item.track;
                lastMark = item.trackMark;
                pos += len;
            }
        }
    }
    return out;
}

// 캐럿(블록) 자리의 바뀜을 적용·취소한다. 했으면 참이고, 캐럿을 둘 자리를 준다.
bool resolveAtCaret(model::Document& doc, std::vector<Node>& pages, bool accept, int& story, int& para, int& from,
                    int& to) {
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return false;
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras) return false;
    story = cur.story;
    bool merged = false;
    if (cur.hasSelection) {
        const int ca = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
        const int cb = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
        const int before = paragraphCharCount((*paras)[static_cast<std::size_t>(cur.endParagraph)]);
        if (!resolveIn(*paras, cur.startParagraph, ca, cur.endParagraph, cb, accept, merged)) return false;
        // 블록은 그대로 둔다(한글). 지운 글자만큼 끝이 당겨진다.
        para = cur.startParagraph;
        from = ca;
        to = cur.startParagraph == cur.endParagraph && !merged
                 ? cb - (before - paragraphCharCount((*paras)[static_cast<std::size_t>(cur.endParagraph)]))
                 : ca;
        return true;
    }
    model::Paragraph& here = (*paras)[static_cast<std::size_t>(cur.paragraph)];
    const int at = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    int a = 0;
    int b = 0;
    bool done = false;
    if (changeAround(here, at, a, b)) {
        done = resolveIn(*paras, cur.paragraph, a, cur.paragraph, b, accept, merged);
    } else if (at >= paragraphCharCount(here) && here.endTrack.track != 0 &&
               cur.paragraph + 1 < static_cast<int>(paras->size())) {
        a = at;
        done = resolveIn(*paras, cur.paragraph, at, cur.paragraph + 1, 0, accept, merged);
    }
    if (!done) return false;
    // 캐럿은 그 바뀜이 있던 맨 앞이다(한글).
    para = cur.paragraph;
    from = to = a;
    return true;
}

void placeCaretRange(model::Document& doc, std::vector<Node>& pages, int story, int para, int from, int to) {
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras || paras->empty()) return;
    para = std::clamp(para, 0, static_cast<int>(paras->size()) - 1);
    const int count = paragraphCharCount((*paras)[static_cast<std::size_t>(para)]);
    from = std::clamp(from, 0, count);
    to = std::clamp(to, from, count);
    const model::CursorPos start{story, para, charIndexToOffset(doc, pages, story, para, from)};
    if (to > from) {
        setPointerCursor(doc, pages, start, model::CursorPos{story, para, charIndexToOffset(doc, pages, story, para, to)});
    } else {
        setPointerCursor(doc, pages, start);
    }
}

}  // namespace

EditResult editResolveChanges(model::Document& doc, std::vector<Node>& pages, bool accept, const std::string& scope,
                              bool useStoredLineHeight, std::string* why) {
    if (readOnly()) return {};
    const PointerCursor cur = pointerCursor();
    bool changed = false;
    int story = cur.active && cur.story > 0 ? cur.story : 1;
    int para = cur.active ? (cur.hasSelection ? cur.startParagraph : cur.paragraph) : 0;
    int from = cur.active ? offsetToCharIndex(doc, pages, story, para, cur.hasSelection ? cur.startOffset : cur.offset) : 0;
    int to = from;
    if (scope == "all") {
        bool merged = false;
        for (model::Section& section : doc.sections) {
            if (section.paragraphs.empty()) continue;
            const int last = static_cast<int>(section.paragraphs.size()) - 1;
            changed = resolveIn(section.paragraphs, 0, 0, last, paragraphCharCount(section.paragraphs.back()), accept,
                                merged) || changed;
        }
        // 표 칸·글상자 안 바뀜도 모두.
        model::forEachParagraph(doc, [&](model::Paragraph& p) {
            forRange(p, 0, paragraphCharCount(p), [&](model::InlineItem& item) {
                if (item.track == 0) return false;
                changed = true;
                return resolveItem(item, accept);
            });
        });
    } else {
        changed = resolveAtCaret(doc, pages, accept, story, para, from, to);
        if (!changed && scope == "move") {
            // 캐럿 자리에 바뀜이 없으면 다음 바뀜으로 가서 고르기만 한다(한글 적용 후 다음으로 이동).
            EditResult moved = editMoveToChange(doc, pages, true);
            if (!moved.handled && why) *why = "바뀐 내용이 없습니다.";
            return moved;
        }
    }
    if (!changed) {
        if (why) *why = "바뀐 내용이 없습니다.";
        return {};
    }
    pruneTrackRecords(doc);
    const EditRelayoutResult relayout = relayoutTracked(doc, pages, useStoredLineHeight);
    placeCaretRange(doc, pages, story, para, from, scope == "all" ? from : to);
    if (scope == "move") {
        // 적용(취소)한 뒤 다음 바뀜을 고른다. 없으면 캐럿은 그 자리다.
        placeCaretRange(doc, pages, story, para, to, to);
        editMoveToChange(doc, pages, true);
    }
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.active ? now.story : story, now.active ? now.paragraph : para, &relayout);
}

EditResult editMoveToChange(model::Document& doc, std::vector<Node>& pages, bool forward) {
    const PointerCursor cur = pointerCursor();
    const int story = cur.active && cur.story > 0 ? cur.story : 1;
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras || paras->empty()) return {};
    const std::vector<ChangeSpan> spans = changeSpans(*paras);
    if (spans.empty()) return {};
    const int startPara = cur.active ? (cur.hasSelection ? (forward ? cur.endParagraph : cur.startParagraph) : cur.paragraph) : 0;
    const int startOff = cur.active ? (cur.hasSelection ? (forward ? cur.endOffset : cur.startOffset) : cur.offset) : 0;
    const int at = offsetToCharIndex(doc, pages, story, startPara, startOff);
    const ChangeSpan* pick = nullptr;
    if (forward) {
        for (const ChangeSpan& span : spans) {
            if (span.paragraph > startPara || (span.paragraph == startPara && span.from >= at)) {
                pick = &span;
                break;
            }
        }
        if (!pick) pick = &spans.front();  // 끝을 지나면 처음 바뀜으로 돌아간다(한글, 묻지 않는다)
    } else {
        for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
            if (it->paragraph < startPara || (it->paragraph == startPara && it->to <= at)) {
                pick = &*it;
                break;
            }
        }
        if (!pick) pick = &spans.back();
    }
    placeCaretRange(doc, pages, story, pick->paragraph, pick->from, pick->to);
    return makeEditResult(doc, true, false, story, pick->paragraph);
}

}  // namespace chwp::render
