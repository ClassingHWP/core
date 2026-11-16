#!/usr/bin/env python3
"""chwp-agent 실행 파일을 찾고, 문서에 필요한 글꼴을 글꼴 서버에서 받아 둔 뒤 인자를 그대로 넘긴다.

무엇을 받을지, 무엇을 할지는 모두 실행 파일(core)이 정한다. 이 스크립트는 실행 파일 자리를 찾고,
core 가 달라고 한 글꼴 파일을 받아 캐시 폴더에 넣는 일만 한다.

실행 파일을 찾는 차례:
  1. 환경 변수 CHWP_AGENT_BIN
  2. 스킬 폴더의 bin/chwp-agent(.exe)  (묶어서 배포할 때)
  3. 저장소 build/*/tools/chwp-agent/ 아래에서 가장 최근에 만든 것

글꼴:
  - 서버: 환경 변수 CHWP_FONT_BASE, 없으면 https://ede.kr/hwp/fonts/ (웹 뷰어와 같은 글꼴 서버)
  - 캐시: 환경 변수 CHWP_FONT_CACHE, 없으면 ~/.cache/chwp-agent/fonts
  - 목록(index.json, license-swap.json, replace.json)은 실행마다 서버에 바뀌었는지 묻고(ETag), 바뀌었으면 새로 받는다.
    받지 못하면 캐시에 있는 목록을 쓴다.
  - 고친 목록을 쓰려면 CHWP_FONT_INDEX, CHWP_FONT_LICENSE_SWAP, CHWP_FONT_REPLACE 에 파일 경로나 주소를 준다.
    글꼴 파일은 그대로 CHWP_FONT_BASE 에서 받는다.
  - --no-font-fetch 를 주면 받지 않고 캐시(와 파일 경로로 준 목록)만 쓴다. 받기가 실패해도 캐시로 이어 가므로
    에이전트는 이 옵션을 붙이지 않는다(미리 붙이면 받을 수 있던 글꼴이 빠진다). 시험·고장 찾기용이다.
  - 받지 못한 파일은 캐시에 "파일.missing" 표시를 남겨 같은 실행에서 다시 묻지 않는다.
  - 글꼴 서버에 보내는 것은 목록 이름과 글꼴 파일 경로(예: full/HANBatang.woff2)뿐이다. 문서 내용·파일 이름은 보내지 않는다.
  - 네트워크가 막혀(또는 환경이 바깥 접속을 막아) 받지 못하면 stderr 에 아래 한 줄을 남긴다. 에이전트는 이 줄을 보고
    사용자에게 글꼴 받기를 허락받은 뒤 --fetch-fonts 로 다시 받는다(SKILL.md 의 글꼴 받기와 허락).
      chwp-agent: fonts-blocked {"server":..., "lists":[...], "fonts":[...], "count":N, "sent":..., "retry":...}
  - --fetch-fonts 는 문서를 고치거나 그리지 않고 그 문서에 필요한 글꼴만 받는다. 결과를 stdout 에 JSON 한 줄로 쓴다.
      python chwp_agent.py --fetch-fonts <문서.hwp|.hwpx>

사용법은 실행 파일과 같다:
  python chwp_agent.py <문서.hwp|.hwpx> [요청.jsonl | -] [-c JSON]... [--save 경로] [--out-dir 폴더]

문서 한눈에 보기(outline 호출의 글을 그대로 찍는다. 문단 번호·글자 수·글, 표 칸 격자와 칸 story):
  python chwp_agent.py --outline <문서.hwp|.hwpx>

MCP 서버(stdio)로 쓰려면:
  python chwp_agent.py --mcp
실행 파일의 MCP 모드(chwp-agent --mcp --font-relay)를 띄워 클라이언트와 줄을 그대로 잇는다. 실행 파일이 캐시에
없는 글꼴을 chwp/fetchFonts 요청으로 달라고 하면 이 스크립트가 글꼴 서버에서 받아 캐시에 넣고 답한다.
그 요청과 답은 클라이언트에 보내지 않는다. 무엇을 할지(도구·판단)는 모두 실행 파일(core)이 정한다.
"""

