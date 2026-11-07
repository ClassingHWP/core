# ClassingHWP

한글 문서(`.hwp`, `.hwpx`)를 브라우저에서 보고 고치는 편집기 ClassingHWP 의 문서 엔진이다. C++ 로 쓰고 WebAssembly 로 빌드해 웹 앱([ClassingHWP/web](https://github.com/ClassingHWP/web))에서 쓴다.

The C++ document engine of ClassingHWP, a browser-based viewer and editor for Hangul word processor documents (`.hwp`, `.hwpx`). It compiles to WebAssembly for the web app at [ClassingHWP/web](https://github.com/ClassingHWP/web). Source-available under Apache-2.0 with the Commons Clause.

> 아직 개발 중이다. 문서마다 한글과 다르게 보이는 곳이 있을 수 있다.

## 바로 써 보기

- 편집기: https://ede.kr/hwp/editor
- 보기 전용: [https://ede.kr/hwp/viewer](https://ede.kr/hwp/viewer?file=/hwp/samples/math-curriculum.hwp)

두 화면의 쓰는 법과 다른 페이지에 넣는 법은 [ClassingHWP/web](https://github.com/ClassingHWP/web#보기-전용과-편집기) 에 있다.

## 구성

| 폴더 | 내용 |
|---|---|
| `core/` | 문서 엔진. HWP·HWPX 읽기·쓰기, 조판, 그리기(Skia) |
| `bindings/wasm/` | 엔진을 웹에서 부르는 WebAssembly 바인딩 |
| `tools/chwp-agent/` | 에이전트 API 를 부르는 명령줄 도구(JSON 한 줄씩) |
| `tools/chwp-dump/` | 문서 구조를 텍스트로 보는 명령줄 도구 |
| `skills/chwp-agent/` | AI 에이전트가 `chwp-agent` 로 한글 문서를 읽고 고치게 하는 스킬 |
| `docs/` | 문서 구조 분석과 설계 메모 |
| `cmake/` | 빌드 설정과 의존성, Skia GN 설정(`cmake/skia/`) |

## 빌드

### 준비물

- CMake 3.24 이상, Ninja
- C++20 컴파일러: Windows 는 Visual Studio 2022, Linux 는 Clang 또는 GCC
- [Emscripten](https://emscripten.org) 6.0.10 (emsdk) — WebAssembly 빌드
- Python 3 — Skia 의존성 받기

FreeType, miniz, pugixml, brotli, woff2 는 CMake 가 처음 구성할 때 받아 온다(`cmake/ChwpDependencies.cmake`).

### Skia

화면 그리기는 [Skia](https://skia.org) M157(커밋 `5b1a7f7c06448e5d5f38f3c9360b18cae315576e`)을 쓴다. `build/skia` 에 받아 정적 라이브러리로 빌드한다. 없으면 엔진은 그리기 없이 빌드된다.

```bash
git clone https://skia.googlesource.com/skia.git build/skia
cd build/skia
git checkout 5b1a7f7c06448e5d5f38f3c9360b18cae315576e
python tools/git-sync-deps
```

빌드 설정은 `cmake/skia/` 에 있다. 산출물 폴더 이름은 `cmake/ChwpSkia.cmake` 가 찾는 이름과 같아야 한다.

| 대상 | 설정 파일 | 산출물 |
|---|---|---|
| WebAssembly | `cmake/skia/chwp-wasm-pthread.gn` | `build/skia/out/chwp-wasm-pthread/libskia.a` |
| Windows (Debug, GPU) | `cmake/skia/chwp-win-dawn.gn` | `build/skia/out/chwp-win-dawn/skia.lib` |
| Windows (Release, GPU) | `cmake/skia/chwp-win-dawn-release.gn` | `build/skia/out/chwp-win-dawn-release/skia.lib` |
| Windows (GPU 없음) | `cmake/skia/chwp-win.gn`, `chwp-win-release.gn` | `build/skia/out/chwp-win/skia.lib` 등 |
| Linux 정적(에이전트 패키지) | `cmake/skia/chwp-linux-musl.gn` | `build/skia/out/chwp-linux-musl/libskia.a` |

```bash
mkdir -p out/chwp-wasm-pthread
cp ../../cmake/skia/chwp-wasm-pthread.gn out/chwp-wasm-pthread/args.gn   # skia_emsdk_dir 를 고친다
bin/gn gen out/chwp-wasm-pthread
ninja -C out/chwp-wasm-pthread skia
```

WebAssembly 빌드는 Skia 에 딸린 Dawn 의 WebGPU 헤더(`out/dawn-wasm/gen/src/emdawnwebgpu/include`)도 쓴다. 이 헤더를 만드는 순서는 아직 정리 중이다.

### 네이티브

엔진 정적 라이브러리와 명령줄 도구(`chwp-agent`, `chwp-dump`)를 빌드한다. 도구를 빼려면 `-DCHWP_BUILD_TOOLS=OFF` 를 준다.

```bash
cmake --preset windows-msvc
cmake --build build/windows-msvc --config Release
```

Linux 는 `linux-release` 프리셋을 쓴다.

```bash
cmake --preset linux-release
cmake --build build/linux-release
```

### WebAssembly

emsdk 환경을 켠 뒤 빌드한다.

```bash
source <emsdk>/emsdk_env.sh
cmake --preset wasm-release
cmake --build build/wasm-release
```

### 에이전트 스킬

`skills/chwp-agent/` 는 AI 에이전트(Claude 등)가 한글 문서를 열어 읽고, 찾고, 고치고, 쪽을 PNG 로 그려 보고, HWP·HWPX 로 저장하게 하는 스킬이다.
스킬은 `chwp-agent` 실행 파일을 부른다. 저장소에서 쓸 때는 먼저 빌드한다.

```bash
cmake --build build/windows-msvc --config Release --target chwp-agent
python skills/chwp-agent/scripts/chwp_agent.py 문서.hwp -c '{"call":"info"}'
```

부르는 법과 요청 모양은 [skills/chwp-agent/SKILL.md](skills/chwp-agent/SKILL.md), [reference.md](skills/chwp-agent/reference.md) 에 있다.
claude.ai 에 올릴 스킬 묶음(Linux 정적 실행 파일 포함)은 `tools/chwp-agent/package-skill.py` 가 Docker 로 만든다.

### 웹 앱

웹 앱은 [ClassingHWP/web](https://github.com/ClassingHWP/web) 저장소에 있다. 이 저장소를 서브모듈 `core/` 로 넣고, 여기서 빌드한 WASM(`build/wasm-release`)을 쓴다.

## 글꼴

저장소에는 글꼴이 없다. 엔진은 글꼴을 직접 갖지 않고, 웹 앱이 글꼴 서버(`https://ede.kr/hwp/fonts/`)에서 받아 넘겨준다. 글꼴이 없으면 그 글꼴을 쓴 문서는 다른 글꼴로 그려져 줄 나눔이 달라질 수 있다.

- 라이선스 때문에 내줄 수 없는 글꼴(한컴 전용 글꼴, 윈도우 글꼴 등)은 OFL 글꼴 하나의 곡선으로 만든 읽기용 대체 글꼴 `ClassingHWP Fallback NNN` 으로 보여 준다. 문서를 고치기 시작하면 그 원본 OFL 글꼴로 바뀐다.
- 글꼴마다의 라이선스와 출처는 웹 앱의 **파일 → ClassingHWP 정보 → 글꼴** 에 있다.

## 라이선스

[Apache License 2.0](LICENSE) 에 **Commons Clause** 조건을 더한 소스 공개(source-available) 라이선스다. OSI 오픈 소스가 아니다.

- 개인 사용, 회사·기관 내부 사용, 수정본의 무료 재배포는 자유롭다.
- 판매, 유료·광고 서비스, 유료 제품 번들은 따로 협의해야 한다.

한국어 요약은 [LICENSE.ko.md](LICENSE.ko.md), 함께 들어간 라이브러리와 글꼴은 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 를 본다.

## 알림

본 제품은 한글과컴퓨터의 글 문서 파일(.hwp) 공개 문서를 참고하여 개발하였습니다.

한글과 한컴오피스는 한글과컴퓨터의 상표다. 이 프로그램은 한글과컴퓨터가 만들거나 보증하지 않는다.

## 문의

shimverz@naver.com
