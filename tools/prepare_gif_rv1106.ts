import { parseGIF, decompressFrames } from "gifuct-js";
import { createCanvas } from "../third_party/pocketjs/node_modules/@napi-rs/canvas/index.js";
import { mkdirSync, readdirSync, rmSync, unlinkSync } from "node:fs";
import { resolve } from "node:path";

const projectDir = resolve(import.meta.dir, "..");
const assetsDir = resolve(projectDir, "assets/original_gif");
const outputDir = resolve(projectDir, "assets/processed");
const outputFps = 30;
const tickRate = 60;
const maxFrames = 32;
type GifFrame = {
  delay?: number;
  disposalType?: number;
  dims: { left: number; top: number; width: number; height: number };
  patch: Uint8ClampedArray;
};

const names: Record<string, { id: string; kind: "loop" | "once"; description: string }> = {
  "token要不足了.gif": { id: "token_low", kind: "once", description: "token is running low" },
  "写代码.gif": { id: "coding", kind: "loop", description: "writing code" },
  "加油.gif": { id: "encourage", kind: "once", description: "encouraging" },
  "吃瓜.gif": { id: "watching_melon", kind: "loop", description: "watching the situation" },
  "吃甜甜圈.gif": { id: "eating_donut", kind: "loop", description: "eating a donut" },
  "哭.gif": { id: "crying", kind: "once", description: "crying" },
  "唱歌.gif": { id: "singing", kind: "loop", description: "singing" },
  "嘲笑.gif": { id: "teasing", kind: "once", description: "teasing" },
  "坐牢.gif": { id: "jailed", kind: "loop", description: "stuck in jail" },
  "墨镜装酷.gif": { id: "cool", kind: "loop", description: "acting cool" },
  "委屈巴巴.gif": { id: "aggrieved", kind: "once", description: "feeling aggrieved" },
  "害怕.gif": { id: "scared", kind: "once", description: "scared" },
  "害羞.gif": { id: "shy", kind: "once", description: "shy" },
  "小丑.gif": { id: "clown", kind: "loop", description: "being silly" },
  "扇子扇风.gif": { id: "fanning", kind: "loop", description: "fanning itself" },
  "打招呼.gif": { id: "greeting", kind: "once", description: "waving hello" },
  "打游戏.gif": { id: "gaming", kind: "loop", description: "playing a game" },
  "拿刀黑脸.gif": { id: "angry", kind: "once", description: "angry with a knife" },
  "无语.gif": { id: "speechless", kind: "once", description: "speechless" },
  "期待.gif": { id: "expectant", kind: "loop", description: "waiting expectantly" },
  "点头.gif": { id: "nodding", kind: "once", description: "nodding" },
  "点赞.gif": { id: "thumbs_up", kind: "once", description: "giving a thumbs up" },
  "爱你.gif": { id: "love", kind: "once", description: "showing affection" },
  "生气的写代码.gif": { id: "angry_coding", kind: "loop", description: "angrily writing code" },
  "疲惫.gif": { id: "tired", kind: "loop", description: "tired" },
  "睡觉.gif": { id: "sleeping", kind: "loop", description: "sleeping" },
  "睡觉前玩手机.gif": { id: "phone_before_sleep", kind: "loop", description: "using a phone before sleep" },
  "自信的思考.gif": { id: "confident_thinking", kind: "loop", description: "thinking confidently" },
  "被摸头.gif": { id: "head_pat", kind: "once", description: "being petted" },
  "警报.gif": { id: "alarm", kind: "once", description: "alarm" },
  "认真的思考.gif": { id: "serious_thinking", kind: "loop", description: "thinking seriously" },
  "记录.gif": { id: "recording", kind: "loop", description: "taking notes" },
  "跳舞.gif": { id: "dancing", kind: "loop", description: "dancing" },
  "震惊.gif": { id: "shocked", kind: "once", description: "shocked" },
  "馋.gif": { id: "hungry", kind: "loop", description: "hungry" },
};

mkdirSync(outputDir, { recursive: true });
for (const file of readdirSync(outputDir, { withFileTypes: true })) {
  if (file.isDirectory()) {
    rmSync(resolve(outputDir, file.name), { recursive: true, force: true });
  } else if (/\.(?:png|json|ts)$/i.test(file.name)) {
    unlinkSync(resolve(outputDir, file.name));
  }
}

function nextPowerOfTwo(value: number): number {
  let result = 1;
  while (result < value) result *= 2;
  return result;
}

function findGrid(
  frameCount: number,
  cellSizes: number[],
): { cellSize: number; cols: number; rows: number } | undefined {
  for (const cellSize of cellSizes) {
    for (let cols = 1; cols * cellSize <= 512; cols *= 2) {
      const rows = Math.ceil(frameCount / cols);
      if (nextPowerOfTwo(rows) === rows && rows * cellSize <= 512) {
        return { cellSize, cols, rows };
      }
    }
  }
  return undefined;
}

function chooseFramePlan(idealFrameCount: number): {
  frameCount: number;
  grid: { cellSize: number; cols: number; rows: number };
} {
  const direct = findGrid(idealFrameCount, [128, 64]);
  if (direct) return { frameCount: idealFrameCount, grid: direct };

  for (let frameCount = idealFrameCount - 1; frameCount >= 1; frameCount--) {
    const grid = findGrid(frameCount, [128, 64]);
    if (grid) return { frameCount, grid };
  }

  const fallback = findGrid(idealFrameCount, [32, 16]);
  if (fallback) return { frameCount: idealFrameCount, grid: fallback };
  throw new Error(`Cannot fit ${idealFrameCount} frames into a PocketJS atlas`);
}

