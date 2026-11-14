# chwp-agent 요청·응답

요청 하나는 JSON 객체 하나다. `call` 이 호출 이름이고, `id`(글 또는 정수)를 주면 응답에 그대로 붙는다.
이 모양은 core `core/src/agent/agent_call.cpp` 와 시험 `tests/test_agent_edit.cpp` 가 고정한다.

## 공통

| 이름 | 모양 |
|---|---|
| 위치 | `{"story":1,"paragraph":12,"char":4}` 또는 `"1:12:4"` |
| 구간 | `{"from":위치,"to":위치}` 또는 `"1:12:4-1:12:10"`. 반열린 [from, to). 같은 story 여야 한다 |
| 쪽 | `3`(쪽 번호), `{"page":3,"section":2,"nth":1}`(section·nth 는 번호가 겹칠 때만), 또는 `{"order":5}`(문서 안 물리 순서, `pages` 의 `order`). `order` 는 번호가 겹쳐도 하나로 정해진다 |
| 길이 | HWPUNIT(HU). 1pt = 100, 1mm ≈ 283.46. 글자 크기는 `size` 1000 = 10pt |
| 실패 | `{"ok":false,"error":{"code":"PositionOutOfRange","message":"..."}}` |

오류 코드: `BadArgument`, `PageNotFound`, `AmbiguousPage`(`candidates` 를 준다), `PositionOutOfRange`, `CrossStory`,
`ReadOnly`, `NotEditable`, `Unsupported`, `ParseFailed`, `SaveFailed`, `RenderFailed`, `Internal`(core 버그).

문서가 어떻게 겹쳐 있는지(구역·글 묶음·문단·글자·개체)는 아래 「문서 구조」, 표·그림을 어디에 어떻게 놓는지는 「개체 놓기」에 있다.

## 문서 구조

한글 문서는 아래처럼 겹쳐 있다. 모든 위치·개체 이름은 이 구조를 따라간다.

```
문서
└─ 구역(section) 1, 2, …          ← 용지·여백·단·쪽 번호·바탕쪽 설정이 구역마다 있다(`section` 호출)
   └─ 구역 본문 = 글 묶음(story) 하나   ← stories 의 kind "section", owner "body"
      └─ 문단(paragraph) 0, 1, …      ← 문단 모양(정렬·줄 간격·여백)과 글자 모양 구간(runs)
         ├─ 글자 0, 1, …              ← 코드 포인트 순번. 위치 {story, paragraph, char}
         └─ 개체(objects) 0, 1, …     ← 이 문단에 붙은 표·그림·도형·수식·각주·머리말 …
            └─ 개체 안 글 묶음        ← 표 칸, 글상자 글, 머리말·꼬리말 글, 각주 글, 캡션 (stories 의 parentStory·parentParagraph)
```

**글 묶음(story)** 은 문단 목록 하나다. 번호는 `stories` 차례이고 구역 본문이 먼저 온다.

| kind / owner | 무엇 | 어디에 붙나 |
|---|---|---|
| `section` / `body` | 구역 본문 | 구역 |
| `tableCell` / `table` | 표 칸 하나(`row`·`col`·`rowSpan`·`colSpan`) | 표가 붙은 문단 |
| `subList` / `textBox` | 글상자(사각형 등 도형 안 글) | 도형이 붙은 문단 |
| `subList` / `header`·`footer` | 머리말·꼬리말 글 | 머리말 컨트롤이 붙은 문단(보통 구역 첫 문단) |
| `subList` / `footnote`·`endnote` | 각주·미주 글 | 주 번호가 들어간 문단 |
| `caption` / `caption` | 개체 캡션 | 그 개체 |
| `masterPage` / `masterPage` | 바탕쪽 글 | 구역 |

- 개체 안 글 묶음은 `parentStory`·`parentParagraph` 로 바깥 문단을 가리킨다. 표 칸 안에 표가 있으면 두 번 겹친다.
- 문단을 나누거나 합치거나 개체를 넣고 지우면 문단 번호·story 번호·개체 순번이 바뀔 수 있다. 고친 뒤에는 응답(`end`,
  `object`, `table`, `cells`)을 쓰거나 다시 읽는다. 한 apply 안의 op 는 앞 op 를 적용한 문서 기준이다.

**문단 글자열**(`paragraph`·`text` 의 `text`)은 그 문단에서 캐럿이 지나가는 글자다.

- 보통 글자, `\t`(탭), `\n`(문단 안 줄바꿈, 문단 나눔이 아니다)
- `￼`(U+FFFC): **글자처럼 취급**한 개체 하나(글자처럼 놓인 표·그림·수식·도형)와 각주·미주 번호, 덧말·글자 겹치기
- 띄운 개체(글자처럼 취급이 아닌 것), 머리말·꼬리말, 구역·단 정의, 책갈피·필드 표시는 글자를 차지하지 않는다

**문단 안 개체**(`paragraph` 의 `objects`)는 그 문단에 붙은 컨트롤이다.