import json
import os
import socket
import subprocess
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

EXE = "chwp-agent.exe" if os.name == "nt" else "chwp-agent"
FONT_BASE = os.environ.get("CHWP_FONT_BASE", "https://ede.kr/hwp/fonts/").rstrip("/") + "/"
FONT_CACHE = Path(os.environ.get("CHWP_FONT_CACHE", Path.home() / ".cache" / "chwp-agent" / "fonts"))
# 목록 파일과 그 목록을 대신할 파일 경로·주소를 주는 환경 변수.
CATALOG = {"index.json": "CHWP_FONT_INDEX", "license-swap.json": "CHWP_FONT_LICENSE_SWAP", "replace.json": "CHWP_FONT_REPLACE"}


def find_binary() -> Path | None:
    env = os.environ.get("CHWP_AGENT_BIN")
    if env and Path(env).is_file():
        return Path(env)
    skill = Path(__file__).resolve().parent.parent
    bundled = skill / "bin" / EXE
    if bundled.is_file():
        # ZIP 으로 올린 스킬은 풀 때 실행 권한이 빠질 수 있다.
        if os.name != "nt" and not os.access(bundled, os.X_OK):
            bundled.chmod(bundled.stat().st_mode | 0o755)
        return bundled
    for root in [skill, *skill.parents]:
        build = root / "build"
        if not build.is_dir():
            continue
        found = sorted(build.glob(f"*/tools/chwp-agent/**/{EXE}"), key=lambda p: p.stat().st_mtime, reverse=True)
        if found:
            return found[0]
    return None



SENT = "글꼴 서버에는 글꼴 목록 이름과 글꼴 파일 경로만 요청합니다. 문서 내용과 파일 이름은 보내지 않습니다."
# 이 실행에서 글꼴을 받은 결과. 막힌 것(네트워크·바깥 접속 차단)과 서버에 없는 것을 나눈다.
FETCH = {"fetched": [], "absent": [], "blocked": [], "lists_blocked": [], "errors": [], "unresolved": 0, "refused": 0}


def blocked_error(error: Exception) -> bool:
    """서버가 없다고 답한 것(404·410)이 아니면 막힌 것으로 본다(접속 실패, 시간 초과, 프록시·방화벽 거절)."""
    return not (isinstance(error, urllib.error.HTTPError) and error.code in (404, 410))


def note_blocked(error: Exception) -> None:
    """막힌 까닭을 센다. 서버 주소조차 찾지 못하면(DNS) 인터넷이 아예 없는 환경으로 본다."""
    if isinstance(getattr(error, "reason", None), socket.gaierror) or isinstance(error, socket.gaierror):
        FETCH["unresolved"] += 1
    else:
        FETCH["refused"] += 1
    if len(FETCH["errors"]) < 3:
        FETCH["errors"].append(str(error))


def download(url: str, target: Path, path: str = "") -> bool:
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read()
    except Exception as error:  # 네트워크가 막혔거나 서버에 없는 파일
        sys.stderr.write(f"글꼴을 받지 못했습니다: {url} ({error})\n")
        if blocked_error(error):
            FETCH["blocked"].append(path or url)
            note_blocked(error)
        else:
            FETCH["absent"].append(path or url)
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    FETCH["fetched"].append(path or url)
    return True


def fonts_blocked() -> bool:
    return bool(FETCH["blocked"]) or (bool(FETCH["lists_blocked"]) and not (FONT_CACHE / "index.json").is_file())


