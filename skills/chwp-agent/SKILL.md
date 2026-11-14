---
name: chwp-agent
description: 한글(HWP·HWPX) 문서를 열어 글을 읽고, 찾고, 고치고(글 넣기·지우기·바꾸기·모두 바꾸기·글자/문단 서식·표 넣기·표 칸 채우기·표 줄/칸 넣기·지우기·칸 합치기·나누기·셀 테두리/배경·셀 크기 같게·그림·머리말/꼬리말·쪽 번호·바탕쪽·각주/미주·메모·교정 부호·단·구역·캡션·책갈피·하이퍼링크·도형(호·다각형·곡선·자유선 포함)·수식·개체 속성), 쪽을 PNG 로 그려 확인하고, HWP·HWPX 또는 PDF 로 저장한다. 사용자가 .hwp/.hwpx 파일의 내용을 묻거나 고쳐 달라고 할 때 쓴다.
---

# chwp-agent

ClassingHWP core 의 에이전트 API 를 부르는 CLI 로 한글 문서를 다룬다. 조판·편집 판단은 모두 core 가 한다.
요청과 응답은 한 줄에 JSON 하나다. 호출·op·서식 이름의 모든 모양은 [reference.md](reference.md) 에 있다.

## 부르는 법

```bash
python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_agent.py <문서> [요청.jsonl | -] [-c JSON]... [--out-dir 폴더] [--save 출력.hwp|.hwpx]
```

스크립트는 이 스킬 폴더의 `scripts/chwp_agent.py` 다(`${CLAUDE_SKILL_DIR}` 가 바뀌지 않는 곳에서는 이 SKILL.md 가 있는
폴더 기준 경로로 부른다). Windows 에서는 `python3` 대신 `python` 이다. 실행 파일은 `bin/chwp-agent`(Linux) 또는 저장소 빌드를 쓴다.

- 한 번 실행이 한 세션이다. 요청은 파일 → 표준 입력(`-`) → `-c` 차례로 같은 문서에 이어서 부른다.
- 응답은 요청마다 stdout 한 줄: `{"ok":true,...}` 또는 `{"ok":false,"error":{"code","message"}}`.
  실패 까닭은 늘 이 응답의 `error.message` 에 있다. stderr 는 글꼴 받기·GPU 같은 진단 글이라 `2>/dev/null` 로
  버려도 된다(core 의 자세한 글꼴 기록은 `--verbose` 를 줄 때만 나오고, 고장 찾기에만 쓴다).
- **문서 상태는 파일로 넘긴다.** 다음 실행에서 고친 결과를 이어 쓰려면 `--save` 로 저장한 파일을 다시 연다.
  원본을 덮어쓰지 말고 새 경로에 저장하고, 사용자가 원할 때만 원본 자리에 쓴다.
- 그림(`render`)은 `--out-dir` 폴더에 쓰고 응답의 `file` 에 경로를 준다. 그 PNG 를 Read 로 열어 눈으로 확인한다.
- 실행 파일이 없으면(저장소에서 쓸 때) `cmake --build build/windows-msvc --config Release --target chwp-agent` 로 빌드한다.
- claude.ai 에서는 사용자가 올린 파일과 결과 파일의 자리를 그 환경 안내대로 쓴다. 결과(hwp·hwpx·png)는 사용자가
  내려받을 수 있는 출력 폴더에 저장한다.
- **글꼴은 스크립트가 알아서 받는다.** 문서를 열기 전에 core 가 필요한 글꼴 파일을 정하고, 스크립트가 그것만
  글꼴 서버(`https://ede.kr/hwp/fonts/`, `CHWP_FONT_BASE` 로 바꿈)에서 받아 캐시(`~/.cache/chwp-agent/fonts`,
  `CHWP_FONT_CACHE`)에 넣는다. 글꼴 파일은 한 번 받으면 다시 받지 않고, 글꼴 목록(`index.json` 등)은 실행마다
  바뀌었는지만 묻는다. 고친 목록을 쓰려면 `CHWP_FONT_INDEX`·`CHWP_FONT_LICENSE_SWAP`·`CHWP_FONT_REPLACE` 에 파일 경로나
  주소를 준다(reference.md 의 글꼴). **`--no-font-fetch` 는 붙이지 않는다.** 네트워크가 막혔을 거라 짐작해 미리
  붙이면 받을 수 있던 글꼴도 빠진다. 받기가 실패해도 스크립트는 stderr 에 한 줄 남기고 캐시·대체 글꼴로 이어 간다.