| 칸 | 뜻 |
|---|---|
| `index` | 그 문단 안 개체 순번(0부터). 개체 이름은 `{story, paragraph, index}` 또는 `"story:문단:index"` |
| `kind` | `table`, `picture`, `shape`(사각형·타원·선·글상자 …), `container`(묶음), `ole`, `equation`, `footnote`, `endnote`, `header`, `footer` |
| `at` | 붙은 글자 자리. 글자처럼 취급이면 그 `￼` 의 순번, 띄운 개체면 그 글자 앞(글자를 차지하지 않는다) |
| `inline` | 글자처럼 취급인가 |
| `width`·`height` | 크기(HU). 머리말·각주처럼 크기가 없는 개체는 0 |
| `placement` | 놓기 값(아래 「개체 놓기」). 크기가 없는 개체에는 없다 |
| `stories` | 안에 든 글 묶음(표 칸들, 글상자 글, 머리말 글 …) |

구역 첫 문단 맨 앞에는 구역 정의·단 정의·머리말·꼬리말·쪽 번호 위치 같은 컨트롤이 모인다. 그래서 첫 문단의 개체 목록에
`header`·`footer` 가 `at` 0, `inline:false` 로 보인다. 그 글은 `stories` 의 글 묶음에서 읽고 고친다.

**쪽**은 조판 결과다. 같은 문단이 여러 쪽에 걸칠 수 있고, 머리말·바탕쪽은 쪽마다 다시 그려진다.
쪽 안 자리는 `layout`(줄·상자 좌표), 모양은 `render` 로 본다. 쪽 번호는 구역마다 다시 시작할 수 있어 `{"order":N}` 이 가장 확실하다.

## 읽기

### info
`{"call":"info"}` → `{"format":"hwp","pages":14,"sections":1,"stories":27,"warnings":[],"missingFonts":["○○체"],"missingGlyphs":[{"font":"휴먼명조","code":"U+F06C"}],"missingGlyphCount":1}`.
`missingFonts` 는 받지 못해 대체 글꼴로 재고 그린 글꼴이다.
`missingGlyphs` 는 그 글꼴과 대체 글꼴 어디에도 없어 **그리지 못하는 글자**다. 그림에서 비거나 네모로 나온다.
앞의 100개만 주고, 모두 몇 개인지는 `missingGlyphCount` 다.

### pages
`{"call":"pages"}` → `{"pages":[{"page":1,"section":1,"label":"1","order":1,"width":84188,"height":59528,"sizeMm":"297.0x210.0","landscape":true}]}`

### stories
`{"call":"stories"}` → `{"stories":[{"story":4,"kind":"tableCell","owner":"table","section":1,"paragraphs":1,"parentStory":1,"parentParagraph":1,"row":0,"col":0}]}`
kind: `section`, `tableCell`, `subList`, `caption`, `masterPage`. owner: `body`, `table`, `header`, `footer`, `footnote`, `endnote`, `textBox`, `caption`, `masterPage`.
합친 표 칸에는 `rowSpan`·`colSpan`(덮는 줄·칸 수)이 붙는다.

### table
`{"call":"table","story":5}` → story 칸이 든 표의 칸 그림.
`{"table":{"parentStory":1,"parentParagraph":0,"rows":4,"cols":7,"columnWidths":[1941,...],"rowHeights":[282,...],
"cells":[{"story":5,"row":0,"col":0,"colSpan":2,"width":4488,"height":282,"text":"01"},...]}}`.
칸은 줄·칸 차례이고 `row`·`col` 은 칸 왼쪽 위 자리(0부터)다. 합친 칸은 `rowSpan`·`colSpan` 으로 여러 자리를 덮고,
모든 자리는 칸 하나에만 덮인다. 칸 높이는 최소 높이이고 글이 많으면 늘어난다. 칸 글의 문단은 `\n` 으로 잇는다.

칸마다 `format` 이 붙는다: `vertAlign`(`TOP` `CENTER` `BOTTOM`), `header`(제목 칸), `ownMargin`, `marginLeft`·`marginRight`·
`marginTop`·`marginBottom`(칸 안 여백. 칸 여백이 없으면 표 안쪽 여백), 테두리/배경 `cell.borderTopType`·`cell.borderTopWidth`·
`cell.borderTopColor`(Top·Bottom·Left·Right·Diagonal), `cell.fillFaceColor`(빈 글이면 배경 없음), `cell.fillHatchStyle` 등.
표에는 `format`(`cellSpacing`, `pageBreak`, `repeatHeader`, 안쪽 여백, `table.` 테두리/배경)과 `placement`(아래 개체 놓기)가 붙는다.
칸 테두리·배경을 고치기 전에 이 값으로 지금 모양을 본다(`cellBorder` 는 켜고 끄기라 결과가 지금 선에 달렸다).

### section
`{"call":"section","section":1}` → `{"section":1,"format":{...}}`. 구역(1부터) 하나의 쪽 모양.
`paperWidth`·`paperHeight`(세로로 놓은 크기), `landscape`, 여백 `marginLeft`·`marginRight`·`marginTop`·`marginBottom`·
`marginHeader`·`marginFooter`·`marginGutter`, `gutterType`(0 한쪽, 1 맞쪽, 2 위로), `lineGrid`·`charGrid`(0 이면 끔), `wongoji`,
`textDirection`(0 가로, 1·2 세로), 단 `columnCount`·`columnGap`·`columnType`(`NEWSPAPER` 일반, `BALANCED_NEWSPAPER` 배분,
`PARALLEL` 평행)·`columnSameSize`·`columnLineType`, `pageStart`(0 이면 앞 구역에 이어서), `sectionDefaultTab`,
`hideHeader`·`hideFooter`·`hideMaster`·`hidePageNum`. 길이는 HU(1mm ≈ 283). 용지 값은 `pageSetup` op 로 같은 이름으로 고친다.

