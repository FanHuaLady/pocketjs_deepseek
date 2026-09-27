import { resolve } from "node:path";
import { Buffer } from "node:buffer";

const projectDir = resolve(import.meta.dir, "..");
const configPath = resolve(projectDir, process.env.PET_CONFIG ?? "pet_config.json");
let fileConfig: {
  device?: { url?: string };
  ai?: { apiUrl?: string; apiKey?: string; model?: string };
  asr?: { provider?: string; apiUrl?: string; apiKey?: string; model?: string; seconds?: number; language?: string };
  tts?: { provider?: string; apiKey?: string; model?: string; voice?: string };
  web?: { port?: number };
} = {};
try {
  fileConfig = await Bun.file(configPath).json();
} catch {
  // Environment variables remain a valid configuration source.
}
const registry = await Bun.file(resolve(projectDir, "assets/processed/asset-registry.json")).json() as {
  defaultId: string;
  assets: Array<{ id: string; kind: string; description: string; durationMs: number }>;
};
const assets = new Map(registry.assets.map((asset) => [asset.id, asset]));
const port = Number(process.env.PET_SERVER_PORT ?? fileConfig.web?.port ?? 8080);
const deviceUrl = process.env.PET_DEVICE_URL ?? fileConfig.device?.url ?? "http://127.0.0.1:17890";
const llmUrl = process.env.LLM_API_URL ?? fileConfig.ai?.apiUrl ?? "https://api.deepseek.com/chat/completions";
const llmKey = process.env.LLM_API_KEY ?? fileConfig.ai?.apiKey;
const llmModel = process.env.LLM_MODEL ?? fileConfig.ai?.model ?? "deepseek-chat";
const asrProvider = process.env.ASR_PROVIDER ?? fileConfig.asr?.provider ?? "qwen";
const asrUrl = process.env.ASR_API_URL ??
  fileConfig.asr?.apiUrl ??
  "https://maas.qianwenaiapi.com/api/v1/services/aigc/multimodal-generation/generation";
const asrKey = process.env.ASR_API_KEY ?? fileConfig.asr?.apiKey ?? fileConfig.tts?.apiKey;
const asrModel = process.env.ASR_MODEL ?? fileConfig.asr?.model ?? "qwen3-asr-flash";
const asrSeconds = Number(process.env.ASR_RECORD_SECONDS ?? fileConfig.asr?.seconds ?? 5);
const asrLanguage = process.env.ASR_LANGUAGE ?? fileConfig.asr?.language ?? "zh";
const ttsProvider = process.env.TTS_PROVIDER ?? fileConfig.tts?.provider ?? "qwen";
const ttsKey = process.env.TTS_API_KEY ?? fileConfig.tts?.apiKey;
const ttsModel = process.env.TTS_MODEL ?? fileConfig.tts?.model ?? "qwen3-tts-flash";
const ttsVoice = process.env.TTS_VOICE ?? fileConfig.tts?.voice ?? "Cherry";
const pocketAudioRate = 22050;

const assetGuide = registry.assets
  .map((asset) => `- ${asset.id}: ${asset.description}; mode=${asset.kind}`)
  .join("\n");

const systemPrompt = `You are a desktop pet controller.
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

async function recordAudioFromDevice(seconds: number) {
  const response = await fetch(`${deviceUrl}/listen`, {
    method: "POST",
    headers: {
      "x-record-seconds": String(Math.max(1, Math.min(15, Math.round(seconds)))),
      "x-audio-rate": "16000",
      "x-audio-channels": "1",
    },
  });
  if (!response.ok) throw new Error(`device listen returned HTTP ${response.status}`);
  const sampleRate = Number(response.headers.get("x-audio-rate") ?? "16000");
  const channels = Number(response.headers.get("x-audio-channels") ?? "1");
  return {
    sampleRate,
    channels,
    data: new Int16Array(await response.arrayBuffer()),
  };
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

const html = `<!doctype html>
<meta charset="utf-8">
<title>Pocket Pet</title>
<style>
body{font:16px sans-serif;max-width:680px;margin:40px auto;padding:0 16px;background:#101820;color:#eef}
textarea{width:100%;height:90px;box-sizing:border-box;background:#182532;color:#fff;border:1px solid #48627d;padding:10px}
button{margin-top:10px;padding:10px 18px;background:#70d6ff;border:0;color:#102238}
pre{white-space:pre-wrap;background:#182532;padding:12px;min-height:80px}
</style>
<h1>Pocket Pet</h1>
<textarea id="message" placeholder="和桌宠说点什么"></textarea><br>
<button id="send">发送</button>
<button id="voice">录音说话</button><pre id="reply"></pre>
<script>
const message=document.querySelector("#message"), reply=document.querySelector("#reply");
document.querySelector("#send").onclick=async()=>{reply.textContent="思考中...";
const r=await fetch("/api/chat",{method:"POST",headers:{"content-type":"application/json"},body:JSON.stringify({message:message.value})});
reply.textContent=JSON.stringify(await r.json(),null,2)};
document.querySelector("#voice").onclick=async()=>{reply.textContent="录音中...";
const r=await fetch("/api/voice-chat",{method:"POST"});
const body=await r.json(); if(body.transcript) message.value=body.transcript;
reply.textContent=JSON.stringify(body,null,2)};
</script>`;

async function chatWithPet(message: string) {
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
    const asset = command.animation ? assets.get(command.animation) : undefined;
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
    await chatWithPet(transcript);
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

const server = Bun.serve({
  port,
  async fetch(request) {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/") {
      return new Response(html, { headers: { "content-type": "text/html; charset=utf-8" } });
    }
    if (request.method === "POST" && url.pathname === "/api/chat") {
      const body = await request.json() as { message?: string };
      const message = body.message?.trim();
      if (!message) return Response.json({ error: "message is required" }, { status: 400 });
      const result = await chatWithPet(message);
      return Response.json(result.body, { status: result.status });
    }
    if (request.method === "POST" && url.pathname === "/api/voice-chat") {
      try {
        const pcm = await recordAudioFromDevice(asrSeconds);
        const transcript = await transcribeSpeech(pcm);
        const result = await chatWithPet(transcript);
        return Response.json({ transcript, ...result.body }, { status: result.status });
      } catch (error) {
        await returnToIdle();
        return Response.json({ error: String(error) }, { status: 502 });
      }
    }
    return new Response("Not found", { status: 404 });
  },
});

console.log(`pet server: http://0.0.0.0:${server.port}`);
console.log(`device: ${deviceUrl}`);
setInterval(() => {
  void pollQueuedVoice();
}, 1000);