def report_blocked() -> None:
    """글꼴 받기가 막혔으면 에이전트가 알아볼 한 줄을 stderr 에 남긴다."""
    if not fonts_blocked():
        return
    if FETCH["unresolved"] and not FETCH["refused"]:
        # 서버 주소를 하나도 찾지 못했다. 인터넷이 없는 환경이라 허락을 받아도 받을 수 없다.
        offline = {"server": FONT_BASE, "count": len(FETCH["blocked"]), "errors": FETCH["errors"],
                   "fallback": "함초롬바탕", "ask": False}
        sys.stderr.write("chwp-agent: fonts-offline " + json.dumps(offline, ensure_ascii=False) + "\n")
        return
    notice = {
        "server": FONT_BASE,
        "lists": FETCH["lists_blocked"],
        "fonts": FETCH["blocked"][:20],
        "count": len(FETCH["blocked"]),
        "errors": FETCH["errors"],
        "sent": SENT,
        "retry": "사용자에게 글꼴 받기를 허락받은 뒤 python chwp_agent.py --fetch-fonts <문서> 를 실행하고 다시 부르세요.",
    }
    sys.stderr.write("chwp-agent: fonts-blocked " + json.dumps(notice, ensure_ascii=False) + "\n")


def json_object(data: bytes) -> bool:
    try:
        return isinstance(json.loads(data.decode("utf-8")), dict)
    except ValueError:
        return False


def fetch_list(url: str, target: Path) -> bool:
    """목록 하나를 받는다. 캐시와 같으면(304) 그대로 둔다. 받았거나 그대로 써도 되면 True 다."""
    tag = target.with_name(target.name + ".etag")
    request = urllib.request.Request(url)
    if target.is_file() and tag.is_file():
        request.add_header("If-None-Match", tag.read_text(encoding="utf-8").strip())
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read()
            etag = response.headers.get("ETag", "")
    except urllib.error.HTTPError as error:
        if error.code == 304:
            return True
        sys.stderr.write(f"글꼴 목록을 받지 못했습니다: {url} ({error})\n")
        if blocked_error(error):
            FETCH["lists_blocked"].append(target.name)
            note_blocked(error)
        return False
    except Exception as error:  # 네트워크가 막혔다
        sys.stderr.write(f"글꼴 목록을 받지 못했습니다: {url} ({error})\n")
        FETCH["lists_blocked"].append(target.name)
        note_blocked(error)
        return False
    if not json_object(data):
        sys.stderr.write(f"글꼴 목록이 JSON 객체가 아닙니다: {url}\n")
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    if etag:
        tag.write_text(etag, encoding="utf-8")
    else:
        tag.unlink(missing_ok=True)
    return True


def prepare_lists(fetch: bool) -> None:
    """글꼴 목록 셋을 캐시에 맞춘다. 환경 변수로 준 목록이 먼저고, 없으면 글꼴 서버 것이다."""
    for name, env in CATALOG.items():
        target = FONT_CACHE / name
        given = os.environ.get(env, "").strip()
        if given and not given.startswith(("http://", "https://")):
            data = Path(given).read_bytes() if Path(given).is_file() else b""
            if not json_object(data):
                sys.stderr.write(f"{env} 의 글꼴 목록을 읽지 못했습니다: {given}\n")
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.with_name(name + ".etag").unlink(missing_ok=True)
            continue
        if fetch:
            fetch_list(given or FONT_BASE + name, target)
        # 목록이 없으면 빈 목록으로 둔다. index.json 이 없으면 받을 글꼴도 없다(캐시와 플랫폼 글꼴만 쓴다).
        if not target.is_file() and name != "index.json":
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("{}", encoding="utf-8")