### layout
`{"call":"layout","page":{"order":3}}` → 그 쪽에 그려진 배치. 그림(`render`) 없이 줄 나눔·넘침·표 자리를 본다.
좌표는 쪽 왼쪽 위에서 잰 HU(1mm ≈ 283)이고 그리기와 같은 자리 계산이다.
`{"page":{...},"width":..,"height":..,"body":{x,y,width,height},"lines":[...],"boxes":[...]}`.

- `body`: 본문 틀(여백 안). 본문 줄은 이 안에 있어야 한다.
- `lines`: 그리는 차례의 줄. `story`·`paragraph`·`from`·`to`(그 줄 글자 범위, `to` 글자는 다음 줄),
  줄 상자 `x`·`y`·`width`·`height`, `inkRight`(글자가 실제로 끝나는 오른쪽. 줄 끝 빈칸은 뺀다),
  `heading`(그 줄 앞에 그려진 문단 번호·글머리, 예 `"1."` `"가)"`), `text`(줄 글),
  `band`(머리말·꼬리말·쪽 번호), `master`(바탕쪽), `turned`(세로쓰기 틀 안. 좌표가 돌린 틀 기준),
  `overflow`(본문 줄이 본문 틀 아래로 나갔다).
- `boxes`: `table`·`cell`·`picture`·`shape`·`equation`·`container`·`column` 상자. 칸은 칸 글 `story` 와 `row`·`col`
  (`rowSpan`·`colSpan`), 개체는 박힌 문단 `story`·`paragraph`, 글자처럼 취급이면 `inline`.
- `"text":false` 를 주면 줄 글을 빼서 짧게 받는다.

쓰는 법: 고친 뒤 그 쪽 `layout` 을 다시 읽어 `overflow`, 줄이 `inkRight > x + width` 인지(글이 틀 밖), 문단이
몇 줄로 나뉘었는지(같은 `paragraph` 의 줄 수), 표가 쪽 끝에서 잘렸는지(`cell` 상자가 `body` 아래로 나감),
제목 줄이 쪽 맨 끝에 홀로 남았는지(마지막 줄의 문단 다음 줄이 다음 쪽 첫 줄)를 본다. 눈으로 볼 모양(색·굵기)만 `render` 로 확인한다.

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
문단의 `pageOrder` 는 첫 글자가 놓인 쪽의 물리 순서(`pages` 의 `order`)다. 쪽에 놓이지 않는 글(바탕쪽 등)에는 없다.
`paragraph` 응답에도 같은 칸이 있다.

### paragraph
`{"call":"paragraph","story":1,"paragraph":2}` → 글, 스타일, 정렬(`JUSTIFY` `LEFT` `RIGHT` `CENTER` `DISTRIBUTE` `DISTRIBUTE_SPACE`),
줄 간격(`lineSpacingType`: `PERCENT` 면 %, 나머지 HU), 들여쓰기·문단 간격(HU), 글자 모양 구간 `runs[{from,to,format{font,size,bold,italic,underline,strikeout,color}}]`,
개체 `objects[{index,kind,at,inline,width,height,placement,script,stories}]`.

개체 칸의 뜻은 「문서 구조」, `placement` 값의 뜻은 「개체 놓기」에 있다.

`"detail":true` 를 주면 모양 값을 모두 붙인다. `paraFormat` 은 문단 모양 전부, `runs` 마다 `charShape` 번호가 붙고
`charShapes` 는 그 번호마다 글자 모양 전부다. 이름은 `format` op 의 `set` 이름과 같아서 읽은 값을 고쳐 그대로 넣을 수 있다
(예: 한 구간의 `charShapes` 값을 그대로 다른 구간의 `set` 에 넣으면 같은 모양이 된다). 값이 많으니 모양을 고치거나
맞출 때만 쓴다.

### find
`{"call":"find","text":"찾을 글","story":0,"matchCase":true,"limit":100}` →
`{"matches":[{"from":위치,"to":위치,"page":{"page":1,"section":1},"context":"앞뒤 글"}]}`.
문단 경계는 넘지 않는다. `matchCase:false` 는 ASCII 만 가린다.

## 그림

### render
`{"call":"render","page":1,"dpi":96,"pixelHeight":0,"name":"p1"}` → `{"file":"<out-dir>/p1.png","page":{...},"width":...,"height":...}`.
`pixelHeight` 를 주면 dpi 대신 그것을 쓴다(32~8000, 넘거나 모자라면 끝값으로 자른다). `name` 이 없으면 `page-s<구역>-p<쪽>.png`
(번호가 겹치는 쪽은 `-n<nth>` 가 붙는다). `--out-dir` 폴더는 없으면 만든다. 파일을 못 쓰면 `SaveFailed`, 그리기 자체가
실패하면 `RenderFailed` 다.

## 개체 놓기

