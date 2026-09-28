# RV1106 PocketJS 桌宠

这是一个运行在 RV1106 开发板上的 PocketJS 桌宠项目。

当前结构：

```text
开发板 RV1106
  ├─ pocket_host                 无参数启动
  ├─ gif_demo.js
  ├─ gif_demo.pak
  ├─ 屏幕、触摸、麦克风、扬声器
  └─ HTTP 控制端口 17890

电脑
  └─ pocket_pet
       ├─ 终端文字对话
       ├─ LLM 对话
       ├─ Qwen ASR 语音转文字
       ├─ Qwen TTS 文字转语音
       └─ 通过 HTTP 控制开发板
```

开发板不会保存 LLM、ASR 或 TTS 的 API Key。AI 服务运行在电脑端。

## 当前功能

- GIF 在构建阶段转换为 PocketJS PNG sprite atlas。
- 待机动画包含吃瓜、吃甜甜圈、墨镜装酷、扇风，每个动画默认保持 45 秒。
- AI 思考时轮换播放认真思考、自信思考、记录。
- AI 回复动画默认播放原始 GIF 时长的 2 倍。
- Qwen TTS 当前音色为 `Bella`。
- 屏幕触摸时显示白色边框。
- 屏幕中间横向区域约为 `15%` 到 `85%`：
  - 上半区：播放被摸头动画；动画结束后通知 AI，AI 回复时禁止再次使用摸头动画，默认可回退到 `love`。
  - 下半区：按住录音，松开结束录音，然后执行 ASR、LLM、TTS。
- 上位机是终端可执行程序，支持直接输入文字。
- 上位机不使用麦克风；语音输入只通过开发板触摸屏完成。
- 开发板的语音识别文本和 AI 文字回复会显示在上位机终端。

## 目录

```text
assets/original_gif/       原始 GIF 素材
assets/processed/          处理后的 PNG atlas 和素材清单
app/gif_demo/              PocketJS 桌宠入口和动画状态机
src/main.c                 RV1106 主循环、触摸分区、白边
src/runtime/pet_host.c     开发板 HTTP 控制、音频和事件队列
src/runtime/mic_host.c     麦克风录音
tools/pet_server.ts        电脑端终端交互程序源码
tools/build_pet_cli.sh     编译电脑端独立可执行文件
tools/build_pet_cli_windows.ps1
                           编译 Windows 版上位机
tools/package_rv1106.sh    打包开发板运行目录
tools/deploy_guest_rv1106.sh
                           上传并启动开发板程序
pet_config.example.json    不含真实 Key 的配置模板
pet_config.json            本地运行配置，已被 Git 忽略
```

## 电脑端启动

直接运行已经编译好的 Linux 版 `pocket_pet`：

```bash
./pocket_pet
```

程序启动后会进入终端对话：

```text
Pocket Pet 终端已启动
设备: http://192.168.9.107:17890
输入文字后按回车发送；开发板触摸屏下半区可进行语音对话；输入 Ctrl+C 退出。
```

输入文字并按回车即可发送。开发板触摸屏下半区按住说话、松开后，上位机会自动轮询录音并处理。

开发板控制接口可以这样检查：

```bash
curl --noproxy 192.168.9.107 \
  http://192.168.9.107:17890/event
```

没有待处理事件时返回 HTTP `204` 是正常现象。

## 编译电脑端程序

开发环境需要 Bun。执行：

```bash
tools/build_pet_cli.sh
```

输出文件：

```text
build/pocket_pet
```

这是包含 Bun 运行时的独立 Linux 可执行文件，用户不需要安装 Bun，也不需要打开浏览器。