def fetch_fonts(binary: Path, args: list[str], stdin: bytes | None) -> None:
    """core 가 달라는 글꼴 파일이 없어질 때까지 받아 캐시에 넣는다.

    요청까지 그대로 넘겨 끝까지 해 보게 한다(파일은 쓰지 않는다). 편집이 새 글꼴을 쓰게 할 수 있어서다.
    """
    # 앞 실행에서 못 받은 표시는 지우고 다시 받아 본다. 표시는 이 실행 안에서 되묻지 않으려고 남긴다.
    if FONT_CACHE.is_dir():
        for mark in FONT_CACHE.rglob("*.missing"):
            mark.unlink(missing_ok=True)
    if not (FONT_CACHE / "index.json").is_file():
        return  # 목록이 없으면 받을 글꼴도 없다. 캐시와 플랫폼 글꼴만 쓴다
    # 받은 글꼴에 없는 글자의 대체 글꼴을 core 가 더 달라고 할 수 있어, 달라는 것이 없을 때까지 되묻는다.
    # 몇 번 되물을지는 core 가 센다. 실행마다 새로 세므로 여기 횟수는 고장 났을 때 멈추는 안전장치일 뿐이다.
    for _ in range(20):
        run = subprocess.run([str(binary), *args, "--font-dir", str(FONT_CACHE), "--font-plan"],
                             input=stdin, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            need = json.loads(run.stdout.decode("utf-8").splitlines()[-1]).get("need", [])
        except (IndexError, ValueError):
            return
        if not need:
            return
        for path in need:
            folder, _, file = path.partition("/")
            target = FONT_CACHE / folder / file
            if not download(FONT_BASE + folder + "/" + urllib.parse.quote(file), target, path):
                missing = target.with_name(target.name + ".missing")
                missing.parent.mkdir(parents=True, exist_ok=True)
                missing.touch()


def first_positional(args: list[str]) -> str | None:
    """실행 파일과 같은 규칙으로 문서 경로를 찾는다. 값을 받는 옵션의 값은 건너뛴다."""
    takes_value = {"-c", "--call", "--save", "--out-dir", "--font-dir"}
    skip = False
    for arg in args:
        if skip:
            skip = False
        elif arg in takes_value:
            skip = True
        elif arg != "-" and not arg.startswith("--"):
            return arg
    return None


def fetch_requested(paths: list[str]) -> None:
    """실행 파일이 달라는 글꼴을 받아 캐시에 넣는다.

    core 는 한 문서에서 같은 파일을 한 번만 달라고 한다. 그래서 못 받은 표시가 있어도 다시 받아 본다.
    허락을 받거나 네트워크가 열린 뒤 문서를 다시 열면(hwp_open) 그때 받는다.
    """
    for path in paths:
        folder, _, file = path.partition("/")
        target = FONT_CACHE / folder / file
        missing = target.with_name(target.name + ".missing")
        if target.is_file():
            continue
        if not download(FONT_BASE + folder + "/" + urllib.parse.quote(file), target, path):
            missing.parent.mkdir(parents=True, exist_ok=True)
            missing.touch()


def run_mcp(binary: Path, fetch: bool) -> int:
    """클라이언트 ↔ 실행 파일(MCP 모드) 사이에서 줄을 잇고, 글꼴 요청만 가로채 받아 준다."""
    if FONT_CACHE.is_dir():
        for mark in FONT_CACHE.rglob("*.missing"):
            mark.unlink(missing_ok=True)
    prepare_lists(fetch)
    report_blocked()
    # 목록이 아직 없어도 캐시 폴더를 넘긴다. 실행 파일은 문서를 열 때마다 목록을 다시 읽는다.
    FONT_CACHE.mkdir(parents=True, exist_ok=True)
    command = [str(binary), "--mcp", "--font-dir", str(FONT_CACHE)]
    if fetch:
        command.append("--font-relay")
    server = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    lock = threading.Lock()

    def to_server(data: bytes) -> None:
        with lock:
            server.stdin.write(data)
            server.stdin.flush()

    def opens_document(line: bytes) -> bool:
        try:
            message = json.loads(line)
        except ValueError:
            return False
        return (isinstance(message, dict) and message.get("method") == "tools/call"
                and isinstance(message.get("params"), dict) and message["params"].get("name") == "hwp_open")

    def pump_client() -> None:
        try:
            for line in sys.stdin.buffer:
                # 문서를 열기 전에 글꼴 목록을 다시 맞춘다. 앞에서 막혔다가 허락받은 뒤 다시 여는 경우를 받는다.
                if fetch and opens_document(line):
                    FETCH["lists_blocked"].clear()
                    prepare_lists(fetch)
                    if FETCH["lists_blocked"]:
                        report_blocked()
                to_server(line)
        except (BrokenPipeError, OSError):
            pass
        finally:
            try:
                server.stdin.close()
            except OSError:
                pass

    threading.Thread(target=pump_client, daemon=True).start()
    for line in server.stdout:
        try:
            message = json.loads(line)
        except ValueError:
            message = None
        if isinstance(message, dict) and message.get("method") == "chwp/fetchFonts":
            before = len(FETCH["blocked"])
            fetch_requested(message.get("params", {}).get("paths", []))
            blocked = FETCH["blocked"][before:]
            if blocked:
                report_blocked()
            answer = {"jsonrpc": "2.0", "id": message.get("id"), "result": {"blocked": blocked}}
            to_server((json.dumps(answer) + "\n").encode("utf-8"))
            continue
        sys.stdout.buffer.write(line)
        sys.stdout.buffer.flush()
    return server.wait()


def fetch_only(args: list[str], binary: Path) -> int:
    """문서에 필요한 글꼴만 받는다. 문서를 고치거나 그리지 않는다. 결과는 stdout 에 JSON 한 줄이다."""
    document = first_positional(args)
    if not document:
        sys.stderr.write("사용법: python chwp_agent.py --fetch-fonts <문서.hwp|.hwpx>\n")
        return 2
    prepare_lists(True)
    fetch_fonts(binary, [document], None)
    blocked = fonts_blocked()
    result = {
        "ok": not blocked,
        "server": FONT_BASE,
        "fetched": len(FETCH["fetched"]),
        "notOnServer": FETCH["absent"],
        "blocked": FETCH["blocked"],
        "listsBlocked": FETCH["lists_blocked"],
        "errors": FETCH["errors"],
        "sent": SENT,
    }
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0 if not blocked else 1


def main() -> int:
    binary = find_binary()
    if binary is None:
        sys.stderr.write(
            "chwp-agent 실행 파일을 찾지 못했습니다. 저장소에서 다음으로 빌드하거나 CHWP_AGENT_BIN 을 주세요:\n"
            "  cmake --build build/windows-msvc --config Release --target chwp-agent\n"
        )
        return 2
    args = sys.argv[1:]
    fetch = "--no-font-fetch" not in args
    args = [a for a in args if a != "--no-font-fetch"]
    if "--mcp" in args:
        return run_mcp(binary, fetch)
    if "--outline" in args:
        rest = [a for a in args if a != "--outline"]
        document = first_positional(rest)
        if not document:
            sys.stderr.write("사용법: python chwp_agent.py --outline <문서.hwp|.hwpx>\n")
            return 2
        run = subprocess.run([str(binary), document, "-c", '{"call":"outline"}'], stdout=subprocess.PIPE)
        try:
            reply = json.loads(run.stdout.decode("utf-8").splitlines()[-1])
        except (IndexError, ValueError):
            sys.stdout.buffer.write(run.stdout)
            return run.returncode or 1
        if not reply.get("ok"):
            print(json.dumps(reply, ensure_ascii=False))
            return 1
        sys.stdout.write(reply["outline"])
        return 0
    if "--fetch-fonts" in args:
        return fetch_only([a for a in args if a != "--fetch-fonts"], binary)
    document = first_positional(args)
    # 요청을 표준 입력(-)으로 받으면 글꼴을 묻는 실행과 진짜 실행에 같은 요청을 주려고 먼저 다 읽는다.
    stdin = sys.stdin.buffer.read() if "-" in args else None
    if document:
        prepare_lists(fetch)
    if document and fetch:
        fetch_fonts(binary, args, stdin)
        report_blocked()
    if document and (FONT_CACHE / "index.json").is_file():
        args += ["--font-dir", str(FONT_CACHE)]
    return subprocess.run([str(binary), *args], input=stdin).returncode


if __name__ == "__main__":
    sys.exit(main())