function renderAtlas(frames: GifFrame[], width: number, height: number, frameCount: number, totalMs: number) {
  const plan = chooseFramePlan(frameCount);
  frameCount = plan.frameCount;
  const { grid } = plan;
  const { cellSize, cols: columns, rows } = grid;
  const atlas = createCanvas(
    nextPowerOfTwo(columns * cellSize),
    nextPowerOfTwo(rows * cellSize),
  );
  const atlasContext = atlas.getContext("2d");
  const canvas = createCanvas(width, height);
  const context = canvas.getContext("2d");
  const patchCanvas = createCanvas(width, height);
  const patchContext = patchCanvas.getContext("2d");
  const durations = frames.map((frame) => Math.max(10, frame.delay ?? 100));
  let sourceIndex = -1;
  let nextFrameStartMs = 0;
  let previousFrame: (typeof frames)[number] | undefined;
  let restorePrevious: ReturnType<typeof context.getImageData> | undefined;

  function drawSourceFrame(index: number): void {
    const frame = frames[index];
    if (previousFrame?.disposalType === 2) {
      context.clearRect(previousFrame.dims.left, previousFrame.dims.top, previousFrame.dims.width, previousFrame.dims.height);
    } else if (previousFrame?.disposalType === 3 && restorePrevious) {
      context.putImageData(restorePrevious, 0, 0);
    }
    restorePrevious = frame.disposalType === 3 ? context.getImageData(0, 0, width, height) : undefined;
    patchContext.clearRect(0, 0, width, height);
    const patch = patchContext.createImageData(frame.dims.width, frame.dims.height);
    patch.data.set(frame.patch);
    patchContext.putImageData(patch, frame.dims.left, frame.dims.top);
    context.drawImage(patchCanvas, 0, 0);
    previousFrame = frame;
    sourceIndex = index;
  }

  drawSourceFrame(0);
  nextFrameStartMs = durations[0];
  for (let outputIndex = 0; outputIndex < frameCount; outputIndex++) {
    const sampleMs = (outputIndex * totalMs) / frameCount;
    while (sourceIndex + 1 < frames.length && sampleMs >= nextFrameStartMs) {
      const nextIndex = sourceIndex + 1;
      drawSourceFrame(nextIndex);
      nextFrameStartMs += durations[nextIndex];
    }
    const column = outputIndex % columns;
    const row = Math.floor(outputIndex / columns);
    const scale = Math.min(cellSize / width, cellSize / height);
    const drawWidth = Math.max(1, Math.round(width * scale));
    const drawHeight = Math.max(1, Math.round(height * scale));
    atlasContext.drawImage(canvas, 0, 0, width, height,
      column * cellSize + Math.floor((cellSize - drawWidth) / 2),
      row * cellSize + Math.floor((cellSize - drawHeight) / 2),
      drawWidth, drawHeight);
  }
  return {
    atlas,
    cols: columns,
    rows,
    frames: frameCount,
    step: Math.max(1, Math.round((totalMs * tickRate) / 1000 / frameCount)),
  };
}

const registry: Array<Record<string, unknown>> = [];
const spriteManifest: Record<string, Record<string, number>> = {};
const files = readdirSync(assetsDir).filter((file) => file.toLowerCase().endsWith(".gif")).sort();
if (files.length === 0) throw new Error(`No GIF files found in ${assetsDir}`);

for (const sourceName of files) {
  const metadata = names[sourceName];
  if (!metadata) throw new Error(`Missing English mapping for ${sourceName}`);
  const parsed = parseGIF(await Bun.file(resolve(assetsDir, sourceName)).arrayBuffer());
  const frames = decompressFrames(parsed, true) as GifFrame[];
  if (frames.length === 0) throw new Error(`${sourceName}: no image frames`);
  const totalMs = frames.reduce((sum, frame) => sum + Math.max(10, frame.delay ?? 100), 0);
  const frameCount = Math.max(1, Math.min(maxFrames, Math.round((totalMs * outputFps) / 1000)));
  const result = renderAtlas(frames, parsed.lsd.width, parsed.lsd.height, frameCount, totalMs);
  const spriteFileName = `${metadata.id}.png`;
  const spriteName = `processed/${spriteFileName}`;
  await Bun.write(resolve(outputDir, spriteFileName), result.atlas.toBuffer("image/png"));
  spriteManifest[spriteName] = { cols: result.cols, rows: result.rows, frames: result.frames, step: result.step };
  registry.push({
    id: metadata.id,
    sprite: spriteName,
    source: sourceName,
    kind: metadata.kind,
    description: metadata.description,
    durationMs: totalMs,
  });
  console.log(`${metadata.id}: ${frames.length} source frames, ${totalMs}ms -> ${result.atlas.width}x${result.atlas.height}, ${result.frames} frames`);
}

await Bun.write(resolve(outputDir, "sprites.json"), `${JSON.stringify(spriteManifest, null, 2)}\n`);
await Bun.write(resolve(outputDir, "asset-registry.json"), `${JSON.stringify({ defaultId: "expectant", assets: registry }, null, 2)}\n`);
await Bun.write(resolve(outputDir, "pet-assets.ts"),
  `export const PET_ASSETS = ${JSON.stringify(registry, null, 2)} as const;\n` +
  `export const DEFAULT_PET_ID = "expectant";\n`);
