#!/usr/bin/env python3
"""claude.ai 에 올릴 chwp-agent 스킬 ZIP 을 만든다.

Linux x86_64 정적 실행 파일을 Docker(alpine) 안에서 빌드하고, 스킬 폴더의 bin/ 에 넣어
chwp-agent.zip 으로 묶는다. ZIP 안 맨 위는 chwp-agent/ 폴더이고, 실행 파일에는 실행 권한을 붙인다.

같은 자리에 실행 파일 없이 글(SKILL.md·reference.md·scripts)만 묶은 chwp-agent-skill.zip 도 만든다. 실행 파일을
받지 않는 곳(텍스트 파일만 받는 스킬 업로드)에 올리는 것이다. 그 안의 scripts/release.json 이 위 ZIP 의 GitHub
릴리스 주소와 SHA-256 을 적고, chwp_agent.py 가 처음 실행할 때 그 ZIP 을 받아 실행 파일만 꺼내 쓴다.
그래서 위 ZIP 은 이름 그대로 core 릴리스(v<버전>)에 올려야 한다. 버전은 CMakeLists.txt 의 VERSION 이다.

이미 만든(올린) 실행 파일 ZIP 으로 글 스킬만 다시 묶으려면 --from <실행 파일 ZIP> 을 준다. Docker 를 쓰지 않는다.
버전을 그 ZIP 이름에서 읽지 못하면 --version 으로 준다.

core 는 Skia 로 그리므로 같은 alpine 에서 GPU 없는 Skia 정적 라이브러리(build/skia/out/chwp-linux-musl)를 먼저 만든다.
한 번 만들면 다시 쓰고, --skia 를 주면 다시 만든다.

  python tools/chwp-agent/package-skill.py [출력.zip] [--skia]  (기본: tmp/chwp-agent-skill/chwp-agent-<버전>.zip)
  python tools/chwp-agent/package-skill.py --from chwp-agent-0.3.5.zip [--version 0.3.5] [출력 폴더]

Docker 는 지금 docker context 의 엔진을 쓴다. 글꼴은 마지막 대체 글꼴 함초롬바탕(bin/fonts/HANBatang.woff2)만 넣고,
나머지는 스킬이 글꼴 서버에서 받는다.
"""

import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RELEASE_URL = "https://github.com/ClassingHWP/core/releases/download/v{version}/{name}"
BINARY_MEMBER = "chwp-agent/bin/chwp-agent"
# 마지막 대체 글꼴. 실행 파일 옆 fonts/ 에 두면 실행 파일이 시작할 때 넣는다(글꼴을 못 받는 환경에서도 글자가 보인다).
# 함초롬은 한컴 자체 라이선스로 배포 제한이 없다(docs/font-license.md). 글꼴 서버의 것과 같은 파일을 쓴다.
FALLBACK_FONT_URL = "https://ede.kr/hwp/fonts/full/HANBatang.woff2"
FALLBACK_FONT_MEMBER = "chwp-agent/bin/fonts/HANBatang.woff2"


def fallback_font() -> bytes:
    """함초롬바탕 woff2. CHWP_FALLBACK_FONT 로 파일을 줄 수 있고, 없으면 글꼴 서버에서 받아 tmp 에 둔다."""
    given = os.environ.get("CHWP_FALLBACK_FONT")
    if given:
        return Path(given).read_bytes()
    cached = ROOT / "tmp" / "chwp-agent-skill" / "HANBatang.woff2"
    if not cached.is_file():
        import urllib.request
        cached.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(FALLBACK_FONT_URL, timeout=120) as response:
            cached.write_bytes(response.read())
    data = cached.read_bytes()
    if not data.startswith(b"wOF2"):
        sys.exit(f"대체 글꼴이 woff2 가 아닙니다: {cached}")
    return data
SKILL = ROOT / "skills" / "chwp-agent"
# skills/chwp-agent/reference.md 는 MCP 서버의 hwp_reference 설명서로 실행 파일에 들어간다(core/CMakeLists.txt).
# 최상위 CMakeLists.txt 가 도구 폴더를 모두 넣으므로 구성에 필요하다(빌드하는 것은 chwp-agent 하나다).
SOURCES = ["CMakeLists.txt", "CMakePresets.json", "cmake", "core", "tools/chwp-agent", "tools/chwp-dump",
           "tools/chwp-font-grid", "skills/chwp-agent/reference.md"]
SKIA = ROOT / "build" / "skia"
SKIA_OUT = "out/chwp-linux-musl"
# core 가 읽는 Skia·Dawn 헤더와 alpine 에서 만든 정적 라이브러리.
SKIA_FOR_CORE = ["include", "src", "modules", "third_party/externals/dawn/include", SKIA_OUT + "/libskia.a",
                 SKIA_OUT + "/libdawn_combined.a", SKIA_OUT + "/gen/third_party/dawn/include"]
