# RV1106 PocketJS Desktop Pet

这是一个运行在 RV1106 上的 PocketJS 桌宠最小版本。GIF 素材在构建时被
转换成 PocketJS 的 PNG sprite atlas，设备端只负责播放和切换动画。

## 构建

```bash
cd /home/flower/Echo-Mate/Flower/07_hello_deepseek
bun install
tools/build_guest_rv1106.sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release
```

构建会生成：

- `build/rv1106-release/pocket_host`
- `build/rv1106-release/gif_demo.js`
- `build/rv1106-release/gif_demo.pak`

转换脚本会扫描 `assets/original_gif/*.gif`，将中文文件名映射为稳定的英文
动画 ID，并把处理后的 atlas 和清单写入 `assets/processed/`。
`app/gif_demo/` 只保留 PocketJS 入口代码。

## 部署到开发板

```bash
RV1106_BOARD=root@192.168.9.107 \
  tools/deploy_guest_rv1106.sh
```

在开发板上运行：

```bash
cd /root/Flower/07_hello_deepseek
RV1106_PET_PORT=17890 ./pocket_host gif_demo.js gif_demo.pak 0
```

主机端会开启 `17890` 端口，接收桌宠动画命令。当前接口只适合局域网内
使用，暂未加入鉴权。

## 接入大模型

第一版将 API Key 放在电脑上的轻量 Web 服务中，开发板不保存 Key。先在
开发机启动：

```bash
LLM_API_KEY='你的 API Key' \
PET_DEVICE_URL='http://192.168.9.107:17890' \
bun tools/pet_server.ts
```

然后浏览器访问 `http://127.0.0.1:8080`。服务会把素材注册表提供给大模型，
要求大模型返回简短中文回复、动画 ID 和 `loop`/`once` 播放模式，再把动画
命令转发到开发板。

可通过环境变量切换兼容的 OpenAI 风格服务：

```bash
LLM_API_URL='https://api.deepseek.com/chat/completions'
LLM_MODEL='deepseek-chat'
```

## 外部配置

复制 `pet_config.example.json` 为 `pet_config.json` 后修改即可。开发板启动时
会读取其中的 `idleSeconds` 和 `responseMultiplier`，电脑端 AI 服务会读取
`device.url`、`ai` 和 `web.port`。`pet_config.json` 已加入忽略列表，不会被
提交到 Git。

```bash
cp pet_config.example.json pet_config.json
vim pet_config.json

# 开发板端：修改配置后只需重启 pocket_host
./pocket_host gif_demo.js gif_demo.pak 0

# 电脑端：不再需要把 Key 写进启动命令
NO_PROXY=192.168.9.107 no_proxy=192.168.9.107 bun tools/pet_server.ts
```

## 动画策略

- 待机动画在吃瓜、吃甜甜圈、墨镜装酷、扇风之间轮换，每个动画约保持 1 分钟。
- AI 请求开始时进入认真的思考、自信的思考或记录状态。
- AI 返回后播放回应动画，持续时间为该 GIF 原始时长的 3 倍，然后回到待机。
- 策略代码位于 `app/gif_demo/animation_manager.ts`，AI 状态通知位于
  `tools/pet_server.ts`。
