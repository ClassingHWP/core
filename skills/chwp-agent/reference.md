# chwp-agent 요청·응답

요청 하나는 JSON 객체 하나다. `call` 이 호출 이름이고, `id`(글 또는 정수)를 주면 응답에 그대로 붙는다.
이 모양은 core `core/src/agent/agent_call.cpp` 와 시험 `tests/test_agent_edit.cpp` 가 고정한다.

## 공통

| 이름 | 모양 |
|---|---|
| 위치 | `{"story":1,"paragraph":12,"char":4}` 또는 `"1:12:4"` |
| 구간 | `{"from":위치,"to":위치}` 또는 `"1:12:4-1:12:10"`. 반열린 [from, to). 같은 story 여야 한다 |
| 쪽 | `3`(쪽 번호) 또는 `{"page":3,"section":2,"nth":1}`. section·nth 는 번호가 겹칠 때만 |
| 길이 | HWPUNIT(HU). 1pt = 100, 1mm ≈ 283.46. 글자 크기는 `size` 1000 = 10pt |
| 실패 | `{"ok":false,"error":{"code":"PositionOutOfRange","message":"..."}}` |

오류 코드: `BadArgument`, `PageNotFound`, `AmbiguousPage`(`candidates` 를 준다), `PositionOutOfRange`, `CrossStory`,
`ReadOnly`, `NotEditable`, `Unsupported`, `ParseFailed`, `SaveFailed`, `RenderFailed`, `Internal`(core 버그).

글 묶음(story): 구역 본문, 표 칸, 글상자, 머리말·꼬리말, 각주·미주, 캡션, 바탕쪽이 각각 하나다. 구역 본문이 먼저 온다.
편집(특히 표 넣기) 뒤에는 story 번호가 바뀔 수 있으니 다시 읽는다.

## 읽기

### info
`{"call":"info"}` → `{"format":"hwp","pages":14,"sections":1,"stories":27,"warnings":[],"missingFonts":["○○체"]}`.
`missingFonts` 는 받지 못해 대체 글꼴로 재고 그린 글꼴이다.

### pages
`{"call":"pages"}` → `{"pages":[{"page":1,"section":1,"label":"1","order":1,"width":84188,"height":59528,"sizeMm":"297.0x210.0","landscape":true}]}`

### stories
`{"call":"stories"}` → `{"stories":[{"story":4,"kind":"tableCell","owner":"table","section":1,"paragraphs":1,"parentStory":1,"parentParagraph":1,"row":0,"col":0}]}`
kind: `section`, `tableCell`, `subList`, `caption`, `masterPage`. owner: `body`, `table`, `header`, `footer`, `footnote`, `endnote`, `textBox`, `caption`, `masterPage`.

### text
여러 story 의 문단 글을 한 번에 읽는다.

| 인자 | 뜻 |
|---|---|
| `story` | 이 story 만. 없으면 모든 story |
| `from`, `count` | `story`(또는 `startStory`)로 고른 story 의 문단 범위 |
| `maxChars` | 글이 이만큼 넘으면 멈춘다(기본 30000). 멈추면 `next: {"startStory","from"}` 를 준다 |
| `startStory` | 이어 읽기. `next` 의 값을 그대로 요청에 넣는다 |
| `empty` | 참이면 빈 문단(글도 개체도 없음)도 낸다. 기본은 뺀다. 문단 번호는 어느 쪽이든 그대로다 |

→ `{"stories":[{"story":1,...,"text":[{"paragraph":0,"text":"제목"},{"paragraph":1,"text":"￼","objects":[{"index":0,"kind":"table","at":0,"stories":[4,5,6]}]}]}],"next":{...}}`

글 안의 `\t` 는 탭, `\n` 은 문단 안 줄바꿈, `￼`(U+FFFC)는 글자처럼 놓인 개체다.

### paragraph
`{"call":"paragraph","story":1,"paragraph":2}` → 글, 스타일, 정렬(`JUSTIFY` `LEFT` `RIGHT` `CENTER` `DISTRIBUTE` `DISTRIBUTE_SPACE`),
줄 간격(`lineSpacingType`: `PERCENT` 면 %, 나머지 HU), 들여쓰기·문단 간격(HU), 글자 모양 구간 `runs[{from,to,format{font,size,bold,italic,underline,strikeout,color}}]`,
개체 `objects[{index,kind,at,inline,width,height,script,stories}]`.

### find
`{"call":"find","text":"찾을 글","story":0,"matchCase":true,"limit":100}` →
`{"matches":[{"from":위치,"to":위치,"page":{"page":1,"section":1},"context":"앞뒤 글"}]}`.
문단 경계는 넘지 않는다. `matchCase:false` 는 ASCII 만 가린다.

## 그림

### render
`{"call":"render","page":1,"dpi":96,"pixelHeight":0,"name":"p1"}` → `{"file":"<out-dir>/p1.png","page":{...},"width":...,"height":...}`.
`pixelHeight` 를 주면 dpi 대신 그것을 쓴다(20~8000). `name` 이 없으면 `page-s<구역>-p<쪽>.png`.

## 고치기

### apply
`{"call":"apply","ops":[op, ...]}` → `{"pages":15,"fontSwaps":[{"from":"여기어때 잘난체","to":"함초롬바탕"}],"ops":[{"end":위치,"cells":[[4,5,6],[7,8,9]]}]}`.
`end` 는 op 뒤 캐럿 자리(넣은 글의 끝), `cells` 는 insertTable 이 만든 칸의 story[행][열].
`fontSwaps` 는 편집 앞에 바꾼 라이선스로 막힌 글꼴이다(있을 때만). 문서 전체에서 그 글꼴 이름이 바뀐다.

