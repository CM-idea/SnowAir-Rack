# 设计说明

## 结构

- 分层窗口（`LayeredWindow`）绘制整根绳子，鼠标移开自动收起
- `RackView` 负责卡片布局、缩略图、便签绘制
- `ItemStore` 把卡片持久化到 `rack_items.json`，文件本身仍归 SnowAir 所有

## 便签

- 无文件路径、只有 `note` 文本的卡片
- 便签样式：黄色渐变、SVG 形状、文字超出裁剪
- 单击复制、双击编辑、右键菜单打开/另存为 TXT/销毁

## 剪切板

- 通过宿主 `entry_*` 接口读取历史与剪切板
- 分类存插件本地 `clip-categories.json`，标签仍存 SnowAir
