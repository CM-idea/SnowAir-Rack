# SnowAir-Rack

[SnowAir 轻雪](https://github.com/CM-idea/SnowAir) 的晾衣架插件：把截图、图片、便签、录屏和文件挂到屏幕顶端的一根晾衣绳上。

## 安装

先安装 [SnowAir 轻雪](https://github.com/CM-idea/SnowAir)，然后两种方式任选其一：

1. **从轻雪插件库安装**：打开轻雪 → 设置 → 插件集成，找到「晾衣架」卡片点下载，装好后重启轻雪。
2. **手动安装**：下载 `SnowAir-Rack.zip`（固定名，始终指向最新版；历史版本仍保留 `SnowAir-Rack-v*.zip`），解压后把其中的 `rack` 文件夹放到 `%AppData%\SnowAir\plugin\`，重启轻雪即可。便携模式则放到 `SnowAir.exe` 同目录的 `plugin\` 下。

## 功能

- 截图 / 图片上绳，点一下复制，拖出可直接粘贴
- 剪切板面板：分类筛选、自定义分类、文本/图片上绳；SVG 代码按轻雪历史里的渲染结果显示成图片，点一下复制的是 SVG 代码（粘贴到 Figma 是矢量）
- 文本 / SVG 拖拽：松手后静默复制并粘贴到前台；UIA/MSAA 能识别落点是输入框时先单击激活，识别不到就正常粘贴；图片/文件仍走原生文件拖拽
- 卡片可横向拖动改顺序，布局统一间距，不会叠在一起；拖回绳子或按 Esc 取消
- 拖到桌面时，同名同内容会把已有桌面图标移到落点；不同名同内容仍正常复制
- 轻雪「停用历史记录」打开时，篮子（历史入口）隐藏，信封右移到最右侧
- 便签卡片：正文存在轻雪的剪切板历史里（标签「便签」），双击编辑、右键打开/另存为 TXT；从绳上取下卡片不会删记录，轻雪剪切板历史里仍是一条带「便签」标签的普通文本

## 构建（开发）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

产物 `dist\rack.dll` 复制到 `%AppData%\SnowAir\plugin\rack\` 后重启轻雪。

发版用 `build.ps1 -Package`，会同时生成 `dist\SnowAir-Rack-v<版本>.zip` 和固定名的 `dist\SnowAir-Rack.zip`；两个都要上传到 Release，插件库下载只认固定名。

## 依赖

SnowAir 插件 ABI 4，接口见 [`include/SnowAirPlugin.h`](include/SnowAirPlugin.h)，与 [SnowAir 仓库](https://github.com/CM-idea/SnowAir) 保持同步。


## 社区

SnowAir 晾衣架由 Liuyiran 维护，欢迎通过 [Issue](https://github.com/CM-idea/SnowAir/issues) 反馈问题或提出建议。

## 鸣谢

感谢 [PicRope](https://github.com/Renly1994/PicRope) 及 [tendedero](https://github.com/alejandrobujan/tendedero) 的开源与分享。

## 许可证

Apache License 2.0