표·그림·도형·수식 같은 개체는 **글자처럼 취급**하거나 **띄워 놓는다**. 한글 개체 속성의 「기본」 탭과 같은 값이다.
값은 `paragraph` 의 `objects[].placement` 나 `{"call":"view","op":"objectProperties","object":"1:0:0"}` 로 읽고,
`objectProperties` op 의 `set` 으로 고친다(이름이 같다). 길이는 모두 HU(1mm ≈ 283.46).

### 글자처럼 취급(`treatAsChar: true`)

- 개체가 글자 하나(`￼`)가 되어 글 흐름 안에 앉는다. 앞뒤 글을 지우거나 넣으면 글자처럼 같이 움직인다.
- 개체 아래 끝이 그 줄의 글자 기준선에 놓이고, 줄 높이가 개체만큼 커진다. 문단 정렬(`align`)을 그대로 따른다.
- 기준·정렬·거리(`*RelTo`·`*Align`·`*Offset`)와 `textWrap` 은 쓰지 않는다(값은 남아 있다가 띄우면 쓰인다).
- `insertPicture`·`insertTable` 은 늘 글자처럼 넣는다. 그림 처음 크기는 픽셀 × 75 HU 이고 단보다 넓으면 단 폭으로 줄인다.

### 띄워 놓기(`treatAsChar: false`)

개체는 붙은 문단(앵커)에 속하지만 글자를 차지하지 않고, 아래 값으로 정한 자리에 놓인다.

| 이름 | 값 | 뜻 |
|---|---|---|
| `textWrap` | `SQUARE` | 어울림: 개체 상자 옆으로 글이 흐른다 |
| | `TOP_AND_BOTTOM` | 자리 차지: 개체가 걸친 줄에는 글이 없고 위아래로만 흐른다 |
| | `BEHIND_TEXT` | 글 뒤로: 글은 그대로 흐르고 개체가 글 밑에 깔린다(바탕 그림) |
| | `IN_FRONT_OF_TEXT` | 글 앞으로: 글은 그대로 흐르고 개체가 글을 덮는다(도장·로고) |
| `textFlow` | `BOTH_SIDES`·`LEFT_ONLY`·`RIGHT_ONLY`·`LARGEST_ONLY` | 어울림일 때 글이 흐를 쪽(양쪽·왼쪽만·오른쪽만·넓은 쪽만) |
| `horzRelTo` | `PAPER` 종이, `PAGE` 쪽(여백 안 본문), `COLUMN` 단, `PARA` 문단(문단 여백 안) | 가로 기준 영역 |
| `vertRelTo` | `PAPER` 종이, `PAGE` 쪽(여백 안 본문), `PARA` 문단 | 세로 기준 영역 |
| `horzAlign` | `LEFT`·`CENTER`·`RIGHT`(·`INSIDE`·`OUTSIDE` 맞쪽) | 기준 영역의 어느 쪽에 맞출지 |
| `vertAlign` | `TOP`·`CENTER`·`BOTTOM` | 같은 규칙(세로) |
| `horzOffset`·`vertOffset` | HU | 맞춘 쪽에서 안으로 잰 거리 |
| `flowWithText` | 참거짓 | 쪽 영역 안으로 제한(문단 기준일 때 개체가 본문 밖으로 나가지 않게) |
| `allowOverlap` | 참거짓 | 다른 개체와 겹쳐도 되는가 |
| `zOrder` | 수 | 겹칠 때 차례. 클수록 앞 |
| `marginLeft`·`marginRight`·`marginTop`·`marginBottom` | HU | 바깥 여백. 어울림·자리 차지 때 글이 이만큼 더 비켜 간다 |
| `width`·`height` | HU | 크기. 하나만 바꾸면 비율이 깨진다. 비율을 지키려면 둘 다 계산해서 준다 |

거리 재는 법(정렬 쪽에서 안으로):

- `LEFT`/`TOP`: 기준 영역 왼쪽(위) 끝 + 거리 = 개체 왼쪽(위) 끝
- `RIGHT`/`BOTTOM`: 기준 영역 오른쪽(아래) 끝 − 거리 = 개체 오른쪽(아래) 끝
- `CENTER`: 기준 영역 가운데에 개체 가운데를 맞추고 거리만큼 오른쪽(아래)으로 민다
- 세로 기준이 `PARA` 면 거리는 **개체가 붙은 글자가 든 줄의 위**에서 잰다. 그 문단이 다음 쪽으로 밀리면 개체도 같이 간다.
- 기준이 `PAPER`·`PAGE` 면 개체는 앵커 문단이 놓인 쪽의 그 자리에 고정된다(글이 늘어도 자리는 그대로, 쪽은 앵커를 따라간다).

### 바꿀 때 알아 둘 것

- **글자처럼 취급을 끄면 그 `￼` 글자가 문단에서 빠진다.** 그 문단 뒤쪽 글자 순번이 하나씩 당겨진다. 다시 켜면 같은 자리(`at`)에
  글자로 돌아온다. 개체 이름(`index`)은 그대로다.
- 띄운 개체를 지우려면 `deleteObject`, 글자처럼 취급한 개체는 `deleteObject` 나 그 `￼` 한 글자를 `delete` 해도 된다.
- 표 크기를 `width`·`height` 로 바꾸면 칸 폭·줄 높이를 비율대로 나눈다(화면의 손잡이 크기 조절과 같다).
- 고친 뒤 `layout` 의 `boxes`(kind `picture`·`table`·`shape`)로 개체 상자 좌표를 보고, 글이 어떻게 비켜 갔는지는 `render` 로 본다.