编译 Windows 版：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_pet_cli_windows.ps1
```

输出文件：

```text
build\pocket_pet.exe
```

Windows 版默认显示终端窗口。电脑需要能够访问开发板的 `192.168.9.107`。程序会依次查找：

```text
1. PET_CONFIG 环境变量指定的路径
2. pocket_pet.exe 同目录下的 pet_config.json
3. 当前工作目录下的 pet_config.json
```

推荐将 `pocket_pet.exe` 和 `pet_config.json` 放在同一个目录。

## 开发板启动

登录开发板：

```bash
ssh root@192.168.9.107
```

启动已经部署好的版本：

```bash
cd /root/Flower/07_hello_deepseek
chmod +x pocket_host
./pocket_host
```

`pocket_host` 不带参数时，会自动读取可执行文件同目录下的 `gif_demo.js` 和 `gif_demo.pak`。因此开发板只需要复制下面三个文件：

```text
pocket_host
gif_demo.js
gif_demo.pak
```

也可以直接生成目录包：

```bash
tools/package_rv1106.sh
```

输出目录为 `build/rv1106-package/`，复制整个目录到开发板后执行：

```bash
cd build/rv1106-package
./pocket_host
```

检查进程：

```bash
ps | grep pocket_host | grep -v grep
```

停止程序：

```bash
start-stop-daemon -K \
  -p /tmp/pocket_host.pid \
  -x /root/Flower/07_hello_deepseek/pocket_host
```

开发板程序默认监听：

```text
http://192.168.9.107:17890
```

## 配置

本地首次配置：

```bash
cp pet_config.example.json pet_config.json
```

`pet_config.json` 不会提交到 Git。主要配置如下：

- `idleSeconds`: 待机动画切换间隔，当前为 `45`。
- `responseMultiplier`: AI 回应动画倍率，当前为 `2`。
- `device.url`: 开发板 HTTP 地址。
- `ai.apiUrl`、`ai.apiKey`、`ai.model`: LLM 配置。
- `ai.personaPrompt`: AI 性格和关系设定。
- `asr`: Qwen 语音识别配置。
- `tts.voice`: TTS 音色，当前为 `Bella`。

只修改 AI 性格、Key 或音色时：

```text
只需要重新启动 pocket_pet，不需要重新编译，也不需要重启开发板。
```

当前性格方向是：温柔、体贴、可靠的平等朋友，不是仆从或奴仆，不盲目服从，也不过度表现恋爱式依赖。

## 构建

首次准备依赖：

```bash
cd /home/flower/Echo-Mate/Flower/07_hello_deepseek
bun install

cd third_party/pocketjs
bun install
cd ../..
```

生成 GIF 处理资源、PocketJS guest 和 pak：

```bash
tools/build_guest_rv1106.sh
```

编译 RV1106 主机程序：

```bash
cmake --preset rv1106-release
cmake --build --preset rv1106-release
```

构建产物：

```text
build/guest/gif_demo.js
build/guest/gif_demo.pak
build/rv1106-release/pocket_host
build/rv1106-package/
build/pocket_pet
build/pocket_pet.exe
```

GIF 处理脚本：

```text
tools/prepare_gif_rv1106.ts
```

它会扫描 `assets/original_gif/*.gif`，生成 `assets/processed/`。当前处理策略保证 sprite cell 至少为 `64px`；点头、被摸头等细节较多的动画会使用 `128px` cell，避免被放大后发糊。

添加新 GIF 时，需要在 `tools/prepare_gif_rv1106.ts` 的 `names` 映射表中增加中文文件名和英文 ID。

## 重新部署

构建完成后，将以下文件复制到开发板目录：

```text
build/rv1106-package/pocket_host
build/rv1106-package/gif_demo.js
build/rv1106-package/gif_demo.pak
```

例如：

```bash
scp build/rv1106-package/pocket_host \
    build/rv1106-package/gif_demo.js \
    build/rv1106-package/gif_demo.pak \
    root@192.168.9.107:/root/Flower/07_hello_deepseek/
```

复制后停止旧程序，再按“开发板启动”章节执行 `./pocket_host`。

也可以使用部署脚本自动打包、上传、停止旧进程并启动新版本：

```bash
RV1106_BOARD=root@192.168.9.107 \
RV1106_REMOTE_DIR=/root/Flower/07_hello_deepseek \
tools/deploy_guest_rv1106.sh
```

开发板静音测试：

```bash
RV1106_AUDIO_VOLUME=0 ./pocket_host
```

`RV1106_AUDIO_VOLUME` 取值范围为 `0` 到 `1`。设置为 `0` 时仍会正常生成、传输和消费 TTS 音频，但扬声器不会发声；不设置时默认为 `1`。

## Git 注意事项

编译产物、`.pak`、`pocket_host`、构建目录和本地 `pet_config.json` 已加入 `.gitignore`。提交前检查：

```bash
git status --short
```

不要提交包含真实 API Key 的 `pet_config.json`。
