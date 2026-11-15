#!/usr/bin/env python3
"""chwp-agent MCP 서버를 stdio 로 띄워 대화해 보는 시험(표준 라이브러리만 쓴다).

  python tools/chwp-agent/mcp_smoke.py <문서.hwp|.hwpx> [서버 명령...]

서버 명령을 주지 않으면 저장소 빌드의 chwp-agent --mcp 를 띄운다. 감싼 스크립트를 시험하려면
  python tools/chwp-agent/mcp_smoke.py 문서.hwpx python skills/chwp-agent/scripts/chwp_agent.py --mcp
처럼 준다. initialize → tools/list → hwp_open → hwp_text → hwp_render → hwp_apply → hwp_undo → hwp_save →
hwp_reference 를 차례로 부르고, 응답 모양을 확인한 결과를 한 줄씩 쓴다. 하나라도 틀리면 끝 코드 1 이다.
"""

import base64
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] if len(Path(__file__).resolve().parents) > 2 else Path.cwd()


def default_server() -> list[str]:
    exe = "chwp-agent.exe" if os.name == "nt" else "chwp-agent"
    found = sorted(ROOT.glob(f"build/*/tools/chwp-agent/**/{exe}"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not found:
        sys.exit("chwp-agent 를 먼저 빌드하세요.")
    return [str(found[0]), "--mcp"]


class Client:
    def __init__(self, command: list[str]):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.next_id = 0

    def send(self, message: dict) -> None:
        self.proc.stdin.write((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def request(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("서버가 끝났습니다.")
            reply = json.loads(line)
            if reply.get("id") == self.next_id:
                return reply

    def tool(self, name: str, arguments: dict | None = None) -> dict:
        return self.request("tools/call", {"name": name, "arguments": arguments or {}})["result"]

    def close(self) -> None:
        self.proc.stdin.close()
        self.proc.wait(timeout=30)


def payload(result: dict) -> dict:
    return json.loads(result["content"][0]["text"])


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    document = str(Path(sys.argv[1]).resolve())
    command = sys.argv[2:] or default_server()
    failures = 0

    def check(label: str, ok: bool, detail: str = "") -> None:
        nonlocal failures
        failures += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'} {label}{(' — ' + detail) if detail else ''}")

    client = Client(command)
    init = client.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                         "clientInfo": {"name": "mcp-smoke", "version": "1"}})["result"]
    check("initialize", init.get("protocolVersion") == "2025-06-18" and "tools" in init.get("capabilities", {}),
          init.get("serverInfo", {}).get("version", ""))
    client.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    tools = [t["name"] for t in client.request("tools/list")["result"]["tools"]]
    check("tools/list", {"hwp_open", "hwp_text", "hwp_render", "hwp_apply", "hwp_save", "hwp_reference"} <= set(tools),
          f"{len(tools)}개")
    before = client.tool("hwp_text")
    check("열기 전 호출은 오류", before["isError"] is True)
    opened = client.tool("hwp_open", {"path": document})
    info = payload(opened)
    check("hwp_open", not opened["isError"] and info.get("ok"), f"{info.get('format')} {info.get('pages')}쪽")
    text = payload(client.tool("hwp_text", {"maxChars": 200}))
    check("hwp_text", text.get("ok") and "stories" in text)
    render = client.tool("hwp_render", {"page": 1, "dpi": 40})
    images = [c for c in render["content"] if c["type"] == "image"]
    png = base64.b64decode(images[0]["data"]) if images else b""
    check("hwp_render 이미지", png.startswith(b"\x89PNG"), f"{len(png)} 바이트")
    applied = payload(client.tool("hwp_apply", {"ops": [{"op": "insertText", "at": "1:0:0", "text": "MCP"}]}))
    check("hwp_apply", applied.get("ok") is True, json.dumps(applied.get("ops", []), ensure_ascii=False)[:80])
    first = payload(client.tool("hwp_paragraph", {"story": 1, "paragraph": 0}))
    check("고친 글 읽기", first.get("ok") and str(first.get("text", "")).startswith("MCP"))
    undone = payload(client.tool("hwp_undo"))
    check("hwp_undo", undone.get("done") is True)
    with tempfile.TemporaryDirectory() as folder:
        target = Path(folder) / "saved.hwpx"
        saved = payload(client.tool("hwp_save", {"path": str(target)}))
        check("hwp_save", saved.get("ok") and target.is_file() and target.stat().st_size > 0, f"{saved.get('bytes')} 바이트")
    reference = client.tool("hwp_reference", {"topic": "apply"})
    check("hwp_reference", "insertText" in reference["content"][0]["text"])
    unknown = client.request("nothing/here")
    check("모르는 메서드", unknown.get("error", {}).get("code") == -32601)
    client.close()
    print("모두 통과" if failures == 0 else f"{failures}개 실패")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
