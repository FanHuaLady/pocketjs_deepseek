# 架构说明

## 运行时角色

项目由三个运行时部分组成：

1. `pocket_host`：运行在 RV1106 上的 C 宿主程序。
2. `gif_demo.js`：由 PocketJS guest 运行的动画和交互逻辑。
3. `pocket_pet`：运行在电脑上的 Bun 独立程序。

RV1106 只负责设备能力和实时交互，不保存 LLM、ASR、TTS 密钥。电脑端负责访问外部 AI 服务，并通过 HTTP 控制开发板。

## 交互流程

### 文字对话

```text
终端输入
  -> pocket_pet
  -> LLM
  -> TTS
  -> pocket_host /audio
  -> 开发板扬声器
```

文字回复会直接显示在电脑终端，开发板同步播放思考和说话动画。

### 触摸屏语音对话

```text
触摸屏按住
  -> pocket_host 录音
触摸屏松开
  -> pocket_pet 轮询 /voice
  -> ASR
  -> LLM
  -> TTS
  -> pocket_host /audio
```

录音数据不会由电脑端麦克风采集。开发板完成录音，电脑端只负责取走音频并调用服务。

### 触摸事件

开发板把触摸行为写入事件队列，电脑端轮询 `/event`。例如摸头事件会触发电脑端对话，语音录音事件会触发电脑端取走录音。

## HTTP 接口

HTTP 服务默认监听 `17890` 端口，主要接口如下：

- `GET /event`：读取一个待处理触摸或录音事件。
- `GET /voice`：读取已经完成的录音数据。
- `POST /audio`：提交 PCM/WAV 音频供开发板播放。
- `POST /command`：发送动画、状态或控制命令。

具体请求格式以 `src/runtime/pet_host.c` 和 `tools/pet_server.ts` 为准。接口属于项目内部协议，修改时需要同时更新两端。

## 构建产物关系

```text
assets/original_gif/
  -> tools/prepare_gif_rv1106.ts
  -> assets/processed/*.png
  -> app/gif_demo/pet-assets.ts
  -> tools/build_guest_rv1106.sh
  -> build/guest/gif_demo.js + gif_demo.pak

src/*.c + third_party/quickjs + PocketJS runtime
  -> CMake
  -> build/rv1106-release/pocket_host
```

`sprites.json` 和 `asset-registry.json` 只用于资源构建过程。`pet-assets.ts` 是 guest 编译所需的生成源码，因此保留在仓库中，方便电脑端和开发板端直接构建。

## 配置边界

- `pet_config.json`：电脑端配置，包含外部服务地址和密钥，不提交。
- `RV1106_*` 环境变量：开发板端设备和运行参数。
- `gif_demo.js`、`gif_demo.pak`：开发板端资源和 guest 程序。

新增配置项时，应同时更新 `pet_config.example.json` 或 README 中的环境变量说明。
