import { dirname, resolve } from "node:path";
import { Buffer } from "node:buffer";
import { createInterface } from "node:readline/promises";
import { stdin as input, stdout as output } from "node:process";
import { PET_ASSETS } from "../assets/processed/pet-assets.ts";

const configPaths = process.env.PET_CONFIG
  ? [process.env.PET_CONFIG]
  : [
      resolve(dirname(process.execPath), "pet_config.json"),
      resolve(process.cwd(), "pet_config.json"),
    ];
let fileConfig: {
  device?: { url?: string };
  ai?: { apiUrl?: string; apiKey?: string; model?: string; personaPrompt?: string };
  asr?: { provider?: string; apiUrl?: string; apiKey?: string; model?: string; language?: string };
  tts?: { provider?: string; apiKey?: string; model?: string; voice?: string };
} = {};
for (const configPath of configPaths) {
  try {
    fileConfig = await Bun.file(configPath).json();
    break;
  } catch {
    // Try the next supported configuration location.
  }
}
const registry = { defaultId: "expectant", assets: PET_ASSETS };
const assets = new Map(registry.assets.map((asset) => [asset.id, asset]));
const deviceUrl = process.env.PET_DEVICE_URL ?? fileConfig.device?.url ?? "http://127.0.0.1:17890";
const llmUrl = process.env.LLM_API_URL ?? fileConfig.ai?.apiUrl ?? "https://api.deepseek.com/chat/completions";
const llmKey = process.env.LLM_API_KEY ?? fileConfig.ai?.apiKey;
const llmModel = process.env.LLM_MODEL ?? fileConfig.ai?.model ?? "deepseek-chat";
const personaPrompt = process.env.PERSONA_PROMPT ?? fileConfig.ai?.personaPrompt ?? "";
const asrProvider = process.env.ASR_PROVIDER ?? fileConfig.asr?.provider ?? "qwen";
const asrUrl = process.env.ASR_API_URL ??
  fileConfig.asr?.apiUrl ??
  "https://maas.qianwenaiapi.com/api/v1/services/aigc/multimodal-generation/generation";
const asrKey = process.env.ASR_API_KEY ?? fileConfig.asr?.apiKey ?? fileConfig.tts?.apiKey;
const asrModel = process.env.ASR_MODEL ?? fileConfig.asr?.model ?? "qwen3-asr-flash";
const asrLanguage = process.env.ASR_LANGUAGE ?? fileConfig.asr?.language ?? "zh";
const ttsProvider = process.env.TTS_PROVIDER ?? fileConfig.tts?.provider ?? "qwen";
const ttsKey = process.env.TTS_API_KEY ?? fileConfig.tts?.apiKey;
const ttsModel = process.env.TTS_MODEL ?? fileConfig.tts?.model ?? "qwen3-tts-flash";
const ttsVoice = process.env.TTS_VOICE ?? fileConfig.tts?.voice ?? "Cherry";
const pocketAudioRate = 22050;

const assetGuide = registry.assets
  .map((asset) => `- ${asset.id}: ${asset.description}; mode=${asset.kind}`)
  .join("\n");

const systemPrompt = `${personaPrompt ? `${personaPrompt}\n\n` : ""}You are a desktop pet controller.
Reply with valid JSON only: {"reply":"short Chinese reply","animation":"asset id","mode":"loop|once"}.
Choose exactly one animation from this allowlist:
${assetGuide}
Use mode "once" for reactions and mode "loop" for persistent activities.
Do not invent animation ids. Keep reply under 80 Chinese characters.`;

function jsonFromText(text: string): { reply?: string; animation?: string; mode?: string } {
  const unfenced = text.replace(/^```(?:json)?\s*/i, "").replace(/\s*```$/i, "").trim();
  const start = unfenced.indexOf("{");
  const end = unfenced.lastIndexOf("}");
  return JSON.parse(start >= 0 && end >= start ? unfenced.slice(start, end + 1) : unfenced);
}