- `{"call":"info"}` 의 `missingFonts` 가 비어 있지 않으면 그 글꼴은 대체 글꼴로 재고 그렸다. 줄 나눔·그림이 한글과
  다를 수 있다고 사용자에게 알린다. `missingGlyphs` 가 비어 있지 않으면 그 글자(글머리표·기호 등)는 그림에서 비어
  나온다. 그림을 줄 때 어느 글자가 빠질 수 있는지 함께 알린다.
- 라이선스로 막힌 글꼴(서버에 폭만 있는 글꼴)이 든 문서를 고치면, 편집기와 같이 편집 앞에 대역 원본 글꼴로 바꾼다.
  apply 응답의 `fontSwaps`(`{from,to}`)에 나오니 **반드시 사용자에게 어떤 글꼴이 바뀌었는지 알린다.**
- 한글 글은 `-c` 보다 요청 파일(UTF-8, JSON Lines)에 써서 넘기는 편이 따옴표 문제가 적다.

## 자주 하는 일

### 문서 분석(요약·질문 답하기)

글과 쪽 그림을 함께 본다. **내용·숫자·날짜·인용은 글에서, 짜임(표의 머리·병합, 그림, 서식 칸, 강조)은 그림에서** 얻는다.

1. **훑기**: `{"call":"info"}` 로 쪽 수, `missingFonts`, `missingGlyphs` 를 본다.
2. **글 읽기**: 글 전체를 평문으로 받는다. 이 글이 답의 근거다.

   ```bash
   python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_text.py "문서.hwp" > text.txt 2>/dev/null
   ```

   `scripts/chwp_text.py` 는 글 전체를 읽는 차례대로 쓴다. 쪽이 바뀌는 자리에 `=== 03_2쪽 ===`(물리 순서_쪽 번호)
   경계가 있고, 아래 쪽 그림 파일 이름과 같다. 표는 본문 제자리에 행마다 `| 칸 | 칸 |` 로 들어가고, 머리말·각주
   등은 끝에 `[owner]` 머리로 붙는다. 바탕쪽 글은 빠진다(`--master` 로 넣음).
3. **쪽 그림 훑기**: 아래 「모든 쪽 그림으로 만들기」로 **96dpi**(30쪽이 넘으면 72dpi) 그림을 만들고, Read 로 훑어
   짜임을 잡는다. 여러 장은 한 번에 나란히 Read 한다. 96dpi 그림의 작은 글자·숫자는 읽지 않는다.
4. **맞춰 보기**: 글만으로 헷갈리는 표(병합 칸, 여러 줄 머리)는 그림의 칸 모양대로 해석한다. 그림·도형·스캔 안에만
   있는 글(text 에는 `￼` 만 있음)이 필요하면 그 쪽을 150dpi 로 다시 그려 읽는다.
5. **답하기**: 쪽은 `=== 03_2쪽 ===` 의 쪽 번호(2쪽)로 말한다. 그림으로만 읽은 내용은 그렇다고 밝힌다.

고칠 자리(위치)가 필요할 때는 이 평문 말고 아래 `text`·`find` 응답을 직접 읽는다.

### 모든 쪽 그림으로 만들기

쪽 번호는 겹칠 수 있다(표지와 본문 첫 쪽이 둘 다 1쪽 등). **먼저 `pages` 를 부르고, 쪽마다 `order` 로 그린다.**
`order` 는 문서 안 물리 순서라 겹치지 않는다. 쪽 수를 짐작해 번호를 더듬지 않는다. `DPI` 는 쓰임에 맞게 고른다
(훑기 96, 쪽 수 많으면 72, 사용자에게 건넬 그림·그림 속 글 읽기 150).

```bash
python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_agent.py "문서.hwp" -c '{"call":"pages"}' 2>/dev/null > pages.jsonl
DPI=96 python3 -c "
import json, os
pages = json.loads(open('pages.jsonl', encoding='utf-8').readline())['pages']
with open('render.jsonl', 'w', encoding='utf-8') as out:
    for p in pages:
        name = '%02d_%s쪽' % (p['order'], p['label'])
        out.write(json.dumps({'call': 'render', 'page': {'order': p['order']}, 'dpi': int(os.environ['DPI']), 'name': name}, ensure_ascii=False) + '\n')
"
python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_agent.py "문서.hwp" render.jsonl --out-dir pages 2>/dev/null
```

