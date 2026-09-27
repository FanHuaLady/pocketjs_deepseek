import { resolve } from "node:path";

const projectDir = resolve(import.meta.dir, "..");
const configPath = resolve(projectDir, process.env.PET_CONFIG ?? "pet_config.json");
let fileConfig: {
  device?: { url?: string };
  ai?: { apiUrl?: string; apiKey?: string; model?: string };
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

async function returnToIdle() {
  try {
    await sendToDevice({ phase: "idle" });
  } catch {
    // Preserve the original LLM error if the board is unreachable.
  }
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
<button id="send">发送</button><pre id="reply"></pre>
<script>
const message=document.querySelector("#message"), reply=document.querySelector("#reply");
document.querySelector("#send").onclick=async()=>{reply.textContent="思考中...";
const r=await fetch("/api/chat",{method:"POST",headers:{"content-type":"application/json"},body:JSON.stringify({message:message.value})});
reply.textContent=JSON.stringify(await r.json(),null,2)};
</script>`;

const server = Bun.serve({
  port,
  async fetch(request) {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/") {
      return new Response(html, { headers: { "content-type": "text/html; charset=utf-8" } });
    }
    if (request.method !== "POST" || url.pathname !== "/api/chat") {
      return new Response("Not found", { status: 404 });
    }
    if (!llmKey) {
      return Response.json({ error: "set LLM_API_KEY first" }, { status: 500 });
    }
    const body = await request.json() as { message?: string };
    const message = body.message?.trim();
    if (!message) return Response.json({ error: "message is required" }, { status: 400 });

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
        return Response.json({ error: `LLM returned HTTP ${llmResponse.status}` }, { status: 502 });
      }
      const llmBody = await llmResponse.json() as { choices?: Array<{ message?: { content?: string } }> };
      const content = llmBody.choices?.[0]?.message?.content;
      if (!content) {
        await returnToIdle();
        thinking = false;
        return Response.json({ error: "LLM returned no content" }, { status: 502 });
      }

      const command = jsonFromText(content);
      const asset = command.animation ? assets.get(command.animation) : undefined;
      if (!asset) throw new Error("invalid animation id");
      await sendToDevice({ phase: "response", animation: asset.id, mode: "once" });
      thinking = false;
      return Response.json({
        reply: typeof command.reply === "string" ? command.reply : "",
        animation: asset.id,
        mode: "once",
      });
    } catch (error) {
      if (thinking) await returnToIdle();
      return Response.json({ error: `invalid LLM command: ${String(error)}` }, { status: 502 });
    }
  },
});

console.log(`pet server: http://0.0.0.0:${server.port}`);
console.log(`device: ${deviceUrl}`);
