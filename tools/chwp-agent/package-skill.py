#!/usr/bin/env python3
"""claude.ai 에 올릴 chwp-agent 스킬 ZIP 을 만든다.

Linux x86_64 정적 실행 파일을 Docker(alpine) 안에서 빌드하고, MicroTeX 수식 리소스와 함께 스킬 폴더의 bin/ 에 넣어
chwp-agent.zip 으로 묶는다. ZIP 안 맨 위는 chwp-agent/ 폴더이고, 실행 파일에는 실행 권한을 붙인다.

core 는 Skia 로 그리므로 같은 alpine 에서 GPU 없는 Skia 정적 라이브러리(build/skia/out/chwp-linux-musl)를 먼저 만든다.
한 번 만들면 다시 쓰고, --skia 를 주면 다시 만든다.

  python tools/chwp-agent/package-skill.py [출력.zip] [--skia]  (기본: tmp/chwp-agent-skill/chwp-agent.zip)

Docker 는 지금 docker context 의 엔진을 쓴다. 글꼴은 넣지 않는다(스킬이 글꼴 서버에서 받는다).
"""

import io
import os
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SKILL = ROOT / "skills" / "chwp-agent"
SOURCES = ["CMakeLists.txt", "CMakePresets.json", "cmake", "core", "tools/chwp-agent", "tools/chwp-dump"]
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
cp -r /b/_deps/microtex-src/res /pkg/microtex-res && rm -f /pkg/microtex-res/SAMPLES.tex
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


def main() -> None:
    args = [arg for arg in sys.argv[1:] if arg != "--skia"]
    out = Path(args[0]) if args else ROOT / "tmp" / "chwp-agent-skill" / "chwp-agent.zip"
    out.parent.mkdir(parents=True, exist_ok=True)
    if "--skia" in sys.argv or not (SKIA / SKIA_OUT / "libdawn_combined.a").is_file():
        build_skia()
    built = tarfile.open(fileobj=io.BytesIO(build_linux()))
    with zipfile.ZipFile(out, "w") as zf:
        for path in sorted(SKILL.rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                add(zf, "chwp-agent/" + path.relative_to(SKILL).as_posix(), path.read_bytes())
        for member in built.getmembers():
            if not member.isfile():
                continue
            name = member.name.removeprefix("./")
            add(zf, "chwp-agent/bin/" + name, built.extractfile(member).read(), executable=name == "chwp-agent")
    print(f"{out} ({out.stat().st_size / 1024 / 1024:.1f} MB)")


if __name__ == "__main__":
    main()