`--out-dir` 폴더는 없으면 만든다. 응답 한 줄이 한 쪽이고, 실패한 쪽은 `error.message` 에 까닭이 있다.
사용자에게 그림을 건넬 때는 `info` 의 `missingGlyphs` 도 같이 보고, 몇 장은 Read 로 열어 눈으로 확인한 뒤 건넨다.

## 문서 구조 한눈에

```
구역(section, 1부터) ─ 용지·여백·단·쪽 번호·바탕쪽
└─ 구역 본문 story ─ 문단(0부터) ─ 글자(코드 포인트, 0부터)  → 위치 "story:문단:글자"
                              └─ 개체(objects, 0부터)       → 개체 이름 "story:문단:개체"
                                 └─ 안의 글 묶음: 표 칸·글상자·머리말/꼬리말·각주·캡션 (각자 story 번호)
```

- **글 묶음(story)** 마다 문단 목록이 따로다. 표 칸 글은 표 칸 story, 머리말 글은 머리말 story 에서 읽고 고친다.
  `{"call":"stories"}` 의 `kind`·`owner`·`parentStory`·`parentParagraph` 로 어디에 든 글인지 안다.
- **개체**는 문단에 붙는다. `paragraph` 응답의 `objects` 가 그 목록이고 `at`(붙은 글자 자리), `inline`(글자처럼 취급),
  `placement`(놓기), `stories`(안의 글 묶음)를 준다. 머리말·꼬리말은 보통 구역 첫 문단에 `at` 0 개체로 붙어 있다.
- **글자처럼 취급**한 개체는 글자 하나(U+FFFC ￼)라 글과 같이 흐르고, **띄운 개체**는 글자를 차지하지 않고 기준(종이·쪽·단·문단)과
  거리로 놓인다. 어울림(`SQUARE`)·자리 차지(`TOP_AND_BOTTOM`)·글 뒤로·글 앞으로가 글이 비켜 가는 방법이다.

## 그림·개체 놓기 요령

1. 그림은 `insertPicture` 로 넣는다. **늘 글자처럼** 들어가고, 응답 `object` 가 새 그림 이름이다.
2. 글 사이에 한 줄로 둘 그림은 그대로 둔다. 가운데 두려면 그 문단을 `format` 의 `align:"CENTER"` 로 한다.
3. 글 옆·종이의 정한 자리·글 뒤에 둘 그림은 이어서 `objectProperties` 로 `treatAsChar:false` 와 `textWrap`·기준·정렬·거리를 준다.
   지금 값은 `{"call":"view","op":"objectProperties","object":"1:0:0"}` 로 읽는다.
4. 띄우면 그 ￼ 글자가 빠져 뒤 글자 순번이 하나 당겨진다. 같은 apply 의 뒤 op 위치를 그만큼 고친다.
5. 놓은 뒤 `layout` 의 `boxes` 로 자리를, `render` 로 글이 어떻게 비켜 갔는지 눈으로 본다.

값의 뜻(기준 영역, 거리를 어느 쪽에서 재는지, 문단 기준 세로 거리 등)과 예 다섯 가지(가운데 그림, 오른쪽 어울림, 종이 오른쪽 위 로고,
쪽 전체 바탕 그림, 캡션)는 reference.md 의 「문서 구조」·「개체 놓기」에 있다. 그림을 놓기 전에 꼭 읽는다.

## 일하는 차례

1. **훑기**: `{"call":"info"}`, `{"call":"text"}` 로 글 전체를 읽는다. 길면 응답의 `next` 를 그대로 붙여 이어 읽는다.
   표는 문단 안 `objects` 의 `stories` 로 칸 글 묶음 번호를 알려 주고, 그 칸 글도 `text` 에 같이 나온다.
