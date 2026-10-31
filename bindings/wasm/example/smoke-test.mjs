// Node.js 로 WASM 모듈을 확인한다.
//   node bindings/wasm/example/smoke-test.mjs <noting.mjs 경로> <파일>...

import { readFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import path from "node:path";

const [modulePath, ...files] = process.argv.slice(2);
if (!modulePath || files.length === 0) {
  console.error("usage: node smoke-test.mjs <noting.mjs> <file.hwp|hwpx>...");
  process.exit(2);
}

const { default: createNotingModule } = await import(pathToFileURL(path.resolve(modulePath)).href);
const noting = await createNotingModule();

let failed = false;
for (const file of files) {
  const bytes = new Uint8Array(await readFile(file));
  const started = performance.now();
  const result = JSON.parse(noting.parse(bytes, false, false));
  const elapsed = (performance.now() - started).toFixed(0);
  const text = noting.extractText(bytes);

  if (!result.ok) {
    failed = true;
    console.log(`${path.basename(file)}: FAIL ${result.error}`);
    continue;
  }
  const doc = result.document;
  console.log(
    `${path.basename(file)}: ${noting.detectFormat(bytes)} ${doc.format.version}, ` +
      `charShapes=${doc.header.charShapes.length}, styles=${doc.header.styles.length}, ` +
      `paragraphs=${doc.sections[0].paragraphs.length}, text=${text.length} chars, ${elapsed} ms`
  );
}
process.exit(failed ? 1 : 0);