type DeviceCommand = {
  phase: "idle" | "thinking" | "response";
  animation?: string;
  mode?: "loop" | "once";
};

async function sendToDevice(command: DeviceCommand) {
  const response = await fetch(`${deviceUrl}/command`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(command),
  });
  if (!response.ok) throw new Error(`device returned HTTP ${response.status}`);
}

async function sendAudioToDevice(pcm: { sampleRate: number; channels: number; data: Int16Array }) {
  const bytes = new Uint8Array(pcm.data.buffer, pcm.data.byteOffset, pcm.data.byteLength);
  const response = await fetch(`${deviceUrl}/audio`, {
    method: "POST",
    headers: {
      "content-type": "application/octet-stream",
      "x-audio-rate": String(pcm.sampleRate),
      "x-audio-channels": String(pcm.channels),
    },
    body: bytes,
  });
  if (!response.ok) throw new Error(`device audio returned HTTP ${response.status}`);
}

async function returnToIdle() {
  try {
    await sendToDevice({ phase: "idle" });
  } catch {
    // Preserve the original LLM error if the board is unreachable.
  }
}

async function fetchQueuedVoiceFromDevice() {
  const response = await fetch(`${deviceUrl}/voice`, { method: "GET" });
  if (response.status === 204) return undefined;
  if (!response.ok) throw new Error(`device voice returned HTTP ${response.status}`);
  const sampleRate = Number(response.headers.get("x-audio-rate") ?? "16000");
  const channels = Number(response.headers.get("x-audio-channels") ?? "1");
  return {
    sampleRate,
    channels,
    data: new Int16Array(await response.arrayBuffer()),
  };
}

async function fetchQueuedEventFromDevice() {
  const response = await fetch(`${deviceUrl}/event`, { method: "GET" });
  if (response.status === 204) return undefined;
  if (!response.ok) throw new Error(`device event returned HTTP ${response.status}`);
  return await response.json() as { message?: string; delayMs?: number };
}

function sleep(ms: number) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function fourcc(view: DataView, offset: number): number {
  return view.getUint32(offset, true);
}

function decodeWavPcm16(bytes: Uint8Array): { sampleRate: number; channels: number; data: Int16Array } {
  const riff = 0x46464952;
  const wave = 0x45564157;
  const fmt = 0x20746d66;
  const data = 0x61746164;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let sampleRate = 0;
  let channels = 0;
  let dataOffset = -1;
  let dataLength = 0;

  if (bytes.byteLength < 44 || fourcc(view, 0) !== riff || fourcc(view, 8) !== wave) {
    throw new Error("TTS returned non-WAV audio");
  }
  for (let offset = 12; offset + 8 <= bytes.byteLength;) {
    const id = fourcc(view, offset);
    const size = view.getUint32(offset + 4, true);
    if (id === fmt) {
      const format = view.getUint16(offset + 8, true);
      channels = view.getUint16(offset + 10, true);
      sampleRate = view.getUint32(offset + 12, true);
      const bits = view.getUint16(offset + 22, true);
      if (format !== 1 || bits !== 16) {
        throw new Error(`TTS WAV must be 16-bit PCM, got format=${format} bits=${bits}`);
      }
    } else if (id === data) {
      dataOffset = offset + 8;
      dataLength = Math.min(size, bytes.byteLength - dataOffset);
    }
    offset += 8 + size + (size & 1);
  }
  if (dataOffset < 0 || !sampleRate || channels < 1 || channels > 2) {
    throw new Error("TTS WAV metadata is unsupported");
  }
  const sampleCount = Math.floor(dataLength / 2);
  const pcm = new Int16Array(sampleCount);
  for (let i = 0; i < sampleCount; i++) {
    pcm[i] = view.getInt16(dataOffset + i * 2, true);
  }
  return { sampleRate, channels, data: pcm };
}

