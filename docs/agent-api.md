# 에이전트 API 설계 (초안)

Agent Skill 이 한글 문서를 읽고, 좌표로 찾고, 그림으로 뽑고, 고칠 수 있게 코어에 둘 공개 API 설계다.
코어가 로직을 갖고, CLI·WASM 은 이 API 를 JSON 으로 감싸기만 한다.

## 1. 원칙

- **상태 없음.** 지금 편집 함수는 `pointer_hold.cpp` 의 전역 커서(`static PointerHold`)에 대고 동작한다.
  에이전트 API 는 위치를 인자로 받고, 전역 커서·블록·조합 상태를 읽지도 바꾸지도 않는다.
- **문서 하나 = 세션 하나.** `AgentDocument` 가 문서 모델, 조판 결과(쪽 트리), story 표, 되돌리기 기록을 함께 가진다.
  편집하면 그 안에서 다시 조판까지 끝내고 돌려준다. 호출자는 쪽 트리를 따로 들고 다니지 않는다.
- **실패는 값으로.** 예외를 던지거나 죽지 않는다. 모든 함수가 `Status`(코드 + 사람이 읽을 문장)를 돌려준다.
  범위를 벗어난 쪽·문단·글자, 읽기 전용, 아직 못 하는 일은 각각 다른 코드다.
- **JSON 이 기본 표현.** 모든 입력·출력 구조체는 JSON 으로 오가는 모양이 정해져 있다(6장). CLI 와 WASM 은 그대로 넘긴다.
- **보는 그림과 같은 결과.** 좌표 찾기와 그림 내보내기는 화면 그리기와 같은 조판·`placeLine`/`glyphBaseIn` 을 쓴다.
  에이전트가 본 그림의 자리와 찾기 결과의 자리가 어긋나지 않는다.

## 2. 단위와 좌표계

| 대상 | 표현 | 비고 |
|---|---|---|
| 쪽 | `PageRef {page, section, nth}` | 2.1 참고. 한글 「찾아가기」처럼 구역과 쪽 번호로 가리킨다 |
| 쪽 안 좌표 | HWPUNIT, 종이 왼쪽 위가 (0, 0), 아래로 +y | 1 mm = 7200 / 25.4 ≈ 283.46 HU. 입력은 `"50mm"`, `"120pt"`, 정수(HU) 모두 받고 출력은 HU 와 mm 를 함께 준다 |
| 사각형 | `{x, y, w, h}` HU | 쪽 좌표 |
| 구역 | `section`: 1부터 | 한글 상태 표시줄의 「1/3 구역」과 같은 번호. 내부 `sections[]` 순번은 1을 뺀 값 |
| 글 묶음 | `story`: 정수 | 구역 본문, 표 칸, 글상자, 머리말·꼬리말, 각주·미주, 캡션, 바탕쪽이 각각 하나 (`StoryKind`) |
| 글자 위치 | `{story, paragraph, char}` | `char` 는 그 문단 **글자 순번**(유니코드 코드 포인트). 개체 자리 하나는 글자 하나로 센다. 3장의 `paragraphText` 가 돌려주는 글과 순번이 같다 |
| 구간 | `{from, to}` 글자 위치 둘 | 같은 story 안이어야 한다. 반열린 구간 [from, to) |
| 개체 | `{story, paragraph, index}` = 그 문단 안 개체 순번 | 떠 있는 개체는 글자를 차지하지 않아 같은 글자 자리에 여럿이 박힐 수 있다. 그래서 순번으로 부르고, 박힌 글자 자리는 `at` 으로 따로 준다. 표 칸은 개체가 담은 story(`ObjectInfo::stories`)로 간다 |

내부의 `CursorPos.offset`(lineseg 단위, 개체가 8칸을 차지)과 `charIndex` 변환은 API 안에서만 한다. 밖으로 내보내지 않는다.

### 2.1 쪽을 가리키는 법

**PDF 는 이렇게 한다.** 쪽의 정체는 문서 안 물리 순서(0, 1, 2, …)다. 화면에 보이는 번호는 따로 붙은
「쪽 이름표」(`/PageLabels`)로, 구간마다 형식(1, 2 / i, ii / A-1)과 시작 번호를 정한다. 이름표는 겹칠 수 있어서,
이름표로 찾으면 뷰어마다 첫 번째를 고르거나 실패한다. 결국 PDF 도구는 물리 순서를 기본 열쇠로 쓴다.

