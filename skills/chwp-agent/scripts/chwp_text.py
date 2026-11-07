#!/usr/bin/env python3
"""한글 문서의 글 전체를 읽는 차례대로 평문으로 쓴다(요약·질문 답하기용).

  python chwp_text.py <문서.hwp|.hwpx> [--master] [chwp_agent.py 옵션...]

chwp_agent.py 로 {"call":"pages"}, {"call":"text"} 를 한 번 불러(글자 수 한도 없음) 그 응답을 풀어 쓴다.
- 본문이 새 쪽으로 넘어가는 자리에 "=== 03_2쪽 ===" 경계를 넣는다. 03 은 문서 안 물리 순서(order),
  2쪽 은 쪽 번호 글이다. SKILL.md 의 쪽 그림 파일 이름과 같다.
- 표·글상자 글은 본문에서 그 개체가 놓인 자리 바로 뒤에 넣는다. 표는 행마다 "| 칸 | 칸 |" 한 줄이다.
- 머리말·꼬리말·각주처럼 본문에서 닿지 않는 글은 끝에 [owner] 머리를 달아 쓴다.
- 바탕쪽 글(쪽 번호·반복 머리글)은 뺀다. --master 를 주면 넣는다.
글을 고칠 자리(story:문단:글자)가 필요하면 이 출력 말고 chwp_agent.py 의 text·find 응답을 쓴다.
"""

import json
import subprocess
import sys
from pathlib import Path

OBJECT_MARK = "￼"


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    master = "--master" in args
    args = [a for a in args if a != "--master"]
    agent = Path(__file__).with_name("chwp_agent.py")
    pages_request = json.dumps({"call": "pages"})
    text_request = json.dumps({"call": "text", "maxChars": 1000000000})
    run = subprocess.run([sys.executable, str(agent), *args, "-c", pages_request, "-c", text_request],
                         stdout=subprocess.PIPE)
    lines = [line for line in run.stdout.decode("utf-8").splitlines() if line.strip()]
    if len(lines) < 2:
        print("chwp_agent.py 가 응답하지 않았습니다.", file=sys.stderr)
        return 2
    pages_reply, reply = json.loads(lines[-2]), json.loads(lines[-1])
    for r in (pages_reply, reply):
        if not r.get("ok"):
            print(json.dumps(r, ensure_ascii=False), file=sys.stderr)
            return 1

    labels = {p["order"]: p["label"] for p in pages_reply["pages"]}
    stories = {s["story"]: s for s in reply["stories"]}
    seen = set()
    out = []
    shown_page = [0]

    def page_mark(para):
        order = para.get("pageOrder", 0)
        if order > shown_page[0]:
            shown_page[0] = order
            if out:
                out.append("")
            out.append(f"=== {order:02d}_{labels.get(order, '?')}쪽 ===")

    def write_story(number, indent):
        story = stories.get(number)
        if story is None or number in seen:
            return
        seen.add(number)
        for para in story["text"]:
            if story["kind"] == "section":
                page_mark(para)
            text = para["text"].replace(OBJECT_MARK, "").strip()
            if text:
                for piece in text.split("\n"):
                    out.append(indent + piece)
            for obj in para.get("objects", []):
                if obj.get("stories"):
                    write_object(obj, indent)

    def cell_text(number):
        story = stories.get(number)
        seen.add(number)
        if story is None:
            return ""
        parts = [p["text"].replace(OBJECT_MARK, "").strip() for p in story["text"]]
        return " / ".join(p.replace("\n", " ") for p in parts if p)

    def write_object(obj, indent):
        cells = [stories[n] for n in obj["stories"] if n in stories and "row" in stories[n]]
        if obj["kind"] == "table" and cells:
            rows = {}
            for cell in cells:
                rows.setdefault(cell["row"], []).append(cell)
            for row in sorted(rows):
                texts = [cell_text(c["story"]) for c in sorted(rows[row], key=lambda c: c["col"])]
                out.append(indent + "| " + " | ".join(texts) + " |")
            # 칸 안의 표·글상자는 그 표 뒤에 이어 쓴다.
            for cell in cells:
                for para in cell["text"]:
                    for inner in para.get("objects", []):
                        if inner.get("stories"):
                            write_object(inner, indent + "  ")
            return
        for number in obj["stories"]:
            write_story(number, indent + "  ")

    for number, story in stories.items():
        if story["kind"] == "section":
            write_story(number, "")
    for number, story in stories.items():
        if number in seen or (story["kind"] == "masterPage" and not master):
            continue
        out.append("")
        out.append(f"[{story['owner']}]")
        write_story(number, "")

    sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