### 그림 넣기 예

넣는 op 의 응답 `object` 가 새 개체 이름이다. 한 apply 안에서는 응답을 미리 볼 수 없으니 이름을 셈해서 쓴다:
`index` 는 그 문단에서 넣는 자리 앞에 붙은 개체 수다(아래 예처럼 개체가 없는 문단 맨 앞이면 0).
구역 첫 문단에는 머리말·꼬리말 개체가 `at` 0 에 붙어 있을 수 있으니, 셈이 확실하지 않으면 apply 를 둘로 나눠
첫 apply 응답의 `object` 를 다음 apply 에 쓴다.

1. **문단 사이 가운데 그림**: 빈 문단을 만들고(`insertText` 로 `"\n"`), 그 문단에 글자처럼 넣고 문단을 가운데 정렬한다.
   ```json
   {"op":"insertPicture","at":"1:1:0","file":"그림.png","width":17008},
   {"op":"format","range":"1:1:0-1:1:1","set":{"align":"CENTER"}}
   ```
2. **글 오른쪽에 어울림**: 문단 맨 앞에 넣고 띄워서 단 오른쪽 위에 붙인다. 글이 왼쪽으로 흐른다.
   ```json
   {"op":"insertPicture","at":"1:3:0","file":"사진.jpg","width":14173},
   {"op":"objectProperties","object":"1:3:0","set":{"treatAsChar":false,"textWrap":"SQUARE",
     "horzRelTo":"COLUMN","horzAlign":"RIGHT","horzOffset":0,"vertRelTo":"PARA","vertAlign":"TOP","vertOffset":0,
     "marginLeft":567}}
   ```
3. **종이 오른쪽 위 고정 로고**: 종이 기준, 글 앞으로. 오른쪽·위에서 15mm(4252) 안쪽.
   ```json
   {"op":"objectProperties","object":"1:0:0","set":{"treatAsChar":false,"textWrap":"IN_FRONT_OF_TEXT",
     "horzRelTo":"PAPER","horzAlign":"RIGHT","horzOffset":4252,"vertRelTo":"PAPER","vertAlign":"TOP","vertOffset":4252}}
   ```
4. **쪽 전체 바탕 그림**: 종이 크기로 넣고(`pages` 의 `width`·`height`) 글 뒤로 보낸다.
   ```json
   {"op":"insertPicture","at":"1:0:0","file":"배경.png","width":59528,"height":84186},
   {"op":"objectProperties","object":"1:0:0","set":{"treatAsChar":false,"textWrap":"BEHIND_TEXT",
     "horzRelTo":"PAPER","horzAlign":"LEFT","horzOffset":0,"vertRelTo":"PAPER","vertAlign":"TOP","vertOffset":0}}
   ```
5. **그림 아래 캡션**: `{"op":"caption","object":"1:1:0","caption":"BOTTOM","text":" 설명 글"}` → "그림 1 설명 글".

도형(`insertShape`)은 처음부터 띄운 개체(종이 기준, 글 앞으로)로 들어간다. 같은 `objectProperties` 로 바꾼다.

## 고치기

### apply
`{"call":"apply","ops":[op, ...]}` → `{"pages":15,"fontSwaps":[{"from":"여기어때 잘난체","to":"함초롬바탕"}],"ops":[{"end":위치,"cells":[[4,5,6],[7,8,9]]}]}`.
`end` 는 op 뒤 캐럿 자리(넣은 글의 끝), `cells` 는 insertTable 이 만든 칸의 story[행][열],
`object` 는 insertPicture·insertShape 가 만든 개체 이름 `{story, paragraph, index}` 다.
표 구조 op 의 응답에는 고친 뒤의 `table`(위 table 호출과 같은 모양)이 붙는다. 표를 지웠으면 `rows` 가 0 이다.
`fontSwaps` 는 편집 앞에 바꾼 라이선스로 막힌 글꼴이다(있을 때만). 문서 전체에서 그 글꼴 이름이 바뀐다.