**한글은 구역마다 쪽 번호가 다시 시작할 수 있다.** 구역의 시작 쪽 번호(`Section::pageStart`)와 구역 중간의
「새 번호로 시작」(`NewNumber`) 때문에 같은 번호가 여러 쪽에 붙을 수 있다. 한글의 「찾아가기」는 그래서
쪽 번호와 구역을 함께 받는다. 상태 표시줄도 `79쪽 … 1/1 구역` 으로 둘을 같이 보여 준다.

**에이전트 API 는 한글처럼 쪽 번호 + 구역으로 가리킨다.** 물리 순서는 입력으로 받지 않는다.

```cpp
struct PageRef {
    int page = 0;      // 인쇄되는 쪽 번호. 그 쪽에 번호를 감췄어도 한글이 매긴 번호다(79쪽이면 79)
    int section = 0;   // 구역 번호. 1부터(한글 표시와 같다). 0이면 생략
    int nth = 0;       // 같은 구역 안에서도 번호가 겹칠 때 몇 번째인지. 1부터. 0이면 생략
};
```

- `page` 하나로 문서 안 쪽이 하나로 정해지면 `section` 은 생략해도 된다.
- 같은 번호가 여러 구역에 있으면 `section` 이 **반드시** 있어야 한다. 없으면 고르지 않고
  `AmbiguousPage` 오류와 함께 후보 목록(`[{page, section, nth, label}]`)을 돌려준다.
- 한 구역 안에서 「새 번호로 시작」으로 번호가 또 겹치면 `nth` 까지 있어야 한다. 규칙은 위와 같다.
- 그런 번호가 없으면 `PageNotFound`. 메시지에 그 구역의 쪽 번호 범위를 적는다.

출력의 쪽은 항상 `{page, section}` 을 다 채우고, 겹칠 때만 `nth` 를 채운다. 같이 주는 것:

| 필드 | 뜻 |
|---|---|
| `label` | 그 쪽 번호 형식대로 쓴 글. `79`, `iv`, `Ⅳ`, `라` 등 (`numFormat`) |
| `numberShown` | 그 쪽에 쪽 번호가 실제로 찍히는지(감추기·번호 위치 없음이면 거짓) |
| `order` | 문서 안 물리 순서(1부터). 출력 전용. 인쇄·PDF 로 옮길 때 대조용이고 입력으로는 받지 않는다 |
| `size` | 종이 크기(HU, mm), 가로 여부 |

## 3. 읽기

```cpp
// 쪽 목록. 문서 순서대로 {page, section, nth}, label, 번호가 찍히는지, 크기.
std::vector<PageInfo> pages() const;
// story 목록. 종류, 담고 있는 개체, 문단 수.
std::vector<StoryInfo> stories() const;
// 한 문단의 글자열(개체 자리는 U+FFFC), 문단 모양, 글자 모양 구간, 들어 있는 개체.
Result<ParagraphInfo> paragraph(int story, int paragraph) const;
// 글자 찾기. 정규식 여부, 대소문자, story 제한. 찾은 구간과 그 구간이 놓인 쪽·상자.
std::vector<Match> find(const FindQuery& query) const;
// 구간이 화면에 놓인 자리. 쪽이 넘어가면 쪽마다 상자를 나눠 준다(블록 칠하기와 같은 상자).
Result<std::vector<PageRect>> boxesOf(const Range& range) const;
```

기존 코드: `ensureStoryMap`, `storyParagraph`, `blockBoxes`, `selectionText`, `export/json` 을 감싼다.

## 4. 좌표로 찾기

```cpp
// 그 쪽 (x, y) 아래 있는 것을 바깥에서 안쪽 순서로. 겹친 개체는 위에 그려진 것부터.
Result<HitStack> hitTest(const PageRef& page, Point at, HitOptions options = {}) const;
// 사각형과 겹치는 줄·개체·칸. 영역 단위로 "여기 뭐가 있나" 를 물을 때.
Result<std::vector<HitItem>> itemsIn(const PageRef& page, Rect area, HitOptions options = {}) const;
```