2. **자리 찾기**: `{"call":"find","text":"..."}` 로 고칠 글의 구간(`from`·`to`)과 쪽을 얻는다.
   위치는 `{story, paragraph, char}` 또는 `"story:문단:글자"` 이고, `char` 는 그 문단 글의 코드 포인트 순번이다.
   모양을 고치거나 맞출 때는 먼저 지금 값을 읽는다: 글자·문단 모양은 `{"call":"paragraph",...,"detail":true}`,
   쪽 모양(용지·여백·단)은 `{"call":"section","section":1}`, 표 칸 테두리·배경·여백은 `{"call":"table","story":N}`.
   읽은 이름이 곧 `format`·`pageSetup` 의 `set` 이름이다.
3. **고치기**: `{"call":"apply","ops":[...]}`. 한 apply 는 트랜잭션이다. 하나라도 실패하면 아무것도 바뀌지 않고
   `error.message` 가 `op N:` 으로 시작한다. op 의 위치는 앞 op 까지 적용한 문서 기준이므로, 한 apply 에 여러 곳을
   고칠 때는 **문서 뒤쪽부터** 고치거나 고친 뒤 다시 `find` 한다.
4. **확인**: 바뀐 문단을 `paragraph`/`text` 로 다시 읽고, 그 쪽을 `{"call":"layout","page":...}` 로 읽어
   줄 나눔·넘침(`overflow`)·표가 쪽에서 잘렸는지를 본다. 색·굵기처럼 눈으로 볼 모양만 `render` 로 그려 본다.
   찾은 결과의 `page` 를 그대로 `render` 의 `page` 로 넘기면 된다.
5. **저장**: `--save 경로` 또는 `{"call":"save","path":"..."}`. 확장자(.hwp/.hwpx/.pdf)로 형식을 정한다. PDF 는 쪽 그림을 묶은 것이다.
   틀렸으면 `{"call":"undo"}` 로 마지막 apply 를 되돌린다(같은 실행 안에서만).

## 예

```jsonl
{"call":"find","text":"2025학년도"}
{"call":"apply","ops":[{"op":"replace","range":"1:12:4-1:12:10","text":"2026학년도"},{"op":"format","range":"1:12:4-1:12:10","set":{"bold":true,"color":"#C00000"}}]}
{"call":"render","page":{"page":1,"section":1},"dpi":100,"name":"p1"}
```

```bash
python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_agent.py "원본.hwp" req.jsonl --out-dir out --save "out/고친본.hwp" 2>/dev/null
```

## 알아 둘 것

- 글에서 U+FFFC(￼)는 글자처럼 놓인 개체(표·그림·수식·도형)와 각주·미주 번호 한 글자다. 지우면 그 개체가 지워진다. 띄운 개체는 글자가 없으니 `deleteObject` 로 지운다.
- 한글 전용 기호는 사용자 영역 글자(U+E000~U+F8FF)로 들어 있어 터미널에 안 보일 수 있다. 순번을 셀 때 빠뜨리지 않는다.
- 쪽은 쪽 번호 + 구역으로 가리키거나, `{"order":N}` 으로 물리 순서를 가리킨다. `AmbiguousPage` 가 오면
  `candidates` 에서 골라 다시 부르거나 `pages` 의 `order` 를 쓴다.
- 머리말·꼬리말·바탕쪽처럼 쪽에 놓이지 않는 글은 `NotEditable` 이 날 수 있다.
- 글자 서식은 빈 구간에 주면 효과가 없다. 문단 서식(정렬·줄 간격 등)은 빈 구간이어도 그 문단에 들어간다.
- 표 구조(`insertRows`·`insertColumns`·`deleteRows`·`deleteColumns`·`mergeCells`·`splitCell`)는 칸 story 로 가리킨다.
  먼저 `{"call":"table","story":칸}` 으로 칸 그림(합친 칸 포함)을 보고, 고친 뒤에는 응답의 `table` 에서 다음 칸을 고른다.
  칸 story 번호는 표를 고칠 때마다 바뀐다. 규칙은 reference.md 의 apply 표에 있다.
- 머리말·각주·쪽 번호·바탕쪽·메모·단·캡션·도형 같은 편집기 명령은 op 이름으로 부르고 자리(at·range·object·story·section)와
  `set` 을 준다. 값 이름은 먼저 `{"call":"view","op":"명령",자리}` 로 읽는다. 목록은 reference.md 의 「편집기 명령 op」다.
- 아직 없는 것: 수식 넣기, 좌표로 찾기(hitTest), 쪽 일부만 그리기.