# Skia 를 빌드할 때 빼는 것: 다른 빌드 산출물, 저장소 기록, Dawn 의 시험과 이 설정이 쓰지 않는 외부 라이브러리.
SKIA_SKIP = {"out", "third_party/externals/dawn/test", "third_party/externals/dawn/third_party/gn"}
# Dawn(Vulkan)이 CMake 로 빌드할 때 쓰는 외부 라이브러리. swiftshader 는 있는지만 보므로 컨테이너에서 빈 폴더로 둔다.
SKIA_EXTERNALS = {"abseil-cpp", "dawn", "egl-registry", "glslang", "jinja2", "markupsafe", "opengl-registry",
                  "spirv-headers", "spirv-tools", "vulkan-headers", "vulkan-utility-libraries", "webgpu-headers"}

# 화면과 같은 Skia(Graphite Dawn + CPU 래스터). 글꼴 관리자·이미지 코덱은 뺀다.
SKIA_ARGS = (ROOT / "cmake" / "skia" / "chwp-linux-musl.gn").read_text(encoding="utf-8")

SKIA_BUILD = r"""
set -e
apk add --no-cache gn ninja ninja-build cmake clang lld python3 build-base linux-headers libx11-dev libxcb-dev >/dev/null 2>&1
# alpine 의 ninja 는 samurai 라 Dawn 빌드가 쓰는 `ninja -t inputs` 가 없다. 진짜 ninja 를 앞에 둔다.
export PATH=/usr/lib/ninja-build/bin:$PATH
mkdir /skia && cd /skia && tar xf -
mkdir -p third_party/externals/swiftshader
mkdir -p /o && printf '%s' "$SKIA_ARGS" > /o/args.gn
gn gen /o >/g.log 2>&1 || { tail -30 /g.log >&2; exit 1; }
ninja -C /o skia >/n.log 2>&1 || { grep -E 'error|FAILED' /n.log | head -40 >&2; exit 1; }
cd /o && tar cf - libskia.a libdawn_combined.a args.gn gen/third_party/dawn/include
"""

BUILD = r"""
set -e
apk add --no-cache build-base cmake ninja linux-headers >/dev/null 2>&1
mkdir /src && cd /src && tar xf -
cmake -S . -B /b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHWP_BUILD_TESTS=OFF -DCMAKE_EXE_LINKER_FLAGS=-static \
  -DCHWP_SKIA_OUT=chwp-linux-musl -DCHWP_SKIA_DAWN=ON >/b.log 2>&1 \
  || { tail -30 /b.log >&2; exit 1; }
cmake --build /b --target chwp-agent >>/b.log 2>&1 || { grep -E 'error|Error' /b.log | head -30 >&2; exit 1; }
strip /b/tools/chwp-agent/chwp-agent
mkdir -p /pkg && cp /b/tools/chwp-agent/chwp-agent /pkg/
cd /pkg && tar cf - .
"""


def build_skia() -> None:
    """build/skia 로 alpine 정적 Skia·Dawn 을 만들어 build/skia/out/chwp-linux-musl 에 둔다. 처음 한 번은 오래 걸린다."""
    def keep(info: tarfile.TarInfo) -> tarfile.TarInfo | None:
        name = info.name.removeprefix("./")
        if ".git" in name.split("/") or any(name == skip or name.startswith(skip + "/") for skip in SKIA_SKIP):
            return None
        if name.startswith("third_party/externals/") and name.count("/") >= 2:
            return info if name.split("/")[2] in SKIA_EXTERNALS else None
        return info

    source = io.BytesIO()
    with tarfile.open(fileobj=source, mode="w") as tar:
        tar.add(SKIA, arcname=".", filter=keep)
    print("Skia 를 alpine 에서 빌드합니다...", file=sys.stderr)
    run = subprocess.run(["docker", "run", "--rm", "-i", "-e", "SKIA_ARGS", "alpine:3.20", "sh", "-c", SKIA_BUILD],
                         input=source.getvalue(), stdout=subprocess.PIPE, env={**os.environ, "SKIA_ARGS": SKIA_ARGS})
    if run.returncode != 0:
        sys.exit("Skia 빌드에 실패했습니다.")
    out = SKIA / SKIA_OUT
    out.mkdir(parents=True, exist_ok=True)
    tarfile.open(fileobj=io.BytesIO(run.stdout)).extractall(out, filter="data")