`HitItem` 하나가 담는 것:

| 필드 | 뜻 |
|---|---|
| `kind` | `page`, `header`, `footer`, `masterPage`, `column`, `paragraph`, `line`, `char`, `table`, `cell`, `picture`, `shape`, `equation`, `textBox`, `container`, `note` |
| `box` | 쪽 좌표 HU 사각형 (`Node::huX/huY/huW/huH`) |
| `position` / `range` | 글이면 글자 위치·구간, 개체면 박힌 자리, 칸이면 `{object, row, col}` |
| `text` | 줄·문단이면 글(앞뒤 잘라 최대 N자), 글자면 그 글자, 수식이면 스크립트 |
| `style` | 글자: 글꼴·크기·굵게·기울임·색. 문단: 정렬·줄 간격·들여쓰기 |
| `object` | 개체: 크기, 글자처럼 취급 여부, 감싸기, 그림이면 BinData 이름 |
| `editable` | 이 자리에서 글을 고칠 수 있는지 (읽기 전용·바탕쪽·쪽 번호 등은 거짓) |

맨 안쪽 `char` 는 `caretOnPage`/`caretInStory` 의 결과다. 글자가 없는 곳이면 가장 가까운 글자 경계를 `nearest` 로 따로 준다.

기존 코드: `pointer_hold.cpp` 의 `hitPaper`, `lineUnder`, `bodyLineAt`, `walkPlacedChildren`, `dragHitAt` 을
`render/hit_test.cpp` 로 옮겨 공개하고, 포인터 처리도 그것을 부르게 한다(판정을 두 벌 두지 않는다).

## 5. 그림으로 내보내기

```cpp
struct RenderOptions {
    double dpi = 150;           // 또는 pixelHeight. 둘 다 주면 dpi
    int pixelHeight = 0;
    std::string format = "png"; // png, rgba
    bool background = true;     // 거짓이면 종이색 대신 투명
    bool marks = false;         // 참이면 hitTest 상자·구간을 덧그린다(디버그·설명용)
};
Result<Image> renderPage(const PageRef& page, const RenderOptions& options) const;
// 쪽 좌표 사각형만. 테두리 밖으로 삐져나온 그림도 사각형 안이면 그린다.
Result<Image> renderRegion(const PageRef& page, Rect area, const RenderOptions& options) const;
// 구간·개체가 놓인 상자를 감싸서. 쪽이 넘어가면 쪽마다 한 장.
Result<std::vector<Image>> renderRange(const Range& range, const RenderOptions& options) const;
Result<Image> renderObject(const Position& object, const RenderOptions& options) const;
```

`Image` 는 바이트와 함께 그 그림이 어느 쪽(`PageRef`)의 어느 사각형인지(`source`)와 HU→픽셀 배율을 준다.
`boxesOf`·`find`·`hitTest` 결과의 쪽도 모두 같은 `PageRef` 모양이다.
에이전트가 그림 위에서 고른 픽셀을 `source` 로 되돌려 `hitTest` 에 넣을 수 있다.

기존 코드: `renderPagePng`, `renderPageBitmap(…, viewX, viewY, w, h)` 를 감싼다. 쪽 좌표 HU 를 픽셀로 바꾸는 일만 더한다.
그리기는 크로스플랫폼 경로(CPU 비트맵 또는 Skia)만 쓴다. GDI 는 쓰지 않는다.

## 6. 고치기

모든 편집은 `EditOp` 목록 하나를 트랜잭션으로 받는다. 하나라도 실패하면 아무것도 바꾸지 않는다.

```cpp
Result<EditReport> apply(const std::vector<EditOp>& ops);
bool undo();
bool redo();
```

