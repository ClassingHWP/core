// 표 자동 채우기(한글 2024 TableAutoFill). 규칙은 한글로 같은 블록을 채워 저장한 HWPX 로 맞췄다
// (tests/test_table_fill.cpp 에 경우마다 한글 결과를 적어 두었다).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/model/table_edit.hpp"

namespace chwp::model {

namespace {

// 한글 2024 의 기본 자동 채우기 목록(HOffice130\Bin\AutoFill.dat 의 [Standard]). 끝 다음은 처음이다.
constexpr const char* kLists[] = {
    "일월,이월,삼월,사월,오월,유월,칠월,팔월,구월,시월,십일월,십이월",
    "1月,2月,3月,4月,5月,6月,7月,8月,9月,10月,11月,12月",
    "１月,２月,３月,４月,５月,６月,７月,８月,９月,１０月,１１月,１２月",
    "一月,二月,三月,四月,五月,六月,七月,八月,九月,十月,十一月,十二月",
    "January,February,March,April,May,June,July,August,September,October,November,December",
    "january,february,march,april,may,june,july,august,september,october,november,december",
    "Jan,Feb,Mar,Apr,May,Jun,Jul,Aug,Sep,Oct,Nov,Dec",
    "JANUARY,FEBRUARY,MARCH,APRIL,MAY,JUNE,JULY,AUGUST,SEPTEMBER,OCTOBER,NOVEMBER,DECEMBER",
    "JAN,FEB,MAR,APR,MAY,JUN,JUL,AUG,SEP,OCT,NOV,DEC",
    "jan,feb,mar,apr,may,jun,jul,aug,sep,oct,nov,dec",
    "월,화,수,목,금,토,일",
    "일,월,화,수,목,금,토",
    "월요일,화요일,수요일,목요일,금요일,토요일,일요일",
    "일요일,월요일,화요일,수요일,목요일,금요일,토요일",
    "月,火,水,木,金,土,日",
    "日,月,火,水,木,金,土",
    "Monday,Tuesday,Wednesday,Thursday,Friday,Saturday,Sunday",
    "monday,tuesday,wednesday,thursday,friday,saturday,sunday",
    "Mon,Tue,Wed,Thu,Fri,Sat,Sun",
    "MONDAY,TUESDAY,WEDNESDAY,THURSDAY,FRIDAY,SATURDAY,SUNDAY",
    "MON,TUE,WED,THU,FRI,SAT,SUN",
    "mon,tue,wed,thu,fri,sat,sun",
    "Sunday,Monday,Tuesday,Wednesday,Thursday,Friday,Saturday",
    "sunday,monday,tuesday,wednesday,thursday,friday,saturday",
    "Sun,Mon,Tue,Wed,Thu,Fri,Sat",
    "SUNDAY,MONDAY,TUESDAY,WEDNESDAY,THURSDAY,FRIDAY,SATURDAY",
    "SUN,MON,TUE,WED,THU,FRI,SAT",
    "sun,mon,tue,wed,thu,fri,sat",
    "태조,정종,태종,세종,문종,단종,세조,예종,성종,연산군,중종,인종,명종,선조,광해군,인조,효종,현종,숙종,경종,영조,정조,순조,헌종,"
    "철종,고종,순종",
    "갑,을,병,정,무,기,경,신,임,계",
    "甲,乙,丙,丁,戊,己,庚,辛,壬,癸",
    "자,축,인,묘,진,사,오,미,신,유,술,해",
    "子,丑,寅,卯,辰,巳,午,未,申,酉,戌,亥",
    "갑자,을축,병인,정묘,무진,기사,경오,신미,임신,계유,갑술,을해,병자,정축,무인,기묘,경진,신사,임오,계미,갑신,을유,병술,정해,"
    "무자,기축,경인,신묘,임진,계사,갑오,을미,병신,정유,무술,기해,경자,신축,임인,계묘,갑진,을사,병오,정미,무신,기유,경술,신해,"
    "임자,계축,갑인,을묘,병진,정사,무오,기미,경신,신유,임술,계해",
    "甲子,乙丑,丙寅,丁卯,戊辰,己巳,庚午,辛未,壬申,癸酉,甲戌,乙亥,丙子,丁丑,戊寅,己卯,庚辰,辛巳,壬午,癸未,甲申,乙酉,丙戌,丁亥,"
    "戊子,己丑,庚寅,辛卯,壬辰,癸巳,甲午,乙未,丙申,丁酉,戊戌,己亥,庚子,辛丑,壬寅,癸卯,甲辰,乙巳,丙午,丁未,戊申,己酉,庚戌,辛亥,"
    "壬子,癸丑,甲寅,乙卯,丙辰,丁巳,戊午,己未,庚申,辛酉,壬戌,癸亥",
    "기역,니은,디귿,리을,미음,비읍,시옷,이응,지읒,치읓,키읔,티읕,피읖,히읗",
    "ㄱ,ㄴ,ㄷ,ㄹ,ㅁ,ㅂ,ㅅ,ㅇ,ㅈ,ㅊ,ㅋ,ㅌ,ㅍ,ㅎ",
    "가,나,다,라,마,바,사,아,자,차,카,타,파,하",
    "A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z",
    "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,w,x,y,z",
    "입춘,우수,경칩,춘분,청명,곡우,입하,소만,망종,하지,소서,대서,입추,처서,백로,추분,한로,상강,입동,소설,대설,동지,소한,대한",
    "立春,雨水,驚蟄,春分,淸明,穀雨,立夏,小滿,芒種,夏至,小暑,大暑,立秋,處暑,白露,秋分,寒露,霜降,立冬,小雪,大雪,冬至,小寒,大寒",
    "first,second,third,fourth,fifth,sixth,seventh,eighth,ninth,tenth,eleventh,twelfth,thirteenth,fourteenth,fifteenth,"
    "sixteenth,seventeenth,eighteenth,nineteenth,twentieth",
    "First,Second,Third,Fourth,Fifth,Sixth,Seventh,Eighth,Ninth,Tenth,Eleventh,Twelfth,Thirteenth,Fourteenth,Fifteenth,"
    "Sixteenth,Seventeenth,Eighteenth,Nineteenth,Twentieth",
    "FIRST,SECOND,THIRD,FOURTH,FIFTH,SIXTH,SEVENTH,EIGHTH,NINTH,TENTH,ELEVENTH,TWELFTH,THIRTEENTH,FOURTEENTH,FIFTEENTH,"
    "SIXTEENTH,SEVENTEENTH,EIGHTEENTH,NINETEENTH,TWENTIETH",
    "１,２,３,４,５,６,７,８,９,１０,１１,１２,１３,１４,１５,１６,１７,１８,１９,２０",
    "하나,둘,셋,넷,다섯,여섯,일곱,여덟,아홉,열,열하나,열둘,열셋,열넷,열다섯,열여섯,열일곱,열여덟,열아홉,스물",
    "첫째,둘째,셋째,넷째,다섯째,여섯째,일곱째,여덟째,아홉째,열째,열하나째,열둘째,열셋째,열넷째,열다섯째,열여섯째,열일곱째,"
    "열여덟째,열아홉째,스물째",
    "one,two,three,four,five,six,seven,eight,nine,ten,eleven,twelve,thirteen,fourteen,fifteen,sixteen,seventeen,eighteen,"
    "nineteen,twenty",
    "One,Two,Three,Four,Five,Six,Seven,Eight,Nine,Ten,Eleven,Twelve,Thirteen,Fourteen,Fifteen,Sixteen,Seventeen,Eighteen,"
    "Nineteen,Twenty",
    "ONE,TWO,THREE,FOUR,FIVE,SIX,SEVEN,EIGHT,NINE,TEN,ELEVEN,TWELVE,THIRTEEN,FOURTEEN,FIFTEEN,SIXTEEN,SEVENTEEN,EIGHTEEN,"
    "NINETEEN,TWENTY",
    "一,二,三,四,五,六,七,八,九,十,十一,十二,十三,十四,十五,十六,十七,十八,十九,二十",
    "第一,第二,第三,第四,第五,第六,第七,第八,第九,第十,第十一,第十二,第十三,第十四,第十五,第十六,第十七,第十八,第十九,第二十",
    "星期天,星期一,星期二,星期三,星期四,星期五,星期六",
    "星期日,星期一,星期二,星期三,星期四,星期五,星期六",
};

const std::vector<std::vector<std::string>>& autoFillLists() {
    static const std::vector<std::vector<std::string>> lists = [] {
        std::vector<std::vector<std::string>> out;
        for (const char* line : kLists) {
            std::vector<std::string> items;
            std::string item;
            for (const char* p = line; *p; ++p) {
                if (*p == ',') {
                    items.push_back(item);
                    item.clear();
                } else {
                    item += *p;
                }
            }
            items.push_back(item);
            out.push_back(std::move(items));
        }
        return out;
    }();
    return lists;
}

bool isSpace(char16_t c) { return c == u' ' || c == u'\t' || c == 0x00A0 || c == 0x3000; }

// 반각 0~9 와 전각 ０~９ 를 숫자로 본다(한글은 "１","２" 를 3, 4 로 잇는다).
int digitOf(char16_t c) {
    if (c >= u'0' && c <= u'9') return c - u'0';
    if (c >= 0xFF10 && c <= 0xFF19) return c - 0xFF10;
    return -1;
}

// 칸 글 하나를 "앞 글 + 수 + 뒤 글" 로 나눈 것. 수가 없으면 hasNumber 가 거짓이다.
// 앞 공백은 버린다. 부호(+, -)는 글 맨 앞일 때만 수에 붙는다("x-2" 의 - 는 앞 글이다).
// 수는 숫자 사이의 쉼표(1,000)와 소수점 뒤 숫자까지다. 뒤 글은 공백을 건너뛴 다음 공백 앞까지 한 낱말만 남는다
// ("3월 1일" 은 3 과 "월", "a 1 b" 는 "a " 와 1 과 "b").
struct NumberText {
    bool hasNumber = false;
    std::u16string prefix;
    double value = 0;
    std::u16string suffix;
};

NumberText splitNumber(const std::string& utf8) {
    std::u16string text = utf8ToUtf16(utf8);
    std::size_t start = 0;
    while (start < text.size() && isSpace(text[start])) ++start;
    text.erase(0, start);
    NumberText out;
    std::size_t i = 0;
    while (i < text.size() && digitOf(text[i]) < 0) ++i;
    if (i == text.size()) return out;
    std::size_t numberStart = i;
    std::string ascii;  // 반각으로 바꾼 수. 쉼표는 뺀다
    if (i == 1 && (text[0] == u'-' || text[0] == u'+')) {
        numberStart = 0;
        if (text[0] == u'-') ascii += '-';
    }
    std::size_t j = i;
    while (j < text.size()) {
        const int d = digitOf(text[j]);
        if (d >= 0) {
            ascii += static_cast<char>('0' + d);
            ++j;
        } else if (text[j] == u',' && j + 1 < text.size() && digitOf(text[j + 1]) >= 0) {
            ++j;
        } else {
            break;
        }
    }
    if (j + 1 < text.size() && text[j] == u'.' && digitOf(text[j + 1]) >= 0) {
        ascii += '.';
        ++j;
        while (j < text.size() && digitOf(text[j]) >= 0) ascii += static_cast<char>('0' + digitOf(text[j++]));
    }
    out.hasNumber = true;
    out.prefix = text.substr(0, numberStart);
    out.value = std::strtod(ascii.c_str(), nullptr);
    std::size_t k = j;
    while (k < text.size() && isSpace(text[k])) ++k;
    std::size_t end = k;
    while (end < text.size() && !isSpace(text[end])) ++end;
    out.suffix = text.substr(k, end - k);
    return out;
}

// 수 글. 소수 여섯째 자리에서 반올림하고 끝의 0 과 소수점을 지운다(한글: 17.0859375 → 17.085938, 3.0 → 3).
std::string formatNumber(double value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%f", value);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

}  // namespace

AutoFillRule autoFillRule(const std::vector<std::string>& seeds) {
    AutoFillRule rule;
    rule.seedCount = static_cast<int>(seeds.size());
    if (seeds.size() < 2) return rule;
    std::vector<NumberText> numbers;
    numbers.reserve(seeds.size());
    bool allNumbers = true;
    bool allPure = true;
    for (const std::string& seed : seeds) {
        numbers.push_back(splitNumber(seed));
        const NumberText& n = numbers.back();
        allNumbers = allNumbers && n.hasNumber;
        allPure = allPure && n.hasNumber && n.prefix.empty() && n.suffix.empty();
    }
    const auto numberRule = [&]() {
        // 둘이면 늘 등차다. 셋 이상은 차이가 모두 같으면 등차, 아니면 비가 모두 같으면 등비, 둘 다 아니면 되풀이.
        // 같은지는 double 값 그대로 본다(한글: 0.1, 0.2, 0.3 은 되풀이).
        std::vector<double> v;
        for (const NumberText& n : numbers) v.push_back(n.value);
        const double step = v[1] - v[0];
        bool arithmetic = true;
        for (std::size_t i = 2; i < v.size(); ++i) arithmetic = arithmetic && (v[i] - v[i - 1]) == step;
        if (arithmetic) {
            rule.kind = AutoFillRule::Kind::Arithmetic;
            rule.step = step;
        } else {
            bool geometric = v[0] != 0;
            const double ratio = geometric ? v[1] / v[0] : 0;
            for (std::size_t i = 2; geometric && i < v.size(); ++i) geometric = v[i - 1] != 0 && v[i] / v[i - 1] == ratio;
            if (!geometric) return;
            rule.kind = AutoFillRule::Kind::Geometric;
            rule.step = ratio;
        }
        rule.last = v.back();
        for (const NumberText& n : numbers) {
            rule.prefixes.push_back(utf16ToUtf8(n.prefix));
            rule.suffixes.push_back(utf16ToUtf8(n.suffix));
        }
    };
    // 수만 있는 칸은 수 규칙이 먼저다(전각 "１","２" 도 목록이 아니라 3, 4). 글이 붙은 칸은 목록이 먼저다("11月","12月" → 1月).
    if (allPure) {
        numberRule();
        return rule;
    }
    const auto& lists = autoFillLists();
    for (std::size_t li = 0; li < lists.size(); ++li) {
        const std::vector<std::string>& list = lists[li];
        const int size = static_cast<int>(list.size());
        int prev = -1;
        bool ok = true;
        for (const std::string& seed : seeds) {
            const auto it = std::find(list.begin(), list.end(), seed);
            if (it == list.end()) {
                ok = false;
                break;
            }
            const int at = static_cast<int>(it - list.begin());
            if (prev >= 0 && at != (prev + 1) % size) {
                ok = false;
                break;
            }
            prev = at;
        }
        if (ok) {
            rule.kind = AutoFillRule::Kind::List;
            rule.list = static_cast<int>(li);
            rule.listIndex = prev;
            return rule;
        }
    }
    if (allNumbers) numberRule();
    return rule;
}

std::optional<std::string> autoFillValue(const AutoFillRule& rule, int k) {
    const int n = rule.seedCount;
    if (n <= 0 || k < n) return std::nullopt;
    const int ahead = k - (n - 1);
    switch (rule.kind) {
        case AutoFillRule::Kind::Repeat: return std::nullopt;
        case AutoFillRule::Kind::List: {
            const std::vector<std::string>& list = autoFillLists()[static_cast<std::size_t>(rule.list)];
            const int size = static_cast<int>(list.size());
            return list[static_cast<std::size_t>((rule.listIndex + ahead) % size)];
        }
        case AutoFillRule::Kind::Arithmetic:
        case AutoFillRule::Kind::Geometric: {
            double value = rule.last;
            for (int i = 0; i < ahead; ++i) {
                value = rule.kind == AutoFillRule::Kind::Arithmetic ? value + rule.step : value * rule.step;
            }
            const std::size_t at = static_cast<std::size_t>(k % n);
            return rule.prefixes[at] + formatNumber(value) + rule.suffixes[at];
        }
    }
    return std::nullopt;
}

namespace {

bool cellHasContent(const TableCell& cell) {
    for (const Paragraph& para : cell.content.paragraphs) {
        for (const Run& run : para.runs) {
            for (const InlineItem& item : run.items) {
                if (item.type == InlineItem::Type::Control || !item.text.empty()) return true;
            }
        }
    }
    return false;
}

std::string firstParagraphText(const TableCell& cell) {
    return cell.content.paragraphs.empty() ? std::string() : cell.content.paragraphs.front().text();
}

// 규칙으로 만든 글 한 문단. 문단 모양·스타일과 글자 모양은 본보기 칸 첫 문단(첫 조각) 것이다.
void writeText(TableCell& target, const TableCell& model, const std::string& text) {
    Paragraph para;
    std::uint32_t charShape = 0;
    if (!model.content.paragraphs.empty()) {
        const Paragraph& first = model.content.paragraphs.front();
        para.paraShapeId = first.paraShapeId;
        para.styleId = first.styleId;
        if (!first.runs.empty()) charShape = first.runs.front().charShapeId;
    }
    Run run;
    run.charShapeId = charShape;
    if (!text.empty()) run.items.push_back(InlineItem::makeText(text));
    para.runs.push_back(std::move(run));
    target.content.paragraphs.clear();
    target.content.paragraphs.push_back(std::move(para));
}

// 되풀이: 본보기 칸 글을 문단째 옮긴다(여러 문단도 그대로).
void copyContent(TableCell& target, const TableCell& model) {
    target.content.paragraphs = model.content.paragraphs;
    for (Paragraph& para : target.content.paragraphs) {
        para.lineSegs.clear();
        para.id = 0;
    }
}

}  // namespace

TableEdit autoFillTableCells(Table& table, int row0, int col0, int row1, int col1) {
    TableEdit out;
    if (!tableGridValid(table)) {
        out.error = "표 칸이 겹치거나 비어 있는 자리가 있어 채울 수 없습니다.";
        return out;
    }
    const TableGrid grid = tableGrid(table);
    row0 = std::max(0, row0);
    col0 = std::max(0, col0);
    row1 = std::min(grid.rows - 1, row1);
    col1 = std::min(grid.cols - 1, col1);
    if (row0 > row1 || col0 > col1) {
        out.error = "채울 칸 블록이 없습니다.";
        return out;
    }
    for (int r = row0; r <= row1; ++r) {
        for (int c = col0; c <= col1; ++c) {
            const TableCell& cell = table.cells[static_cast<std::size_t>(grid.at(r, c))];
            if (cell.rowSpan > 1 || cell.colSpan > 1) {
                out.error = "합친 칸이 든 블록은 자동 채우기를 하지 않습니다.";
                return out;
            }
        }
    }
    // 칸을 더 많이 잡은 쪽으로 채운다. 줄과 칸 수가 같으면 가로(줄마다 왼쪽에서 오른쪽)다.
    const bool across = (col1 - col0) >= (row1 - row0);
    const int lines = across ? row1 - row0 + 1 : col1 - col0 + 1;
    const int length = across ? col1 - col0 + 1 : row1 - row0 + 1;
    const auto cellAt = [&](int line, int i) -> TableCell& {
        const int r = across ? row0 + line : row0 + i;
        const int c = across ? col0 + i : col0 + line;
        return table.cells[static_cast<std::size_t>(grid.at(r, c))];
    };
    if (!cellHasContent(cellAt(0, 0))) {
        out.error = "블록의 첫 칸이 비어 있어 채우지 않습니다.";
        return out;
    }
    AutoFillRule rule;
    std::vector<TableCell> models;  // 규칙을 정한 본보기 칸. 뒤 줄이 이 규칙을 이어 쓸 때도 그대로다
    int k = 0;
    bool changed = false;
    for (int line = 0; line < lines; ++line) {
        int i = 0;
        if (cellHasContent(cellAt(line, 0))) {
            // 첫 칸부터 처음 빈 칸 앞까지가 본보기다. 그 뒤 칸은 모두 덮어쓴다.
            models.clear();
            std::vector<std::string> seeds;
            while (i < length && cellHasContent(cellAt(line, i))) {
                models.push_back(cellAt(line, i));
                seeds.push_back(firstParagraphText(cellAt(line, i)));
                ++i;
            }
            rule = autoFillRule(seeds);
            k = i;
        }
        // 첫 칸이 빈 줄은 앞 줄의 규칙을 이어 간다.
        for (; i < length; ++i, ++k) {
            TableCell& target = cellAt(line, i);
            const TableCell& model = models[static_cast<std::size_t>(k % static_cast<int>(models.size()))];
            if (const std::optional<std::string> value = autoFillValue(rule, k)) {
                writeText(target, model, *value);
            } else {
                copyContent(target, model);
            }
            changed = true;
        }
    }
    if (!changed) out.error = "채울 빈 칸이 없습니다.";
    out.focusCell = grid.at(row0, col0);
    return out;
}

}  // namespace chwp::model
