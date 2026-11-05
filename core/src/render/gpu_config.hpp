#pragma once

// 이 빌드에 Skia Graphite(GPU)가 있는가. 웹은 브라우저 WebGPU, 네이티브는 Dawn 이 든 Skia 를 연결했을 때다.
// GPU 코드는 이 값으로 감싸고, 그 안에서 플랫폼마다 다른 것은 GPU 장치·표면을 얻는 호출뿐이다(gpu_device.cpp).
// 장치를 못 얻으면 같은 그리기를 Skia CPU 래스터로 한다.
#if defined(__EMSCRIPTEN__) || CHWP_HAS_DAWN
#define CHWP_HAS_GPU 1
#else
#define CHWP_HAS_GPU 0
#endif