function resamplePcm16(
  pcm: { sampleRate: number; channels: number; data: Int16Array },
  targetRate: number,
): { sampleRate: number; channels: number; data: Int16Array } {
  if (pcm.sampleRate === targetRate) return pcm;
  const inputFrames = Math.floor(pcm.data.length / pcm.channels);
  const outputFrames = Math.max(1, Math.floor(inputFrames * targetRate / pcm.sampleRate));
  const output = new Int16Array(outputFrames * pcm.channels);
  const ratio = pcm.sampleRate / targetRate;

  for (let frame = 0; frame < outputFrames; frame++) {
    const source = frame * ratio;
    const left = Math.floor(source);
    const right = Math.min(inputFrames - 1, left + 1);
    const fraction = source - left;
    for (let channel = 0; channel < pcm.channels; channel++) {
      const a = pcm.data[left * pcm.channels + channel] ?? 0;
      const b = pcm.data[right * pcm.channels + channel] ?? a;
      const sample = Math.round(a + (b - a) * fraction);
      output[frame * pcm.channels + channel] = sample < -32768
        ? -32768
        : sample > 32767
          ? 32767
          : sample;
    }
  }
  return { sampleRate: targetRate, channels: pcm.channels, data: output };
}

function monoPcm16(pcm: { sampleRate: number; channels: number; data: Int16Array }) {
  if (pcm.channels === 1) return pcm;
  const frames = Math.floor(pcm.data.length / pcm.channels);
  const output = new Int16Array(frames);
  for (let frame = 0; frame < frames; frame++) {
    let total = 0;
    for (let channel = 0; channel < pcm.channels; channel++) {
      total += pcm.data[frame * pcm.channels + channel] ?? 0;
    }
    output[frame] = Math.round(total / pcm.channels);
  }
  return { sampleRate: pcm.sampleRate, channels: 1, data: output };
}

function wavFromPcm16(pcm: { sampleRate: number; channels: number; data: Int16Array }) {
  const dataBytes = pcm.data.byteLength;
  const bytes = new Uint8Array(44 + dataBytes);
  const view = new DataView(bytes.buffer);
  const text = (offset: number, value: string) => {
    for (let i = 0; i < value.length; i++) bytes[offset + i] = value.charCodeAt(i);
  };

  text(0, "RIFF");
  view.setUint32(4, 36 + dataBytes, true);
  text(8, "WAVE");
  text(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, pcm.channels, true);
  view.setUint32(24, pcm.sampleRate, true);
  view.setUint32(28, pcm.sampleRate * pcm.channels * 2, true);
  view.setUint16(32, pcm.channels * 2, true);
  view.setUint16(34, 16, true);
  text(36, "data");
  view.setUint32(40, dataBytes, true);
  bytes.set(new Uint8Array(pcm.data.buffer, pcm.data.byteOffset, dataBytes), 44);
  return bytes;
}

function extractAsrText(body: unknown): string {
  const anyBody = body as {
    output?: {
      text?: string;
      choices?: Array<{ message?: { content?: string | Array<{ text?: string }> } }>;
    };
    choices?: Array<{ message?: { content?: string | Array<{ text?: string }> } }>;
  };
  const choices = anyBody.output?.choices ?? anyBody.choices;
  const content = choices?.[0]?.message?.content;
  if (typeof content === "string") return content.trim();
  if (Array.isArray(content)) {
    return content.map((part) => part.text ?? "").join("").trim();
  }
  if (typeof anyBody.output?.text === "string") return anyBody.output.text.trim();
  return "";
}

