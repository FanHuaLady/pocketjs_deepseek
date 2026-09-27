import { mount } from "@pocketjs/framework";
import { Sprite, View } from "@pocketjs/framework/components";
import { createAnimationManager } from "./animation_manager.ts";
const hostPet = (globalThis as unknown as { pet?: { poll?: () => string } }).pet;

function App() {
  const { sprite } = createAnimationManager(hostPet);
  return (
    <View class="w-full h-full flex-row justify-center items-center bg-[#101820]">
      <Sprite class="w-[192] h-[192]" sprite={sprite()} />
    </View>
  );
}

mount(App);