| op | 인자 | 하는 일 |
|---|---|---|
| `insertText` | `at`, `text` | 글을 넣는다. `\n` 은 문단 나눔, ` ` 은 문단 안 줄바꿈, `\t` 는 탭 |
| `delete` | `range` | 지운다. 문단을 넘으면 문단을 합친다 |
| `replace` | `range`, `text` | 지우고 그 자리에 넣는다. 넣은 글은 앞 글자의 모양을 따른다 |
| `format` | `range`, `set`, `style` | `set` 의 서식을 입힌다. `style` 은 문단 스타일 이름(예: `"바탕글"`, `"개요 1"`) |
| `insertTable` | `at`, `rows`, `cols`, `cells` | 표를 넣는다(단 폭, 글자처럼 취급). 표는 제 문단에 놓인다. `cells` 는 `[["가","나"],["다"]]` |
| `setText` | `story`, `text` | 그 story 글 전체를 바꾼다. 표 칸을 고칠 때 쓴다 |

### format 의 set 이름

글자 모양(구간 글자에):

| 이름 | 값 |
|---|---|
| `bold`, `italic`, `strikeout`, `underline` | 참거짓 |
| `underlineType` | `BOTTOM`, `TOP`, `NONE` |
| `color`, `shadeColor`, `underlineColor`, `strikeColor`, `shadowColor` | `"#RRGGBB"` |
| `font` | 글꼴 이름(모든 언어에) |
| `fontSizePt` | pt. `fontSize` 는 HU(1000 = 10pt) |
| `fontSizeStep` | +1 / -1 (1pt 씩) |
| `ratio` 장평 %, `spacing` 자간 %, `relSize` 상대 크기 %, `charOffset` 글자 위치 % | 숫자 |
| `superscript`, `subscript`, `emboss`, `engrave`, `outline`(0~6), `shadow`(0~2), `emphasis`(0~6) | |

문단 모양(구간이 걸친 문단 모두에. 빈 구간이면 그 문단에):

| 이름 | 값 |
|---|---|
| `align` | `JUSTIFY`, `LEFT`, `RIGHT`, `CENTER`, `DISTRIBUTE`, `DISTRIBUTE_SPACE` |
| `lineSpacingType` | `PERCENT`, `FIXED`, `BETWEEN_LINES`, `AT_LEAST` |
| `lineSpacingValue` | 지금 간격 종류 단위(퍼센트면 %, 아니면 pt) |
| `indent`(양수 들여쓰기, 음수 내어쓰기), `marginLeft`, `marginRight`, `spacingPrev`, `spacingNext` | HU |
| `keepWithNext`, `keepLines`, `pageBreakBefore`, `widowOrphan` | 참거짓 |
| `headingType`(`NONE` `OUTLINE` `NUMBER` `BULLET`), `headingLevel`(0부터) | |

모르는 이름이나 꼴이 틀린 값은 `BadArgument` 다.

### undo / redo
`{"call":"undo"}` → `{"done":true,"pages":14}`. apply 하나가 한 번이다. 같은 실행 안에서만 된다.

## 글꼴

스크립트(`scripts/chwp_agent.py`)가 글꼴 서버에서 목록과 파일을 받아 캐시에 넣고, 실행 파일에 `--font-dir` 로 넘긴다.

| 환경 변수 | 뜻 |
|---|---|
| `CHWP_FONT_BASE` | 글꼴 서버(기본 `https://ede.kr/hwp/fonts/`). 목록과 글꼴 파일(`<폴더>/<파일>`)을 여기서 받는다 |
| `CHWP_FONT_CACHE` | 캐시 폴더(기본 `~/.cache/chwp-agent/fonts`) |
| `CHWP_FONT_INDEX` | `index.json` 대신 쓸 글꼴 목록. 파일 경로나 주소 |
| `CHWP_FONT_LICENSE_SWAP` | `license-swap.json`(라이선스로 막힌 글꼴의 대역) 대신 쓸 것 |
| `CHWP_FONT_REPLACE` | `replace.json`(이름 바꾸기) 대신 쓸 것 |

목록은 실행마다 서버에 바뀌었는지 묻고(ETag), 바뀌었을 때만 새로 받는다. 받지 못하면 캐시에 있는 목록을 쓴다.
서버 목록을 받아 고친 파일을 `CHWP_FONT_INDEX` 로 주면, 목록은 그것을 쓰고 글꼴 파일은 `CHWP_FONT_BASE` 에서 받는다.
글꼴 파일은 한 번 받으면 다시 받지 않는다.

### CLI 옵션

스크립트가 대신 하므로 보통은 쓸 일이 없다.

| 옵션 | 뜻 |
|---|---|
| `--font-dir 폴더` | 글꼴 서버와 같은 모양의 캐시(`index.json`, `license-swap.json`, `replace.json`, `full/…`, `metrics/…`). core 가 고른 파일 중 있는 것은 넣고, 없는 것은 못 받은 글꼴로 친다 |
| `--font-plan` | 요청을 부르지 않고 캐시에 아직 없어 받아야 할 파일을 `{"ok":true,"need":["full/HANBatang.woff2",...]}` 로 쓴다 |

받지 못한 파일은 캐시에 `파일.missing` 표시를 두면 그 실행 안에서 다시 묻지 않는다.

## 저장

### save
`{"call":"save","path":"out/고친본.hwpx","format":"hwpx"}` → `{"file":"...","format":"hwpx","bytes":12345}`.
`format` 이 없으면 확장자, 그것도 없으면 연 형식이다. CLI 의 `--save 경로` 는 마지막에 이 요청을 하나 더 부른다.
