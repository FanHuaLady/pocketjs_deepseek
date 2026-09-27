import { createSignal, type Accessor } from "solid-js";
import { createWavPlayer, type WavPcm, type WavPlayer } from "@pocketjs/framework/audio";
import { onFrame } from "@pocketjs/framework/lifecycle";
import { PET_ASSETS } from "../../assets/processed/pet-assets.ts";

const FRAME_RATE = 60;
const IDLE_ANIMATIONS = [
  "watching_melon",
  "eating_donut",
  "cool",
  "fanning",
] as const;

const THINKING_ANIMATIONS = [
  "serious_thinking",
  "confident_thinking",
  "recording",
] as const;

type PetAsset = (typeof PET_ASSETS)[number];

export type PetCommand = {
  phase?: "idle" | "thinking" | "response";
  animation?: string;
  mode?: "loop" | "once";
};

type PetHost = { poll?: () => string };
type PetAudioHost = {
  poll?: () => string | undefined;
  read?: (id: number) => ArrayBuffer | undefined;
};
type PetAudioMetadata = {
  id?: number;
  sampleRate?: number;
  channels?: number;
  bytes?: number;
};

const assets = new Map<string, PetAsset>(PET_ASSETS.map((asset) => [asset.id, asset]));

function createSpeechPlayer(): { pump: () => void } {
  const transport = (globalThis as unknown as { petAudio?: PetAudioHost }).petAudio;
  const player: WavPlayer = createWavPlayer();
  let lastAudioId = 0;

  function readNextPcm(): WavPcm | undefined {
    const raw = transport?.poll?.();
    if (!raw) return undefined;
    let metadata: PetAudioMetadata;
    try {
      metadata = JSON.parse(raw) as PetAudioMetadata;
    } catch {
      return undefined;
    }
    if (
      !metadata.id ||
      metadata.id === lastAudioId ||
      !metadata.sampleRate ||
      !metadata.channels
    ) {
      return undefined;
    }
    const buffer = transport?.read?.(metadata.id);
    if (!buffer) return undefined;
    lastAudioId = metadata.id;
    const data = new Int16Array(buffer);
    return {
      sampleRate: metadata.sampleRate,
      channels: metadata.channels,
      frames: Math.floor(data.length / metadata.channels),
      data,
    };
  }

  return {
    pump(): void {
      const pcm = readNextPcm();
      if (pcm && player.loadPcm(pcm)) player.play();
      player.pump();
    },
  };
}

export function createAnimationManager(hostPet: PetHost | undefined): { sprite: Accessor<string> } {
  const firstIdle = assets.get(IDLE_ANIMATIONS[0])!;
  const hostConfig = (hostPet as PetHost & {
    config?: { idleSeconds?: number; responseMultiplier?: number };
  } | undefined)?.config;
  const idleSeconds = Number.isFinite(hostConfig?.idleSeconds)
    ? Math.max(1, hostConfig!.idleSeconds!)
    : 60;
  const responseMultiplier = Number.isFinite(hostConfig?.responseMultiplier)
    ? Math.max(0.1, hostConfig!.responseMultiplier!)
    : 3;
  const idleSwitchFrames = FRAME_RATE * idleSeconds;
  const [sprite, setSprite] = createSignal(firstIdle.sprite);
  let phase: PetCommand["phase"] = "idle";
  let idleIndex = 0;
  let thinkingIndex = 0;
  let idleElapsedFrames = 0;
  let responseRemainingFrames = 0;
  const speechPlayer = createSpeechPlayer();

  function selectAnimation(id: string): PetAsset | undefined {
    const asset = assets.get(id);
    if (!asset) return undefined;
    setSprite(asset.sprite);
    return asset;
  }

  function enterIdle(): void {
    phase = "idle";
    idleElapsedFrames = 0;
    responseRemainingFrames = 0;
    selectAnimation(IDLE_ANIMATIONS[idleIndex]);
  }

  function enterNextIdle(): void {
    idleIndex = (idleIndex + 1) % IDLE_ANIMATIONS.length;
    enterIdle();
  }

  function enterThinking(): void {
    phase = "thinking";
    idleElapsedFrames = 0;
    responseRemainingFrames = 0;
    const animation = THINKING_ANIMATIONS[thinkingIndex];
    thinkingIndex = (thinkingIndex + 1) % THINKING_ANIMATIONS.length;
    selectAnimation(animation);
  }

  function handleCommand(command: PetCommand): void {
    const commandPhase = command.phase ?? (command.animation ? "response" : undefined);
    if (commandPhase === "idle") {
      enterIdle();
      return;
    }
    if (commandPhase === "thinking") {
      enterThinking();
      return;
    }
    if (commandPhase !== "response" || !command.animation) return;

    const asset = selectAnimation(command.animation);
    if (!asset) return;
    phase = "response";
    idleElapsedFrames = 0;
    responseRemainingFrames = Math.max(
      1,
      Math.ceil((asset.durationMs * responseMultiplier / 1000) * FRAME_RATE),
    );
  }

  onFrame(() => {
    const raw = hostPet?.poll?.();
    if (raw) {
      try {
        handleCommand(JSON.parse(raw) as PetCommand);
      } catch {
        // Ignore malformed commands and keep the current animation.
      }
    }
    speechPlayer.pump();

    if (phase === "response" && responseRemainingFrames > 0) {
      responseRemainingFrames -= 1;
      if (responseRemainingFrames === 0) enterIdle();
    } else if (phase === "idle") {
      idleElapsedFrames += 1;
      if (idleElapsedFrames >= idleSwitchFrames) enterNextIdle();
    }
  });

  return { sprite };
}
