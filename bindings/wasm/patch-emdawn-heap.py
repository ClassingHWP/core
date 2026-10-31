# Dawn 패키지를 다시 만들어도 WebGPU가 커질 수 있는 wasm 힙을 받지 않게 한다.
# 호출에 쓰는 구간만 고정 버퍼로 복사한다.
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "build" / "skia" / "out" / "dawn-wasm" / "emdawnwebgpu_pkg" / "webgpu" / "src" / "library_webgpu.js"

HELPER = """
    // 커질 수 있는 wasm 힙은 WebGPU가 거절한다. 호출에 쓰는 구간만 고정 버퍼로 복사한다.
    fixedU8: (ptr, size) => {
      ptr >>>= 0;
      size >>>= 0;
      var view = HEAPU8.subarray(ptr, ptr + size);
      if (view.buffer.resizable || view.buffer.growable) return view.slice();
      return view;
    },
    fixedOffsets: (ptr, count) => {
      ptr >>>= 0;
      count >>>= 0;
      var view = HEAPU32.subarray(ptr >>> 2, (ptr >>> 2) + count);
      if (view.buffer.resizable || view.buffer.growable) return view.slice();
      return view;
    },

"""

BIND = "pass.setBindGroup(groupIndex, group, HEAPU32, {{{ getHeapOffset('dynamicOffsetsPtr', 'u32') }}}, dynamicOffsetCount);"
BIND_FIXED = "pass.setBindGroup(groupIndex, group, WebGPU.fixedOffsets(dynamicOffsetsPtr, dynamicOffsetCount));"
BYTES = "var subarray = HEAPU8.subarray(data, data + size);"
BYTES_FIXED = "var subarray = WebGPU.fixedU8(data, size);"
TEXTURE = "var subarray = HEAPU8.subarray(data, data + dataSize);"
TEXTURE_FIXED = "var subarray = WebGPU.fixedU8(data, dataSize);"
ANCHOR = "    // Public utility functions useful for translating between WASM/JS."


def main():
    if not TARGET.is_file():
        raise SystemExit(f"emdawn 라이브러리가 없습니다: {TARGET}")
    text = TARGET.read_text(encoding="utf-8")
    original = text
    if "fixedU8:" not in text:
        if ANCHOR not in text:
            raise SystemExit("WebGPU 도우미를 넣을 위치를 찾지 못했습니다.")
        text = text.replace(ANCHOR, HELPER + ANCHOR, 1)
    text = text.replace(BIND, BIND_FIXED)
    text = text.replace(BYTES, BYTES_FIXED)
    text = text.replace(TEXTURE, TEXTURE_FIXED)
    if text == original:
        print("emdawn heap copy already applied")
        return
    TARGET.write_text(text, encoding="utf-8", newline="\n")
    print("emdawn heap copy applied")


if __name__ == "__main__":
    main()
