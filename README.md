# SnowAir-Rack

[SnowAir 轻雪](https://github.com/CM-idea/SnowAir) 的晾衣架插件：把截图、图片、便签、录屏和文件挂到屏幕顶端的一根晾衣绳上。

## 安装

先安装 [SnowAir 轻雪](https://github.com/CM-idea/SnowAir)，然后两种方式任选其一：

1. **从轻雪插件库安装**：打开轻雪 → 设置 → 插件集成，找到「晾衣架」卡片点下载，装好后重启轻雪。
2. **手动安装**：下载 `SnowAir-Rack-v*.zip`，解压后把其中的 `rack` 文件夹放到 `%AppData%\SnowAir\plugin\`，重启轻雪即可。便携模式则放到 `SnowAir.exe` 同目录的 `plugin\` 下。

## 功能

- 截图 / 图片上绳，点一下复制，拖出可直接粘贴
- 剪切板面板：分类筛选、自定义分类、文本/图片上绳
- 便签卡片：双击编辑、右键打开/另存为 TXT/销毁

## 构建（开发）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

产物 `dist\rack.dll` 复制到 `%AppData%\SnowAir\plugin\rack\` 后重启轻雪。

## 依赖

SnowAir 插件 ABI 3，接口见 [`include/SnowAirPlugin.h`](include/SnowAirPlugin.h)，与 [SnowAir 仓库](https://github.com/CM-idea/SnowAir) 保持同步。


## 社区

SnowAir 晾衣架由 Liuyiran 维护，欢迎通过 [Issue](https://github.com/CM-idea/SnowAir/issues) 反馈问题或提出建议。

## 鸣谢

感谢 [PicRope](https://github.com/Renly1994/PicRope) 及 [tendedero](https://github.com/alejandrobujan/tendedero) 的开源与分享。

## 许可证

Apache License 2.0