| op | 인자 | 하는 일 |
|---|---|---|
| `insertText` | `at`, `text` | 글을 넣는다. `\n` 은 문단 나눔, ` ` 은 문단 안 줄바꿈, `\t` 는 탭 |
| `delete` | `range` | 지운다. 문단을 넘으면 문단을 합친다 |
| `replace` | `range`, `text` | 지우고 그 자리에 넣는다. 넣은 글은 앞 글자의 모양을 따른다 |
| `format` | `range`, `set`, `style` | `set` 의 서식을 입힌다. `style` 은 문단 스타일 이름(예: `"바탕글"`, `"개요 1"`) |
| `insertTable` | `at`, `rows`, `cols`, `cells` | 표를 넣는다(단 폭, 글자처럼 취급). 표는 제 문단에 놓인다. `cells` 는 `[["가","나"],["다"]]` |
| `insertPicture` | `at`, `file` 또는 `base64`, `width`, `height` | 그림(png·jpg·gif·bmp)을 **글자처럼** 넣는다(`at` 자리에 `￼` 한 글자). `file` 은 CLI 가 읽는 경로, `base64` 는 파일 바이트의 base64(`data:image/png;base64,...` 도 된다). 크기는 HWPUNIT 이고 없으면 픽셀 × 75(단보다 넓으면 단 폭), 하나만 주면 그림 비율을 지킨다. 응답 `object` 가 새 그림 이름이다. 띄워 놓으려면 이어서 `objectProperties` 로 바꾼다(「개체 놓기」의 예) |
| `setText` | `story`, `text` | 그 story 글 전체를 바꾼다. 표 칸을 고칠 때 쓴다 |
| `insertRows` | `story`, `side`(`below`·`above`), `count` | story 칸이 걸친 줄의 아래(위)에 줄을 넣는다 |
| `insertColumns` | `story`, `side`(`right`·`left`), `count` | story 칸이 걸친 칸의 오른쪽(왼쪽)에 칸을 넣는다. 표가 그만큼 넓어진다 |
| `deleteRows` | `story`, `toStory` | 두 칸을 덮는 사각형이 걸친 줄을 지운다(`toStory` 가 없으면 story 칸이 걸친 줄) |
| `deleteColumns` | `story`, `toStory` | 같은 규칙으로 칸을 지운다. 모든 줄·칸을 지우면 표가 없어진다 |
| `mergeCells` | `story`, `toStory` | 두 칸을 덮는 사각형을 한 칸으로 합친다 |
| `splitCell` | `story`, `rows`, `cols` | 칸을 rows × cols 로 나눈다 |
| `cellBorder` | `story`, `toStory`, `border`, `lineType`, `lineWidth`, `lineColor` | 두 칸을 덮는 셀 블록의 테두리 단추. `border` 는 `all`·`outside`·`inside`·`top`·`bottom`·`left`·`right`·`insideHorz`·`insideVert`·`none`·`diagonalDown`(╲)·`diagonalUp`(╱). 고른 선이 모두 있으면 지우고 아니면 긋는다(`none` 은 늘 지운다). 선은 맞닿은 이웃 칸에도 적는다. 선은 기본 실선 0.12mm 검정(`lineWidth` 는 HWPUNIT) |
| `cellFill` | `story`, `toStory`, `color` | 셀 블록 배경색 `"#RRGGBB"`. `"none"` 이면 배경을 없앤다 |
| `equalWidth` | `story`, `toStory` | 셀 너비를 같게: 줄마다 블록 칸이 블록 폭을 똑같이 나눈다(나머지는 올림). 다른 줄과 경계가 어긋나면 칸이 늘어난다 |
| `equalHeight` | `story`, `toStory` | 셀 높이를 같게: 칸마다 블록 칸이 블록의 그려진 높이(글이 늘린 높이까지)를 똑같이 나눈다(나머지는 올림) |
| `insertBreak` | `at`, `kind`(`page`·`column`·`columnDef`·`section`) | 그 자리에서 쪽·단·단 설정·구역을 나눈다. 뒤 글은 새 문단(구역이면 새 구역의 첫 문단)이 된다. 구역 본문에서만 된다 |
| `pageSetup` | `section`, `set`, `whole` | 구역(1부터)의 편집 용지를 `section` 읽기와 같은 이름으로 고친다(용지·여백·제본·격자·글자 방향). `whole` 이면 모든 구역 |
| `replaceAll` | `find`, `text`, `matchCase` | 문서(본문·표 칸·글상자·머리말 등)의 모든 `find` 를 `text` 로 바꾼다(한글 모두 바꾸기). i 번째 새 글자는 찾은 i 번째 글자의 모양을, 남는 새 글자는 찾은 마지막 글자의 모양을 받는다. `matchCase` 가 없으면 영문 대소문자를 가리지 않는다. 문단은 넘지 않고 `text` 에 줄 끝을 넣을 수 없다. 응답 op 의 `replaced` 가 바꾼 수다 |

### 편집기 명령 op

편집기(한글 2024 와 같은 규칙)의 명령을 그대로 부른다. 판정과 거절 까닭은 편집기와 같다.

| 인자 | 뜻 |
|---|---|
| 자리 | `range`(블록) > `at`(캐럿) > `story`(그 글 묶음 첫 자리, 표 칸이면 그 칸) > `section`(구역 본문 첫 자리) 가운데 처음 것. 명령은 그 자리의 문단·구역·표에 걸린다 |
| `object` | 개체 이름 `{"story":1,"paragraph":0,"index":0}` 또는 `"1:0:0"`(`paragraph` 의 `objects[index]`). 그 개체를 고른 채로 명령한다 |
| `set` | 대화 상자 값 `{이름: 값}`. 이름은 `view` 로 읽은 이름과 같다(읽은 값을 고쳐 그대로 넣는다) |
| `text` | 머리말·꼬리말·바탕쪽·각주·캡션·글상자를 만든 뒤 그 안에 칠 글. 응답 `end` 가 그 글 묶음 안의 끝 자리다 |
| 그 밖 | 명령마다 아래 인자(글·수·참거짓) |