| op | 인자 | 상태 |
|---|---|---|
| `insertText` | `at`, `text` (`\n` 은 문단 나눔, `\t` 은 탭) | 지금 있음(커서 기반) → 위치 기반으로 |
| `delete` | `range` | 있음 → 위치 기반으로 |
| `replace` | `range`, `text` | delete + insertText |
| `setCharFormat` | `range`, 글꼴·크기·굵게·기울임·밑줄·취소선·색 | `FormatPatch` 있음 |
| `setParaFormat` | `range`(걸친 문단 전부), 정렬·줄 간격·들여쓰기·문단 간격·스타일 | 일부 있음 |
| `splitParagraph` / `mergeParagraphs` | `at` / `paragraph` | 있음(`splitParagraphAt`, `mergeParagraphs`) |
| `insertTable` | `at`, 행·열, 칸 너비, 칸 글 | **새로** |
| `setCellText` | `{object,row,col}`, `text` | 칸 story 에 insert/delete 로 |
| `insertPicture` | `at`, `file`(셸이 읽음)·`base64`, `width`·`height`(HU) | 있음(2026-11-10). 글자처럼 넣는다 |
| `insertEquation` | `at`, `script`, `size`(pt) | 됨: 명령 op `insertEquation`·`equation`(고치기), 크기는 한글 수식 엔진과 같게 잰다 |
| `deleteObject` | `object` | 있음(2026-11-10). `render::editDeleteObject` |
| `objectProperties` | `object`, `set`(놓기·크기·바깥 여백) | 있음(2026-11-10). `render::editObjectProperties`, 크기는 손잡이 조절과 같은 규칙 |
| 편집기 명령 | 머리말·꼬리말·쪽 번호·바탕쪽·쪽 지우기·메모·교정 부호·블록 계산·각주/미주·주석 모양·쪽 테두리·구역·단·표 속성·표↔문자열·캡션·책갈피·하이퍼링크·덧말·글자 바꾸기·정렬·조판 부호 지우기·모양 복사·도형 | 있음(2026-11-10). `OpKind::Command`(command 이름, 자리 at·range·object·story·section, set·args). 값은 `commandView`(JSON `view`) |

`EditReport`:

- `changed`: 바뀐 story·문단 범위, 다시 그린 쪽 범위, 쪽 수가 바뀌었는지.
- `positions`: op 마다 결과 위치(넣은 글의 끝, 새 개체의 자리). 다음 op 를 이어 붙일 때 쓴다.
- `remap`: 이전 위치를 새 위치로 옮기는 규칙. 에이전트가 먼저 받아 둔 위치를 버리지 않고 고쳐 쓰게 한다.

되돌리기는 트랜잭션마다 고친 story 의 문단 사본(바뀌기 전)을 남긴다. 메모리를 더 써서 빨리 되돌린다.

다시 조판은 기존 `relayoutAfterEdit` / `reflowEditedPages` 를 쓴다. 표 칸·글상자·각주 안 편집이 쪽을 넘기는 경우는
지금 부분적이므로(헤더 주석) 그 경우에는 그 구역을 통째로 다시 놓는다. 느리더라도 틀리지 않게 한다.

## 7. 오류

```cpp
enum class Code { Ok, BadArgument, PageNotFound, AmbiguousPage, PositionOutOfRange, CrossStory, ReadOnly, NotEditable,
                  Unsupported, ParseFailed, RenderFailed, Internal };
struct Status { Code code = Code::Ok; std::string message; };
template <class T> struct Result { Status status; T value; };
```

- 입력 검사는 API 입구에서 한 번에 한다. 안쪽 함수는 이미 검사된 값만 받는다.
- `message` 는 한국어 한 문장. 어떤 인자가 왜 틀렸는지 적는다. 예: `5쪽이 구역 1과 구역 3에 있습니다. section 을 주세요.`
- `AmbiguousPage` 는 `candidates` 에 고를 수 있는 쪽을 모두 담는다. 에이전트는 그중 하나를 골라 다시 부른다.
- `Internal` 이 나오면 버그다. 테스트가 이 코드를 한 번도 보지 않게 한다.

## 8. JSON 과 CLI

같은 구조를 JSON 으로 받는 함수 하나를 둔다: `std::string agentCall(AgentDocument&, std::string_view requestJson)`.

