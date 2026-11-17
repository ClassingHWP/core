---
name: chwp-agent
description: 한글(HWP·HWPX) 문서를 열어 글을 읽고, 찾고, 고치고(글 넣기·지우기·바꾸기·모두 바꾸기·글자/문단 서식·표 넣기·표 칸 채우기·표 줄/칸 넣기·지우기·칸 합치기·나누기·셀 테두리/배경·셀 크기 같게·그림·머리말/꼬리말·쪽 번호·바탕쪽·각주/미주·메모·교정 부호·단·구역·캡션·책갈피·하이퍼링크·도형(호·다각형·곡선·자유선 포함)·수식·개체 속성), 쪽을 PNG 로 그려 확인하고, HWP·HWPX 또는 PDF 로 저장한다. 사용자가 .hwp/.hwpx 파일의 내용을 묻거나 고쳐 달라고 할 때 쓴다.
---

# chwp-agent

ClassingHWP core 로 한글 문서를 다루는 CLI 다. 조판·편집 판단은 모두 core 가 한다. 요청과 응답은 한 줄에 JSON 하나다.
**이 파일의 빠른 시작만 읽고 시작한다.** [reference.md](reference.md) 는 통째로 읽지 말고, 여기 없는 op·서식 이름이
필요할 때만 그 이름으로 찾아 그 부분만 읽는다.

```bash
python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_agent.py <문서> [요청.jsonl | -] [-c JSON]... [--out-dir 폴더] [--save 출력.hwp|.hwpx|.pdf]
```

`${CLAUDE_SKILL_DIR}` 이 없는 곳에서는 이 SKILL.md 가 있는 폴더 기준 경로로 부른다(Windows 는 `python`).
엔진은 `bin/chwp-agent`(Linux x86_64) 이고, 스크립트가 알아서 찾는다. 스크립트가 stderr 에
`chwp-agent: engine-missing {"install":"..."}` 를 내면(Windows·macOS 처럼 bin/ 이 맞지 않는 곳) 그 `install` 명령을
한 번 실행하고 원래 명령을 다시 부른다. 그 명령은 pip 으로 이 스킬 버전의 엔진 패키지(classinghwp)를 깐다.

## 빠른 시작

일을 몇 번의 실행으로 끝낸다. 여러 곳을 고칠 때도 한 요청 파일에 모아 **한 번** 실행한다.

1. **짜임 보기(한 번):** 문단 번호·글자 수·글, 표는 칸 격자와 칸 story 를 나무로 찍는다.
   ```bash
   python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_agent.py --outline 문서.hwp 2>/dev/null
   ```
   ```
   s1 section body (구역 1)
     p2 (186) "  우리 학교는 ..."        ← story 1, 문단 2, 186글자
     p3 (1) "￼"
       table 1:3:0 2x3                  ← 개체 이름 1:3:0, 2줄 3칸
         (0,1) s21 "❖ 안내 ❖"           ← 0행 1열 칸의 story 21
         (1,0 1x3) s23                  ← 1행 0열, 1줄 3칸 합침
           p0 (50) "   ▣ 목적 ..."
   ```
   글만 읽어 요약·답하기가 목적이면 대신 `python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_text.py 문서.hwp` 를 쓴다.
2. **고치기·저장·확인을 한 요청 파일로(한 번):** 아래처럼 쓰고 `--save` 로 새 파일에 저장한다.
   ```jsonl
   {"call":"apply","ops":[
     {"op":"setText","story":14,"text":"『기초탄탄 랜선야학』 참여 안내"},
     {"op":"setParagraph","story":1,"paragraph":2,"text":"  서울특별시교육청에서는 ..."},
     {"op":"setParagraph","story":23,"paragraph":3,"text":"   ▣ 대상: 보충학습이 필요한 학생 중 희망자"},
     {"op":"deleteRows","story":36,"toStory":44}]}
   {"call":"info"}
   {"call":"render","page":{"order":1},"dpi":96,"name":"p1"}
   ```
   ```bash
   python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_agent.py 문서.hwp req.jsonl --out-dir out --save out/결과.hwp 2>/dev/null
   ```
   한 줄에 JSON 하나여야 한다(위는 보기 좋게 줄을 나눈 것). 파이썬으로 `json.dumps(..., ensure_ascii=False)` 해서 쓰면 따옴표가 안전하다.
3. **확인:** 응답 줄마다 `ok` 를 보고, `out/p1.png` 를 열어 눈으로 본다. 고칠 것이 남았으면 저장한 파일을 다시 열어 2를 한 번 더 한다.
   결과 파일(hwp·hwpx·pdf·png)은 사용자가 내려받을 수 있는 곳에 둔다.

### 자주 쓰는 op