| op | 자리·인자 | 하는 일 |
|---|---|---|
| `header`, `footer` | 자리, `set.apply`(`BOTH`·`ODD`·`EVEN`), `text` | 머리말·꼬리말을 만들고 들어간다 |
| `editBand` | 자리, `set.band`(`header`·`footer`)·`set.apply`, `text` | 그 쪽에 보이는 머리말·꼬리말로 들어간다(`text` 를 그 앞에 친다) |
| `deleteBand` | `at`(머리말·꼬리말 글 안) | 그 머리말·꼬리말을 지운다 |
| `pageNumber` | 자리, `set.pos`(`BOTTOM_CENTER` …)·`format`(`DIGIT` …)·`sideChar`·`start` | 쪽 번호 매기기 |
| `newNumber` | 자리, `set.numType`·`number` | 새 번호로 시작 |
| `pageHiding` | 자리, `set.hideHeader`·`hideFooter`·`hideMaster`·`hideBorder`·`hideFill`·`hidePageNum` | 현재 쪽만 감추기 |
| `insertPageNum` | `at` | 쪽 번호 넣기(자동 번호) |
| `masterPage` | 자리, `set.type`(`BOTH`·`ODD`·`EVEN`·`LAST_PAGE`·`OPTIONAL_PAGE`)·`pageNumber`, `text` | 바탕쪽을 만들거나 들어간다. apply 가 끝나면 바탕쪽 편집에서 나온다 |
| `deleteMaster` | 자리, `set.scope`(`current`·`section`·`document`) | 바탕쪽 지우기 |
| `masterOptions` | 자리, `set.front`·`duplicate`·`excludeFirst` | 바탕쪽 설정 |
| `deletePage` | 자리, `set.range`(`current`·`selected`·`custom`)·`pages`(`"2-3,5"`)·`pageNumber` | 쪽 지우기 |
| `insertMemo` | `range`(없으면 `at` 낱말), `set.text`·`author` | 메모 |
| `editMemo`, `deleteMemo` | `at`(메모 글 안), `set.text` | 메모 고치기·지우기 |
| `insertProofMark` | 범위 부호는 `range`, 자리 부호는 `at`, `set.kind`·`text`(·`start`·`gap` pt) | 교정 부호. `kind`: `delete`·`clipping`·`attach`·`lineAttach`·`sawtooth`·`thinking`·`praise`·`line`·`simpleChange`·`change`·`space`·`lineSeparate`·`lineInsert`·`symbol`·`insert`·`rightMove`·`leftMove` |
| `blockCalc` | `range`, `set.mode`(`sum`·`average`) | 블록 합계·평균을 블록 뒤에 넣는다 |
| `insertNote` | `at`, `endnote`, `text` | 각주(미주)를 넣고 그 안에 친다 |
| `exchangeNotes` | `mode`(0 각주→미주, 1 미주→각주, 2 서로) | 문서 전체 주를 바꾼다. 자리는 없어도 된다 |
| `noteShape` | 자리, `endnote`, `whole`, `set` | 주석 모양 |
| `pageBorder` | 자리, `whole`, `set` | 쪽 테두리/배경 |
| `sectionSettings` | 자리, `set` | 구역 설정 |
| `columns` | 자리, `set.count`·`type`·`gapMm`·`line`·`preset`(`one`·`two`·`three`·`left`·`right`)·`applyTo`(`WHOLE`·`NEW_PAGE`·`NEW_COLUMN`) | 단 설정 |
| `tableProperties` | `story`(표 칸, 칸 블록은 `range`), `set` | 표/셀 속성(쪽 나눔·제목 줄 반복·안·바깥 여백·놓기·셀 여백·세로 정렬 …, 길이 이름은 `Mm` 끝). 쪽 나눔 `pageBreak`: `CELL` 은 한글의 "나눔"(칸 안 글 줄 사이에서도 나눈다), `TABLE` 은 "셀 단위로 나눔"(줄을 통째로 다음 쪽으로), `NONE` 은 "나누지 않음" |
| `tableToText` | `story`(표 칸), `set.delimiter`(`tab`·`comma`·`space`·`other`)·`text` | 표를 문자열로 |
| `textToTable` | `range`, `set.delimiter`(`auto`·`tab`·`comma`·`space`·`char`·`string`)·`text`·`keep` | 문자열을 표로 |
| `caption` | `object` 또는 `story`(표 칸), `caption`(`TOP`·`BOTTOM`·`LEFT_TOP` … `NONE`), `text` | 캡션 달기·떼기. 새 캡션이면 `text` 를 번호 뒤에 친다 |
| `bookmark` | `at`, `action`(`insert`·`delete`·`rename`), `name`, `newName` | 책갈피 |
| `insertHyperlink` | `at` 또는 `range`(고른 글은 바뀐다), `url`, `text` | 하이퍼링크 |
| `glyphGroup` | `range` 또는 `at`, `set.kind`(`ruby`·`compose`)·`mainText`·`subText`·`pos`·`ratio`·`chars`·`circle` | 덧말·글자 겹치기 |
| `convertText` | `range`, `set.kind`(`case`·`width`)·`mode`(`upper`·`lower`·`toggle`·`wordCap`·`sentenceCap`)·`to`(`half`·`full`) | 대문자/소문자·전각/반각 바꾸기 |
| `sortParagraphs` | `range`, `set.key1Position`·`key1Format`·`key1Desc`·`delimiter` … | 본문 정렬 |
| `deleteMarks` | `range`, `set.marks`(지울 조판 부호 이름, 줄로 나눔) | 조판 부호 지우기 |
| `copyShape` | `at`(가져올 자리), `range`(입힐 블록), `type`(0 글자, 1 문단, 2 둘 다, 4 스타일) | 모양 복사 |
| `insertShape` | `page`, `tool`(`rect`·`ellipse`·`line`·`textBox`·`verticalTextBox`·`arc`·`polygon`·`curve`·`freeDrawing`), 상자 도형은 `x`·`y`·`width`·`height`(HU 또는 `"30mm"`), 점 도형(`polygon`·`curve`·`freeDrawing`)은 `points`(`"x,y x,y ..."` 종이 좌표 HU), `text` | 종이 좌표에 도형을 그린다(글 앞, 종이 기준, 선 검정 0.12mm). 응답 op 의 `object` 가 새 개체 이름이다. 선은 (x, y)→(x+width, y+height). `arc` 는 상자 왼쪽 위가 중심인 사분 호, `polygon` 은 점 3개 이상(흰 채움), `curve` 는 점 2개 이상을 잇는 곡선(흰 채움), `freeDrawing` 은 채우지 않은 꺾은선이다 |
| `insertEquation` | `at`, `script`(한글 수식 스크립트), `size`(pt, 기본 10) | 수식을 **글자처럼** 넣는다(`at` 자리에 `￼` 한 글자). 상자 크기·기준선은 한글 수식 엔진과 같게 잰 값이다. 응답 `object` 가 새 수식 이름이다. 스크립트 예: `x = {-b +- sqrt {b^2 - 4ac}} over {2a}`, `sum _{k=1} ^{n} k^2`, `lim _{x -> 0} {sin x} over x` |
| `equation` | `object`, `script`, `size`(pt, 주지 않으면 그대로) | 수식을 고친다. `view` 로 지금 `script`·`size` 를 읽는다 |
| `objectProperties` | `object`, `set` | 개체 놓기·크기. 값의 뜻과 예는 「개체 놓기」(`paragraph` 의 `placement` 이름: `treatAsChar`, `textWrap`, `textFlow`, `vertRelTo`, `horzRelTo`, `vertAlign`, `horzAlign`, `vertOffset`, `horzOffset`, `width`, `height`, `zOrder`, `flowWithText`, `allowOverlap`, 바깥 여백 `marginLeft`·`marginRight`·`marginTop`·`marginBottom`. HU). 크기는 손잡이 크기 조절과 같다(표는 칸·줄을 비율대로) |
| `deleteObject` | `object` | 개체를 지운다(표면 칸 글까지) |