async function transcribeQwenSpeech(pcm: { sampleRate: number; channels: number; data: Int16Array }) {
  if (!asrKey) throw new Error("set ASR_API_KEY/asr.apiKey or tts.apiKey first");
  const wav = wavFromPcm16(monoPcm16(resamplePcm16(pcm, 16000)));
  const dataUrl = `data:audio/wav;base64,${Buffer.from(wav).toString("base64")}`;
  const response = await fetch(asrUrl, {
    method: "POST",
    headers: {
      authorization: `Bearer ${asrKey}`,
      "content-type": "application/json",
    },
    body: JSON.stringify({
      model: asrModel,
      input: {
        messages: [
          {
            role: "system",
            content: [{ text: "" }],
          },
          {
            role: "user",
            content: [{ audio: dataUrl }],
          },
        ],
      },
      parameters: {
        asr_options: {
          language: asrLanguage,
          enable_itn: true,
        },
      },
    }),
  });
  if (!response.ok) throw new Error(`ASR returned HTTP ${response.status}`);
  const text = extractAsrText(await response.json());
  if (!text) throw new Error("ASR returned no text");
  return text;
}

async function transcribeSpeech(pcm: { sampleRate: number; channels: number; data: Int16Array }) {
  if (asrProvider !== "qwen") {
    throw new Error(`unsupported ASR provider: ${asrProvider}`);
  }
  return transcribeQwenSpeech(pcm);
}

async function synthesizeQwenSpeech(text: string) {
  if (!ttsKey) throw new Error("set TTS_API_KEY or tts.apiKey first");
  const response = await fetch("https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation", {
    method: "POST",
    headers: {
      authorization: `Bearer ${ttsKey}`,
      "content-type": "application/json",
    },
    body: JSON.stringify({
      model: ttsModel,
      input: {
        text,
        voice: ttsVoice,
        language_type: "Chinese",
      },
    }),
  });
  if (!response.ok) throw new Error(`TTS returned HTTP ${response.status}`);
  const body = await response.json() as { output?: { audio?: { url?: string } } };
  const audioUrl = body.output?.audio?.url;
  if (!audioUrl) throw new Error("TTS returned no audio URL");
  const audioResponse = await fetch(audioUrl);
  if (!audioResponse.ok) throw new Error(`TTS audio returned HTTP ${audioResponse.status}`);
  const wav = new Uint8Array(await audioResponse.arrayBuffer());
  return resamplePcm16(decodeWavPcm16(wav), pocketAudioRate);
}

async function synthesizeSpeech(text: string) {
  if (ttsProvider !== "qwen") {
    throw new Error(`unsupported TTS provider: ${ttsProvider}`);
  }
  return synthesizeQwenSpeech(text);
}

async function chatWithPet(
  message: string,
  options: { forbiddenAnimations?: string[]; fallbackAnimation?: string } = {},
) {
  if (!llmKey) {
    return { body: { error: "set LLM_API_KEY first" }, status: 500 };
  }
  let thinking = false;
  try {
    await sendToDevice({ phase: "thinking" });
    thinking = true;
    const llmResponse = await fetch(llmUrl, {
      method: "POST",
      headers: {
        authorization: `Bearer ${llmKey}`,
        "content-type": "application/json",
      },
      body: JSON.stringify({
        model: llmModel,
        temperature: 0.7,
        response_format: { type: "json_object" },
        messages: [
          { role: "system", content: systemPrompt },
          { role: "user", content: message },
        ],
      }),
    });
    if (!llmResponse.ok) {
      await returnToIdle();
      thinking = false;
      return { body: { error: `LLM returned HTTP ${llmResponse.status}` }, status: 502 };
    }
    const llmBody = await llmResponse.json() as { choices?: Array<{ message?: { content?: string } }> };
    const content = llmBody.choices?.[0]?.message?.content;
    if (!content) {
      await returnToIdle();
      thinking = false;
      return { body: { error: "LLM returned no content" }, status: 502 };
    }

    const command = jsonFromText(content);
    const forbiddenAnimations = new Set(options.forbiddenAnimations ?? []);
    let asset = command.animation ? assets.get(command.animation) : undefined;
    if (asset && forbiddenAnimations.has(asset.id)) {
      asset = options.fallbackAnimation ? assets.get(options.fallbackAnimation) : undefined;
    }
    if (!asset) throw new Error("invalid animation id");
    const reply = typeof command.reply === "string" ? command.reply : "";
    let ttsError = "";
    if (reply) {
      try {
        await sendAudioToDevice(await synthesizeSpeech(reply));
      } catch (error) {
        ttsError = String(error);
      }
    }
    await sendToDevice({ phase: "response", animation: asset.id, mode: "once" });
    thinking = false;
    return {
      body: {
        reply,
        animation: asset.id,
        mode: "once",
        ...(ttsError ? { ttsError } : {}),
      },
      status: 200,
    };
  } catch (error) {
    if (thinking) await returnToIdle();
    return { body: { error: `invalid LLM command: ${String(error)}` }, status: 502 };
  }
}

