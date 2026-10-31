# Third-party notices

ClassingHWP 문서 엔진에 들어가는 다른 사람의 코드와 글꼴이다. 각자의 라이선스를 따른다. `core/third_party/` 의 코드는 파일 머리·끝에 라이선스가 있고, 나머지는 빌드할 때 받아 온다. 라이선스 전문은 웹 앱 저장소의 `public/license/oss/` 와 웹 앱의 **파일 → ClassingHWP 정보** 창에서도 볼 수 있다. 웹 화면 라이브러리는 [ClassingHWP/web](https://github.com/ClassingHWP/web) 의 고지를 본다.

## 코드

### 문서 엔진 (C++ / WebAssembly)

| 이름 | 버전 | 라이선스 | 저작권 |
|---|---|---|---|
| [Skia](https://skia.org) | M157 (`5b1a7f7c`) | BSD-3-Clause | Copyright (c) 2011 Google Inc. |
| [Dawn](https://dawn.googlesource.com/dawn) | Skia M157 에 딸린 판 | BSD-3-Clause | Copyright 2017-2026 The Dawn & Tint Authors |
| [FreeType](https://freetype.org) | 2.13.3 | FreeType License (FTL) | Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved. |
| [Brotli](https://github.com/google/brotli) | 1.1.0 | MIT | Copyright (c) 2009, 2010, 2013-2016 by the Brotli Authors. |
| [WOFF2](https://github.com/google/woff2) | 1.0.2 | MIT | Copyright (c) 2013-2017 by the WOFF2 Authors. |
| [miniz](https://github.com/richgel999/miniz) | 3.0.2 | MIT | Copyright 2013-2014 RAD Game Tools and Valve Software |
| [pugixml](https://pugixml.org) | 1.14 | MIT | Copyright (c) 2006-2023 Arseny Kapoulkine |
| [MicroTeX](https://github.com/NanoMichael/MicroTeX) | `0e3707f6` | MIT | Copyright (c) 2020 Nano Michael |
| [TinyXML-2](https://github.com/leethomason/tinyxml2) | 10.0.0 | zlib | Lee Thomason |
| [LZ4](https://github.com/lz4/lz4) (`core/third_party/lz4`) | — | BSD-2-Clause | Copyright (C) 2011-2023, Yann Collet. |
| [stb](https://github.com/nothings/stb) (`core/third_party/stb`) | — | MIT 또는 퍼블릭 도메인 | Copyright (c) 2017 Sean Barrett |
| [Emscripten](https://emscripten.org) 런타임 | 6.0.10 | MIT | Copyright (c) 2010-2014 Emscripten authors |

FreeType 의 FTL 은 문서에 위 출처 문장을 적도록 요구한다.

## 글꼴

이 저장소에는 **글꼴이 들어 있지 않다.** 웹 앱이 글꼴 서버에서 받아 쓰고, 서버에는 글꼴마다의 라이선스 글과 출처가 함께 있다. 웹 앱의 **파일 → ClassingHWP 정보 → 글꼴** 이 그 목록을 보인다.

| 묶음 | 라이선스 | 출처 |
|---|---|---|
| 바탕·바탕체·굴림·굴림체·돋움·돋움체·궁서·궁서체 | SIL OFL 1.1 | Google Fonts 의 [googlefonts/batang](https://github.com/googlefonts/batang), [googlefonts/gulim](https://github.com/googlefonts/gulim). 어센트·디센트 같은 세로 값만 바꿨다. |
| 대체 글꼴 Noting Fallback 001~130 | SIL OFL 1.1 | 라이선스 때문에 내줄 수 없는 글꼴을 비슷하게 보여 주려고 OFL 글꼴 하나의 곡선으로 만든 읽기용 글꼴이다. 원래 글꼴과는 다른 글꼴이다. |
| Noting Bullet | SIL OFL 1.1 | 글머리표 원 하나만 직접 그린 글꼴 |
| 그 밖의 글꼴 | 글꼴마다 다르다(OFL, Apache-2.0, 공공누리 등) | 정보 창의 글꼴 탭 |

## 한글 문서 형식

본 제품은 한글과컴퓨터의 글 문서 파일(.hwp) 공개 문서를 참고하여 개발하였습니다. 한글과 한컴오피스는 한글과컴퓨터의 상표이며, 이 프로그램은 한글과컴퓨터가 만들거나 보증하지 않는다.
