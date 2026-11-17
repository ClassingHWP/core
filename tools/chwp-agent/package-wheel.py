#!/usr/bin/env python3
"""chwp-agent 엔진을 담은 pip 패키지(classinghwp wheel)를 만든다.

스킬 ZIP(package-skill.py)에는 claude.ai·ChatGPT 샌드박스용 Linux x86_64 엔진만 들어 있다. Windows·macOS 에서
스킬을 쓰는 로컬 에이전트(Claude Code 등)는 이 패키지를 pip 으로 깔고, 스킬의 chwp_agent.py 가 그 엔진을 찾는다.

  python tools/chwp-agent/package-wheel.py [엔진 | 스킬.zip] [출력 폴더]

- 엔진을 주지 않으면 Windows 에서는 build/windows-msvc/tools/chwp-agent/Release/chwp-agent.exe 를 쓴다.
- 스킬 ZIP 을 주면 그 안의 Linux 정적 엔진(chwp-agent/bin/chwp-agent)을 쓴다.
- 플랫폼 태그는 엔진 파일 머리(PE·ELF·Mach-O)에서 정한다. Linux 엔진은 정적 링크여야 한다(어느 glibc·musl 에서나 돈다).
- 출력 폴더 기본: tmp/chwp-agent-skill/. 버전은 CMakeLists.txt 의 VERSION 이다.

패키지 안:
  classinghwp/__init__.py            __version__, engine_path()
  classinghwp/bin/chwp-agent(.exe)   엔진
  classinghwp/bin/msvcp140 등         (Windows) 엔진이 쓰는 VC++ 런타임. 재배포 패키지가 없는 PC 에서도 돌게 옆에 둔다
  classinghwp/bin/fonts/HANBatang.woff2   마지막 대체 글꼴(엔진이 실행 파일 옆 fonts/ 에서 읽는다)
  classinghwp-<버전>.dist-info/      METADATA, WHEEL, RECORD, licenses/
"""

import base64
import hashlib
import importlib.util
import os
import stat
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAME = "classinghwp"
LICENSES = ["LICENSE", "LICENSE.ko.md", "THIRD_PARTY_NOTICES.md"]
DEFAULT_WINDOWS = ROOT / "build" / "windows-msvc" / "tools" / "chwp-agent" / "Release" / "chwp-agent.exe"
SKILL_BINARY = "chwp-agent/bin/chwp-agent"

INIT = '''"""ClassingHWP 한글 문서 엔진(chwp-agent 실행 파일).

chwp-agent 스킬의 scripts/chwp_agent.py 가 이 패키지의 엔진을 찾아 쓴다. 엔진을 직접 부를 때는 engine_path() 를 쓴다.
"""

import os
from pathlib import Path

__version__ = "{version}"


def engine_path() -> Path:
    """이 패키지에 든 chwp-agent 실행 파일 경로."""
    return Path(__file__).resolve().parent / "bin" / ("chwp-agent.exe" if os.name == "nt" else "chwp-agent")
'''

SUMMARY = "ClassingHWP 한글(HWP·HWPX) 문서 엔진 chwp-agent. chwp-agent 스킬이 Windows·macOS 에서 쓴다."
DESCRIPTION = """# classinghwp

ClassingHWP 한글(HWP·HWPX) 문서 엔진 `chwp-agent` 실행 파일을 담은 패키지다.

AI 에이전트 스킬 `chwp-agent` 를 내 컴퓨터(Windows·macOS)에서 쓸 때 엔진으로 깐다. 스킬의 `scripts/chwp_agent.py` 가
이 패키지를 찾아 엔진을 부른다. 스킬 버전과 같은 버전을 깐다.

```
python -m pip install classinghwp==<스킬 버전> --find-links https://github.com/ClassingHWP/core/releases/expanded_assets/v<스킬 버전>
```

엔진이 없으면 스킬 스크립트가 이 명령을 알려 준다.
"""