let voicePollBusy = false;
async function pollQueuedVoice() {
  if (voicePollBusy) return;
  voicePollBusy = true;
  try {
    const pcm = await fetchQueuedVoiceFromDevice();
    if (!pcm) return;
    const transcript = await transcribeSpeech(pcm);
    console.log(`\n[开发板语音] ${transcript}`);
    printChatResult(await chatWithPet(transcript));
  } catch (error) {
    const text = String(error);
    if (!text.includes("ECONNREFUSED") && !text.includes("fetch failed")) {
      console.warn(`voice poll failed: ${text}`);
    }
    await returnToIdle();
  } finally {
    voicePollBusy = false;
  }
}

let eventPollBusy = false;
async function pollQueuedEvent() {
  if (eventPollBusy) return;
  eventPollBusy = true;
  try {
    const event = await fetchQueuedEventFromDevice();
    if (!event?.message) return;
    const delayMs = Math.max(0, Math.min(10000, Math.round(event.delayMs ?? 0)));
    if (delayMs > 0) await sleep(delayMs);
    printChatResult(await chatWithPet(event.message, {
      forbiddenAnimations: ["head_pat"],
      fallbackAnimation: "love",
    }));
  } catch (error) {
    const text = String(error);
    if (!text.includes("ECONNREFUSED") && !text.includes("fetch failed")) {
      console.warn(`event poll failed: ${text}`);
    }
  } finally {
    eventPollBusy = false;
  }
}

function printChatResult(result: {
  body: { reply?: string; animation?: string; error?: string; ttsError?: string };
  status: number;
}) {
  if (result.body.reply) console.log(`[桌宠] ${result.body.reply}`);
  if (result.body.animation) console.log(`[动画] ${result.body.animation}`);
  if (result.body.ttsError) console.warn(`[语音合成失败] ${result.body.ttsError}`);
  if (result.body.error) console.error(`[处理失败] ${result.body.error}`);
}

let inputBusy = false;
async function submitText(message: string) {
  if (inputBusy) {
    console.log("当前还有一条消息正在处理，请稍后再输入。");
    return;
  }
  inputBusy = true;
  try {
    console.log("[桌宠] 思考中...");
    printChatResult(await chatWithPet(message));
  } finally {
    inputBusy = false;
  }
}

console.log("Pocket Pet 终端已启动");
console.log(`设备: ${deviceUrl}`);
console.log("输入文字后按回车发送；开发板触摸屏下半区可进行语音对话；输入 Ctrl+C 退出。\n");
const pollTimer = setInterval(() => {
  void pollQueuedVoice();
  void pollQueuedEvent();
}, 1000);

const readline = createInterface({ input, output, prompt: "> " });
readline.prompt();
readline.on("SIGINT", () => readline.close());
readline.on("line", (line) => {
  const message = line.trim();
  if (message) void submitText(message);
  readline.prompt();
});
readline.on("close", async () => {
  clearInterval(pollTimer);
  await returnToIdle();
  process.exit(0);
});
