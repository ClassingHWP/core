---
name: chwp-agent
description: 한글(HWP·HWPX) 문서를 열어 글을 읽고, 찾고, 고치고(글 넣기·지우기·바꾸기·글자/문단 서식·표 넣기·표 칸 채우기), 쪽을 PNG 로 그려 확인하고, HWP 또는 HWPX 로 저장한다. 사용자가 .hwp/.hwpx 파일의 내용을 묻거나 고쳐 달라고 할 때 쓴다.
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
  core 진단 기록은 stderr 로 나오니 `2>/dev/null` 로 버려도 된다.
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
  주소를 준다(reference.md 의 글꼴). 네트워크가 막힌 곳에서는 `--no-font-fetch` 로 캐시만 쓴다.
- `{"call":"info"}` 의 `missingFonts` 가 비어 있지 않으면 그 글꼴은 대체 글꼴로 재고 그렸다. 줄 나눔·그림이 한글과
  다를 수 있다고 사용자에게 알린다.
- 라이선스로 막힌 글꼴(서버에 폭만 있는 글꼴)이 든 문서를 고치면, 편집기와 같이 편집 앞에 대역 원본 글꼴로 바꾼다.
  apply 응답의 `fontSwaps`(`{from,to}`)에 나오니 **반드시 사용자에게 어떤 글꼴이 바뀌었는지 알린다.**
- 한글 글은 `-c` 보다 요청 파일(UTF-8, JSON Lines)에 써서 넘기는 편이 따옴표 문제가 적다.

## 일하는 차례

1. **훑기**: `{"call":"info"}`, `{"call":"text"}` 로 글 전체를 읽는다. 길면 응답의 `next` 를 그대로 붙여 이어 읽는다.
   표는 문단 안 `objects` 의 `stories` 로 칸 글 묶음 번호를 알려 주고, 그 칸 글도 `text` 에 같이 나온다.
2. **자리 찾기**: `{"call":"find","text":"..."}` 로 고칠 글의 구간(`from`·`to`)과 쪽을 얻는다.
   위치는 `{story, paragraph, char}` 또는 `"story:문단:글자"` 이고, `char` 는 그 문단 글의 코드 포인트 순번이다.
3. **고치기**: `{"call":"apply","ops":[...]}`. 한 apply 는 트랜잭션이다. 하나라도 실패하면 아무것도 바뀌지 않고
   `error.message` 가 `op N:` 으로 시작한다. op 의 위치는 앞 op 까지 적용한 문서 기준이므로, 한 apply 에 여러 곳을
   고칠 때는 **문서 뒤쪽부터** 고치거나 고친 뒤 다시 `find` 한다.
4. **확인**: 바뀐 문단을 `paragraph`/`text` 로 다시 읽고, 모양이 중요하면 `render` 로 그 쪽을 그려 본다.
   찾은 결과의 `page` 를 그대로 `render` 의 `page` 로 넘기면 된다.
5. **저장**: `--save 경로` 또는 `{"call":"save","path":"..."}`. 확장자(.hwp/.hwpx)로 형식을 정한다.
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

- 글에서 U+FFFC(￼)는 글자처럼 놓인 개체(표·그림·수식) 한 글자다. 지우면 그 개체가 지워진다.
- 한글 전용 기호는 사용자 영역 글자(U+E000~U+F8FF)로 들어 있어 터미널에 안 보일 수 있다. 순번을 셀 때 빠뜨리지 않는다.
- 쪽은 물리 순서가 아니라 쪽 번호 + 구역으로 가리킨다. `AmbiguousPage` 가 오면 `candidates` 에서 골라 다시 부른다.
- 머리말·꼬리말·바탕쪽처럼 쪽에 놓이지 않는 글은 `NotEditable` 이 날 수 있다.
- 글자 서식은 빈 구간에 주면 효과가 없다. 문단 서식(정렬·줄 간격 등)은 빈 구간이어도 그 문단에 들어간다.
- 아직 없는 것: 그림·수식 넣기, 좌표로 찾기(hitTest), 쪽 일부만 그리기.
