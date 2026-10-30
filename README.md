# SnowAir-Rack

SnowAir 轻雪的晾衣架插件：把截图、图片和便签挂到屏幕顶端的一根细线上。

## 安装

1. 安装 [SnowAir 轻雪](https://github.com/CM-idea/SnowAir)；
2. 在轻雪的插件仓库里找到「晾衣架」并安装启用。

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

SnowAir 插件 ABI 3，接口见 `include/SnowAirPlugin.h`。


## 鸣谢

感谢 [PicRope](https://github.com/Renly1994/PicRope) 及 [tendedero](https://github.com/alejandrobujan/tendedero) 的开源与分享。

## 许可证

Apache License 2.0