def load_package_skill():
    spec = importlib.util.spec_from_file_location("package_skill", Path(__file__).with_name("package-skill.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def engine_bytes(source: Path) -> tuple[bytes, str]:
    """엔진 바이트와 패키지 안 파일 이름."""
    if source.suffix.lower() == ".zip":
        with zipfile.ZipFile(source) as zf:
            return zf.read(SKILL_BINARY), "chwp-agent"
    data = source.read_bytes()
    return data, ("chwp-agent.exe" if data[:2] == b"MZ" else "chwp-agent")


def elf_static(data: bytes) -> bool:
    """64비트 ELF 에 동적 링커(PT_INTERP)가 없으면 정적이다."""
    little = data[5] == 1
    order = "little" if little else "big"
    phoff = int.from_bytes(data[32:40], order)
    phentsize = int.from_bytes(data[54:56], order)
    phnum = int.from_bytes(data[56:58], order)
    for i in range(phnum):
        at = phoff + i * phentsize
        if int.from_bytes(data[at:at + 4], order) == 3:  # PT_INTERP
            return False
    return True


def platform_tag(data: bytes) -> str:
    """엔진 머리로 wheel 플랫폼 태그를 정한다."""
    if data[:2] == b"MZ":
        pe = int.from_bytes(data[0x3C:0x40], "little")
        machine = int.from_bytes(data[pe + 4:pe + 6], "little")
        tags = {0x8664: "win_amd64", 0xAA64: "win_arm64"}
        if data[pe:pe + 4] != b"PE\0\0" or machine not in tags:
            sys.exit(f"모르는 Windows 실행 파일입니다(machine 0x{machine:04X}).")
        return tags[machine]
    if data[:4] == b"\x7fELF":
        if data[4] != 2:
            sys.exit("64비트 ELF 만 받습니다.")
        machine = int.from_bytes(data[18:20], "little" if data[5] == 1 else "big")
        arch = {0x3E: "x86_64", 0xB7: "aarch64"}.get(machine)
        if arch is None:
            sys.exit(f"모르는 Linux CPU 입니다(e_machine 0x{machine:X}).")
        if not elf_static(data):
            # 동적 링크면 만든 곳의 glibc·musl 에 묶인다. manylinux·musllinux 태그를 함께 달 수 없다.
            sys.exit("Linux 엔진은 정적 링크여야 합니다(package-skill.py 의 alpine 빌드).")
        return f"manylinux_2_17_{arch}.manylinux2014_{arch}.musllinux_1_1_{arch}"
    if data[:4] == b"\xcf\xfa\xed\xfe":
        cpu = int.from_bytes(data[4:8], "little")
        tags = {0x01000007: "macosx_10_15_x86_64", 0x0100000C: "macosx_11_0_arm64"}
        if cpu not in tags:
            sys.exit(f"모르는 macOS CPU 입니다(cputype 0x{cpu:X}).")
        return tags[cpu]
    if data[:4] == b"\xca\xfe\xba\xbe":
        return "macosx_11_0_universal2"
    sys.exit("엔진 파일 형식을 알아보지 못했습니다(PE·ELF·Mach-O 가 아님).")


def pe_imports(data: bytes) -> list[str]:
    """PE32+ 실행 파일이 불러 쓰는 DLL 이름들(가져오기 표)."""
    pe = int.from_bytes(data[0x3C:0x40], "little")
    sections = int.from_bytes(data[pe + 6:pe + 8], "little")
    optional = pe + 24
    optional_size = int.from_bytes(data[pe + 20:pe + 22], "little")
    import_rva = int.from_bytes(data[optional + 120:optional + 124], "little")  # 자료 디렉터리 1번(가져오기)
    table = []
    for i in range(sections):
        at = optional + optional_size + i * 40
        virtual_size, virtual_address, raw_size, raw_pointer = (
            int.from_bytes(data[at + o:at + o + 4], "little") for o in (8, 12, 16, 20))
        table.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

    def offset(rva: int) -> int:
        for address, size, pointer in table:
            if address <= rva < address + size:
                return rva - address + pointer
        raise ValueError(f"RVA 0x{rva:X} 가 어느 구역에도 없습니다.")

    names = []
    at = offset(import_rva)
    while True:
        name_rva = int.from_bytes(data[at + 12:at + 16], "little")
        if name_rva == 0:
            return names
        start = offset(name_rva)
        names.append(data[start:data.index(b"\0", start)].decode("ascii"))
        at += 20


def vc_runtime(data: bytes, tag: str) -> list[tuple[str, bytes]]:
    """엔진이 쓰는 VC++ 런타임 DLL(msvcp140·vcruntime140 …)을 Visual Studio 재배포 폴더에서 가져온다.

    Windows 에 VC++ 재배포 패키지가 깔려 있지 않아도 엔진이 돌도록 실행 파일 옆에 둔다(앱 옆 배포).
    api-ms-win-crt-*(UCRT)는 Windows 10 부터 OS 에 들어 있어 넣지 않는다.
    """
    wanted = [name for name in pe_imports(data)
              if name.lower().startswith(("msvcp140", "vcruntime140", "concrt140"))]
    if not wanted:
        return []
    arch = {"win_amd64": "x64", "win_arm64": "arm64"}[tag]
    redist = os.environ.get("CHWP_VC_REDIST")
    if not redist:
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
            "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if not vswhere.is_file():
            sys.exit("vswhere.exe 를 찾지 못했습니다. CHWP_VC_REDIST 에 VC\\Redist\\MSVC\\<버전> 폴더를 주세요.")
        install = subprocess.run([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
                                 stdout=subprocess.PIPE, text=True).stdout.strip()
        versions = sorted((Path(install) / "VC" / "Redist" / "MSVC").glob("14.*"),
                          key=lambda p: [int(x) for x in p.name.split(".") if x.isdigit()])
        if not versions:
            sys.exit(f"VC++ 재배포 폴더가 없습니다: {install}\\VC\\Redist\\MSVC")
        redist = str(versions[-1])
    crt = sorted((Path(redist) / arch).glob("Microsoft.VC14*.CRT"))
    if not crt:
        sys.exit(f"VC++ 런타임 폴더가 없습니다: {redist}\\{arch}\\Microsoft.VC14*.CRT")
    out = []
    for name in wanted:
        path = crt[-1] / name.lower()
        if not path.is_file():
            sys.exit(f"VC++ 런타임 DLL 이 없습니다: {path}")
        out.append((name.lower(), path.read_bytes()))
    return out


def record_line(name: str, data: bytes) -> str:
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode("ascii")
    return f"{name},sha256={digest},{len(data)}"


def build(source: Path, out_dir: Path) -> Path:
    package_skill = load_package_skill()
    version = package_skill.project_version()
    engine, engine_name = engine_bytes(source)
    tag = platform_tag(engine)
    dist_info = f"{NAME}-{version}.dist-info"
    metadata = "\n".join([
        "Metadata-Version: 2.4",
        f"Name: {NAME}",
        f"Version: {version}",
        f"Summary: {SUMMARY}",
        "Home-page: https://github.com/ClassingHWP/core",
        "Requires-Python: >=3.8",
        *[f"License-File: {name}" for name in LICENSES],
        "Description-Content-Type: text/markdown",
        "",
        DESCRIPTION,
    ])
    wheel = "\n".join([
        "Wheel-Version: 1.0",
        "Generator: chwp package-wheel.py",
        "Root-Is-Purelib: false",
        *[f"Tag: py3-none-{one}" for one in tag.split(".")],
        "",
    ])
    files: list[tuple[str, bytes, bool]] = [
        (f"{NAME}/__init__.py", INIT.format(version=version).encode("utf-8"), False),
        (f"{NAME}/bin/{engine_name}", engine, True),
        *[(f"{NAME}/bin/{name}", dll, False) for name, dll in (vc_runtime(engine, tag) if tag.startswith("win") else [])],
        (f"{NAME}/bin/fonts/HANBatang.woff2", package_skill.fallback_font(), False),
        *[(f"{dist_info}/licenses/{name}", (ROOT / name).read_bytes(), False) for name in LICENSES],
        (f"{dist_info}/METADATA", metadata.encode("utf-8"), False),
        (f"{dist_info}/WHEEL", wheel.encode("utf-8"), False),
    ]
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"{NAME}-{version}-py3-none-{tag}.whl"
    record = [record_line(name, data) for name, data, _ in files] + [f"{dist_info}/RECORD,,"]
    files.append((f"{dist_info}/RECORD", ("\n".join(record) + "\n").encode("utf-8"), False))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data, executable in files:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            # pip 은 실행 권한 비트가 있는 파일을 실행 가능하게 깐다.
            info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            zf.writestr(info, data)
    return out


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if args:
        source = Path(args[0])
    elif sys.platform == "win32":
        source = DEFAULT_WINDOWS
    else:
        sys.exit("사용법: python tools/chwp-agent/package-wheel.py <엔진 | 스킬.zip> [출력 폴더]")
    if not source.is_file():
        sys.exit(f"엔진 파일이 없습니다: {source}")
    out_dir = Path(args[1]) if len(args) > 1 else ROOT / "tmp" / "chwp-agent-skill"
    out = build(source, out_dir)
    print(f"{out} ({out.stat().st_size / 1024 / 1024:.1f} MB)")


if __name__ == "__main__":
    main()
