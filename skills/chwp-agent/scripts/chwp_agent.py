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
  - --no-font-fetch 를 주면 받지 않고 캐시(와 파일 경로로 준 목록)만 쓴다(네트워크가 막힌 곳).
  - 받지 못한 파일은 캐시에 "파일.missing" 표시를 남겨 같은 실행에서 다시 묻지 않는다.

사용법은 실행 파일과 같다:
  python chwp_agent.py <문서.hwp|.hwpx> [요청.jsonl | -] [-c JSON]... [--save 경로] [--out-dir 폴더]
"""

import json
import os
import subprocess
import sys
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


def download(url: str, target: Path) -> bool:
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read()
    except Exception as error:  # 네트워크가 막혔거나 서버에 없는 파일
        sys.stderr.write(f"글꼴을 받지 못했습니다: {url} ({error})\n")
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return True


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
        return False
    except Exception as error:  # 네트워크가 막혔다
        sys.stderr.write(f"글꼴 목록을 받지 못했습니다: {url} ({error})\n")
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
            if not download(FONT_BASE + folder + "/" + urllib.parse.quote(file), target):
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
    document = first_positional(args)
    # 요청을 표준 입력(-)으로 받으면 글꼴을 묻는 실행과 진짜 실행에 같은 요청을 주려고 먼저 다 읽는다.
    stdin = sys.stdin.buffer.read() if "-" in args else None
    if document:
        prepare_lists(fetch)
    if document and fetch:
        fetch_fonts(binary, args, stdin)
    if document and (FONT_CACHE / "index.json").is_file():
        args += ["--font-dir", str(FONT_CACHE)]
    return subprocess.run([str(binary), *args], input=stdin).returncode


if __name__ == "__main__":
    sys.exit(main())