def build_linux() -> bytes:
    source = io.BytesIO()
    with tarfile.open(fileobj=source, mode="w") as tar:
        for name in SOURCES:
            # 공개 저장소에는 tools/chwp-font-grid 가 없다(최상위 CMakeLists.txt 도 그 줄이 없다).
            if (ROOT / name).exists():
                tar.add(ROOT / name, arcname=name)
        for name in SKIA_FOR_CORE:
            tar.add(SKIA / name, arcname="build/skia/" + name)
    run = subprocess.run(["docker", "run", "--rm", "-i", "alpine:3.20", "sh", "-c", BUILD],
                         input=source.getvalue(), stdout=subprocess.PIPE)
    if run.returncode != 0:
        sys.exit("Linux 빌드에 실패했습니다.")
    return run.stdout


def add(zf: zipfile.ZipFile, name: str, data: bytes, executable: bool = False) -> None:
    info = zipfile.ZipInfo(name)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (0o755 if executable else 0o644) << 16
    zf.writestr(info, data)


def project_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    found = re.search(r"project\([^)]*VERSION\s+([0-9.]+)", text)
    if not found:
        sys.exit("CMakeLists.txt 에서 VERSION 을 찾지 못했습니다.")
    return found.group(1)


def add_skill_text(zf: zipfile.ZipFile) -> None:
    for path in sorted(SKILL.rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts and path.name != "release.json":
            add(zf, "chwp-agent/" + path.relative_to(SKILL).as_posix(), path.read_bytes())


def write_text_skill(full: Path, version: str, out: Path) -> None:
    """실행 파일 없이 글만 묶은 스킬. release.json 이 full(릴리스에 올린 이름 그대로)을 가리킨다."""
    data = full.read_bytes()
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        names = set(archive.namelist())
        if BINARY_MEMBER not in names:
            sys.exit(f"{full.name} 에 {BINARY_MEMBER} 가 없습니다.")
    # 실행 파일 옆에 함께 꺼낼 파일(ZIP 안 경로 → 실행 파일 폴더 기준 경로). 그 ZIP 에 있는 것만 적는다(옛 릴리스에는 없다).
    extras = {member: relative for member, relative in {FALLBACK_FONT_MEMBER: "fonts/HANBatang.woff2"}.items()
              if member in names}
    release = {
        "version": version,
        "binaries": {
            "linux-x86_64": {
                "url": RELEASE_URL.format(version=version, name=full.name),
                "sha256": hashlib.sha256(data).hexdigest(),
                "member": BINARY_MEMBER,
                "files": extras,
            }
        },
    }
    with zipfile.ZipFile(out, "w") as zf:
        add_skill_text(zf)
        add(zf, "chwp-agent/scripts/release.json", (json.dumps(release, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(f"{out} ({out.stat().st_size / 1024:.0f} KB, 실행 파일은 {release['binaries']['linux-x86_64']['url']})")


def text_skill_path(full: Path) -> Path:
    return full.with_name(full.name.replace("chwp-agent", "chwp-agent-skill", 1))


def main() -> None:
    argv = sys.argv[1:]
    take = lambda flag: argv[argv.index(flag) + 1] if flag in argv else None  # noqa: E731
    given_from = take("--from")
    given_version = take("--version")
    args = [a for i, a in enumerate(argv)
            if not a.startswith("--") and (i == 0 or argv[i - 1] not in ("--from", "--version"))]
    if given_from:
        full = Path(given_from)
        found = re.search(r"chwp-agent-([0-9.]+)\.zip$", full.name)
        version = given_version or (found.group(1) if found else None)
        if not version:
            sys.exit("--from ZIP 이름에서 버전을 읽지 못했습니다. --version 을 주세요.")
        folder = Path(args[0]) if args else full.parent
        folder.mkdir(parents=True, exist_ok=True)
        write_text_skill(full, version, folder / text_skill_path(full).name)
        return
    version = given_version or project_version()
    out = Path(args[0]) if args else ROOT / "tmp" / "chwp-agent-skill" / f"chwp-agent-{version}.zip"
    out.parent.mkdir(parents=True, exist_ok=True)
    if "--skia" in sys.argv or not (SKIA / SKIA_OUT / "libdawn_combined.a").is_file():
        build_skia()
    built = tarfile.open(fileobj=io.BytesIO(build_linux()))
    with zipfile.ZipFile(out, "w") as zf:
        add_skill_text(zf)
        for member in built.getmembers():
            if not member.isfile():
                continue
            name = member.name.removeprefix("./")
            add(zf, "chwp-agent/bin/" + name, built.extractfile(member).read(), executable=name == "chwp-agent")
        add(zf, FALLBACK_FONT_MEMBER, fallback_font())
    print(f"{out} ({out.stat().st_size / 1024 / 1024:.1f} MB)")
    write_text_skill(out, version, text_skill_path(out))


if __name__ == "__main__":
    main()