```json
{"call": "hitTest", "page": {"page": 79}, "at": {"x": "60mm", "y": "150mm"}}
{"call": "renderRegion", "page": {"page": 3, "section": 2}, "area": {"x": "20mm", "y": "140mm", "w": "120mm", "h": "70mm"}, "dpi": 200}
{"call": "apply", "ops": [{"op": "replace", "range": {"from": {"story": 0, "paragraph": 488, "char": 12},
                                                     "to":   {"story": 0, "paragraph": 488, "char": 16}},
                           "text": "인수분해"}]}
```

CLI 는 따로 둔다: `tools/chwp-agent`. `chwp-agent 문서.hwp req.json [-o out.json] [--out-dir DIR] [--save out.hwp]`.
요청 파일에 여러 줄(JSON Lines)을 주면 같은 세션에서 차례로 부른다. 그림은 `--out-dir` 에 쓰고 JSON 에는 경로를 남긴다.
`chwp-agent` 는 `agentCall` 을 감싸는 일만 한다. 문서 열기, 요청 읽기, 응답·그림 쓰기, 저장. 판단은 모두 core 에 있다.

- 이 CLI 의 입력·출력 JSON 이 Skill 이 기대는 유일한 약속이다. 모양은 이 문서와 `tests/test_agent.cpp` 로 고정하고,
  바꿀 때는 이 문서부터 고친다.
- `chwp-dump` 는 개발용 점검 도구로 남는다. 출력이 필요에 따라 바뀌므로 Skill 은 부르지 않는다.

WASM 은 `agentCall` 을 그대로 내보낸다.

## 9. 문서화 규칙

- 공개 헤더(`chwp/agent.hpp`)의 모든 함수 위에: 무엇을 하는지, 좌표·위치 단위, 실패 코드, 부작용(조판·되돌리기 기록).
- 구조체 필드마다 단위와 원점.
- 예시 요청·응답은 `docs/agent-api.md` 와 테스트(`tests/test_agent.cpp`)가 같은 JSON 을 쓴다. 문서 예시가 테스트로 검증된다.

## 10. 구현 순서

1. ✅ `agent.hpp` 타입·오류·단위 변환, `AgentDocument` 열기·저장, `pages`/`resolvePage`/`stories`/`paragraph`. (`core/src/agent/agent.cpp`, `tests/test_agent.cpp`)
2. 판정 코드를 `hit_test.cpp` 로 옮기고 `hitTest`/`itemsIn`. 포인터 처리도 같은 코드를 쓰게.
3. `renderPage`/`renderRegion`/`renderRange`/`renderObject`.
4. 위치 기반 `insertText`/`delete`/`replace`/서식, 트랜잭션·되돌리기.
5. `agentCall` JSON, `chwp-agent` CLI, WASM 내보내기.
6. `insertTable`/`insertPicture`/`insertEquation`, 칸·글상자 편집의 다시 조판 보강.

각 단계마다 `tests/test_agent.cpp` 에 샘플 문서(커밋된 `samples/shortcut.hwp` 등)로 왕복 테스트를 붙인다.

## 11. 프리뷰와 딥링크

AI 웹(claude.ai 등)에서 문서를 보여 주는 길은 세 가지다. 2026-10 기준 공식 문서로 확인한 제약이다.

### 11.1 Skill 이 그림을 만들어 보여 준다: 모든 곳에서 된다

- Skill 은 코드 실행 컨테이너에서 돈다. **Linux x86_64, Python 3.11, 메모리 5 GiB, 디스크 5 GiB, CPU 1개.**
  Node 는 기본 목록에 없다. 런타임에 패키지를 설치할 수 없다.
- 네트워크: Claude API 는 **완전히 막힘**. claude.ai 는 사용자·관리자 설정에 따라 다름. Claude Code 는 열림.
  → 글꼴·WASM 을 내려받는 방식은 쓸 수 없다. **필요한 것은 모두 Skill 안에 넣는다.**
- 그래서 Skill 에는 **Linux x86_64 정적 빌드 CLI**(`chwp-agent`)를 넣는다. Python 으로 부르기만 한다.
  글꼴은 자유 글꼴 묶음(함초롬 등)만 넣고, 없는 글꼴은 대체 규칙으로 그린다. 대체가 일어나면 결과 JSON 에 적는다.