`header` 등 쪽 탭 명령은 `{"op":"pageControl","control":"header",...}` 로도 부를 수 있다.
값을 모르면 먼저 `view` 로 읽는다.

### view
`{"call":"view","op":"sectionSettings","section":1}` → `{"values":{...}}`. 요청은 명령 op 와 같은 모양이고 문서는 바뀌지 않는다.
읽을 수 있는 op: 쪽 탭 명령(`header` 등), `noteShape`(`endnote`), `pageBorder`, `sectionSettings`, `columns`(`set` 을 얹은 값),
`tableProperties`, `glyphGroup`, `sortParagraphs`, `bookmark`(이름 목록 `names`), `objectProperties`.

표 구조 op 의 규칙은 한글 2024 와 같다.
- 합친 칸이 기준이면 그 칸 전체가 기준이다. 아래 넣기는 그 칸의 마지막 줄 뒤, 줄 지우기는 그 칸이 걸친 줄 모두다.
- 새 줄(칸)은 기준 줄(칸)의 높이(폭)·칸 나눔·테두리/배경을 따르고 비어 있다. 넣는 자리를 가로지르는 합친 칸은 늘어난다.
- 지우는 줄을 일부만 걸친 합친 칸은 줄고 글은 남는다. 어느 칸도 나누지 않게 된 경계선은 없어진다(줄·칸 수가 더 줄 수 있다).
- 합치기는 칸 글을 칸 자리 차례로 잇고 빈 칸은 건너뛴다. 사각형 밖으로 걸친 합친 칸이 있으면 `BadArgument` 다.
- 나누기는 폭을 똑같이 나누고 글은 첫 칸에 남는다.
- 고친 뒤에는 칸 story 번호가 바뀐다. 다음 op 의 칸은 응답의 `table` 에서 고른다.

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
| `--verbose` | core 진단 기록(글꼴 받기·대체 시도 등)을 stderr 로 낸다. 주지 않으면 버린다. 빠진 글자는 이 기록 말고 `info` 의 `missingGlyphs` 로 본다 |

받지 못한 파일은 캐시에 `파일.missing` 표시를 두면 그 실행 안에서 다시 묻지 않는다.

## 저장

### save
`{"call":"save","path":"out/고친본.hwpx","format":"hwpx"}` → `{"file":"...","format":"hwpx","bytes":12345}`.
`format` 이 없으면 확장자, 그것도 없으면 연 형식이다. CLI 의 `--save 경로` 는 마지막에 이 요청을 하나 더 부른다.
PDF 로도 쓴다: `{"call":"save","path":"out/고친본.pdf","dpi":200}`(또는 `"format":"pdf"`). 편집기의 인쇄·PDF 로 저장과 같이
쪽마다 `dpi`(72~600, 기본 200)로 그린 그림을 종이 크기에 채운다(글은 그림이라 PDF 에서 고르거나 찾을 수 없다).
