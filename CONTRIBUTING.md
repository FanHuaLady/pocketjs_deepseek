# 参与开发

感谢你对 RV1106 PocketJS 桌宠项目的关注。

## 开发前准备

请先阅读 [`README.md`](README.md)，完成 Bun、PocketJS 依赖和 RV1106 SDK 配置。提交代码前不要把真实 API Key、设备配置或构建目录加入仓库。

## 修改建议

- 动画和交互状态机放在 `app/gif_demo/`。
- framebuffer、触摸、音频和 HTTP 宿主逻辑放在 `src/`。
- 电脑端终端程序放在 `tools/pet_server.ts`。
- 构建、打包和部署流程放在 `tools/`。
- 第三方代码只在确有必要时更新，并保留原有许可证文件。

## 本地检查

```bash
bash -n tools/*.sh
bun tools/prepare_gif_rv1106.ts
git diff --check
```

如果配置了 RV1106 SDK，再运行：

```bash
tools/build_guest_rv1106.sh
cmake --preset rv1106-release
cmake --build --preset rv1106-release
```

电脑端程序可以单独构建：

```bash
tools/build_pet_cli.sh
```

## 提交规范

- 一个提交只解决一个主题。
- 不提交 `pet_config.json`、API Key、设备日志和 `build/`。
- 修改运行协议或环境变量时同步更新 README。
- 修改动画资源时同时说明资源来源和许可证。
- 提交前确认开发板端和电脑端至少可以完成一次文字对话或语音对话。