| op | 인자 | 하는 일 |
|---|---|---|
| `setParagraph` | `story`, `paragraph`, `text` | 문단 글 전체를 바꾼다. 문단·첫 글자 모양은 그대로. 표·그림이 든 문단(￼)은 거절 → `replace` |
| `setText` | `story`, `text` | 그 story(표 칸 등) 글 전체를 바꾼다. `\n` 은 문단 나눔 |
| `replace` | `range`("1:2:4-1:2:10"), `text` | 구간을 바꾼다. 위치는 `story:문단:글자`(글자는 0부터) |
| `insertText` / `delete` | `at` / `range` | 넣기·지우기. `\n` 은 문단 나눔 |
| `replaceAll` | `find`, `text` | 문서 전체에서 모두 바꾸기 |
| `format` | `range`, `set`({"bold":true,"color":"#C00000","size":1200,"align":"CENTER"}) | 글자·문단 모양 |
| `insertRows` / `deleteRows` | `story`(칸), `toStory`, `side`, `count` | 표 줄 넣기·지우기(칸도 `insertColumns`·`deleteColumns`) |
| `mergeCells` / `splitCell` | `story`, `toStory` / `rows`, `cols` | 칸 합치기·나누기 |
| `insertTable` | `at`, `rows`, `cols`, `cells` | 표 넣기 |

- 한 apply 는 트랜잭션이다. 하나라도 틀리면 아무것도 바뀌지 않고 `error.message` 가 `op N:` 으로 시작한다.
- op 의 자리는 **앞 op 까지 적용한 문서 기준**이다. 문단을 나누거나 표 줄·칸을 바꾸면 뒤 번호가 바뀌니,
  그런 op 는 맨 뒤에 두거나 **문서 뒤쪽부터** 고친다. 표 구조를 바꾼 뒤의 칸 story 는 응답의 `table` 을 본다.

## 꼭 지킬 것

- 한 번 실행이 한 세션이다. 문서 상태는 파일로 넘긴다. 원본은 덮어쓰지 말고 새 경로에 저장한다.
- 응답은 요청마다 stdout 한 줄 `{"ok":true,...}` / `{"ok":false,"error":{...}}`. stderr 는 진단이라 버려도 된다.
- apply 응답의 `fontSwaps`(라이선스로 막힌 글꼴을 편집 앞에 바꾼 것)는 **반드시 사용자에게 알린다.**
- `info` 의 `missingFonts` 는 대체 글꼴로 그린 글꼴이다(줄 나눔이 한글과 다를 수 있다고 알린다). `missingGlyphs` 는 그림에서 비는 글자다.
- 글꼴은 스크립트가 글꼴 서버(`https://ede.kr/hwp/fonts/`)에서 알아서 받는다. `--no-font-fetch` 는 붙이지 않는다.

## 글꼴 받기와 허락

글꼴 서버에는 **글꼴 이름과 글꼴 파일 경로만** 보낸다. 문서 내용·파일 이름은 보내지 않는다. 받지 못해도 글자는
실행 파일 옆 `bin/fonts/HANBatang.woff2`(함초롬바탕)로 그려진다.

- stderr 에 `chwp-agent: fonts-blocked {...}` 가 나오면(문서를 읽은 뒤 바깥 접속을 막는 환경) 함초롬바탕 결과를 먼저 주고
  묻는다. **"문서를 전송"이라고 말하지 않는다.** 예: "글꼴을 받지 못해 함초롬바탕으로 대신 그렸어요. 한글과 같은 글꼴로
  보려면 글꼴 서버(ede.kr)에서 글꼴 N개를 받아야 해요. 서버에는 글꼴 이름만 보내요. 허락하시겠어요?"
  허락하면 `python3 ${CLAUDE_SKILL_DIR}/scripts/chwp_agent.py --fetch-fonts 문서.hwp` 로 글꼴만 받고(`ok` 가 참이면) 원래 요청을 다시 부른다.
  허락하지 않으면 함초롬바탕 결과를 쓰고 `missingFonts` 가 대체되었다고 알린다.
- `chwp-agent: fonts-offline {...}` 가 나오면 인터넷이 아예 없는 환경이다. 묻지 않고, 구할 수 없는 글꼴을 함초롬바탕으로 그렸다고만 알린다.

## 더 필요할 때(reference.md 에서 그 절만)

「문서 구조」(story·개체·위치), 「읽기」(info·pages·outline·text·paragraph·table·layout·find), 「그림」(render),
「개체 놓기」(그림 띄우기·어울림), 「고치기」(모든 op·편집기 명령 op·view), 「저장」, 「작업 요령」(문서 분석, 모든 쪽 그림, 그림 놓기, 알아 둘 것).