- `renderPage`/`renderRegion` 이 만든 PNG 를 Claude 가 대화에 보여 준다. 편집 결과는 HWP/HWPX 파일로 돌려준다.
- 정할 것: Skill 묶음 크기 한도(공식 수치를 찾지 못함), 넣을 글꼴과 라이선스.

### 11.2 아티팩트 안에 뷰어를 넣는다: 쓰지 않는다

- 아티팩트는 외부 요청(`connect-src`)이 막혀 있다는 보고가 있고, 공식 허용 목록은 찾지 못했다.
  WASM·글꼴을 받아 와야 하는 우리 뷰어를 여기에 기대지 않는다.

### 11.3 iframe 으로 우리 뷰어를 띄운다: 대화 안 인터랙티브 프리뷰는 이 길로

- claude.ai·Claude Desktop 은 **MCP Apps** 를 지원한다. MCP 서버의 도구가 `_meta.ui.resourceUri` 로 화면을 선언하면
  호스트가 그 HTML 을 대화 안의 샌드박스 iframe 에 띄운다.
- 그 화면은 `_meta.ui.csp` 로 쓸 외부 출처를 선언한다.
  - `frameDomains`: 안에 띄울 iframe 출처 → **`https://ede.kr` 를 넣고 우리 뷰어를 그대로 띄운다.**
  - `resourceDomains`(스크립트·글꼴·그림), `connectDomains`(fetch) 도 있다.
- 감싸는 화면은 얇다. 도구 결과(문서 바이트 또는 내려받기 표, 보여 줄 자리)를 받아 안쪽 뷰어 iframe 에 `postMessage` 로 넘긴다.
  뷰어 쪽 CSP·글꼴·WASM 은 지금 ede.kr 그대로다.
- 제약:
  - 안쪽 iframe 은 교차 출처 격리(COOP/COEP)를 받기 어렵다 → **단일 스레드 빌드**(`?threads=1`)로 띄운다.
  - 감싸는 화면의 기본 `script-src` 에 `'wasm-unsafe-eval'` 이 없다 → WASM 은 감싸는 화면이 아니라 **안쪽 ede.kr 페이지**에서만 돌린다.
  - 문서를 서버에 남기지 않으려면 바이트를 `postMessage` 로 넘기고, 큰 파일은 짧게 사는 내려받기 표를 쓴다. 보관 기간을 정한다.
- 뷰어 쪽에 필요한 것: `/embed` 경로(메뉴 없는 보기 화면), `postMessage` 로 문서 받기, 아래 딥링크로 자리와 강조 받기.

### 11.4 딥링크: 보여 줄 자리를 한 가지 모양으로

11.1~11.3 과 `hitTest`·`find` 결과가 같은 표현을 쓴다. 주소 조각(`#`)이라 서버에 남지 않는다.

```
#section=1&page=79                         쪽
#section=1&page=79&rect=20mm,140mm,120mm,70mm    쪽 안 사각형 강조
#range=0:488:12-0:488:16                   글 구간 강조(story:문단:글자-story:문단:글자)
#object=0:488:3                            개체 강조
#zoom=fitWidth | fitPage | 120
```

같은 값을 JSON 으로도 준다: `{"page": {"page": 79, "section": 1}, "rect": {...}, "range": {...}, "zoom": "fitWidth"}`.

## 12. 정할 것

1. ~~글자 순번 단위~~ → 유니코드 코드 포인트로 정함. 편집기의 문단 글자 번호와 같은 셈이다.
2. ~~쪽: 0부터 순번 / 인쇄 쪽 번호~~ → 쪽 번호 + 구역으로 정함(2.1).
3. ~~좌표 입력~~ → `mm`, `cm`, `in`, `pt`, `hu`, 단위 없는 정수(HU) 모두 받음(`parseLength`).
4. ~~여러 문서 동시~~ → 지금은 한 번에 한 문서로 정함. 동시 처리가 필요해지면 글꼴·줄 캐시를 세션으로 옮긴다.
5. Skill 에 넣을 글꼴 묶음과 라이선스, Skill 크기 한도(11.1).
6. MCP App 프리뷰에서 문서를 넘기는 방식: `postMessage` 만 / 큰 파일은 짧게 사는 내려받기 표(11.3).
