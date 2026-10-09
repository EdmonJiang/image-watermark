# 更新日志 · Changelog

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)。
This project adheres to [Semantic Versioning](https://semver.org/).

---

## [1.0.0] — 2026-10-09

首个正式版本 / First stable release.

### ✨ 功能 · Features

- **多格式支持**：读取与写出 PNG / JPEG / BMP / TIFF / GIF
- **六项可调参数**：水印文字、文字颜色、字号（10–200 px）、倾斜角度（-90°–90°）、填充密度（1–20 列）、透明度（1%–100%）
- **实时预览**：任意参数调整立即反映到预览区
- **高质量输出**：PNG / BMP / TIFF 逐字节完全无损；JPEG 以质量 100% 编码；全程不重采样
- **高 DPI 感知**：4K / 高缩放屏幕下界面与文字清晰
- **中文界面**、程序图标、绿色单文件

### 🔧 实现 · Implementation

- 解码使用 Windows Imaging Component (WIC)，统一转换为 32bpp BGRA
- 编码使用 GDI+ 平面 API（`GdipCreateBitmapFromScan0` + `GdipSaveImageToStream`）
- 文字遮罩由 GDI 渲染，经逆映射 + 双线性插值旋转后按 alpha 混合到原图
- 预览采用双缓冲绘制，参数输入做 150 ms 防抖，滑条仅在松手时重算，避免闪烁与卡顿

### 📦 构建 · Build

- 编译器：TDM-GCC 9.2 (tdm64-1)，MinGW-w64 亦兼容
- 优化：`-O2 -mwindows -municode`
- 依赖：仅系统库（comctl32 / gdi32 / gdiplus / ole32 / oleaut32 / uuid / comdlg32 / shlwapi）
- 产物：`watermark.exe`，463,274 字节，PE32+ x86-64
- SHA256：`a2669e2ea6c10eeb55e84a9d1371f92a7ff120c5dad808c7b10e16242b79466e`

### ⚠️ 已知限制 · Known Limitations

- 仅支持 Windows 10 / 11 x64
- JPEG 与 GIF 为有损格式：JPEG 即使质量 100% 仍存在轻微色度抽样损失；GIF 限 256 色
- 保存为 JPEG / BMP / GIF 时会丢弃透明通道（格式本身不支持 alpha）
- 水印为「烧录」式，保存后不可撤销，建议保留原图

---

## 版本说明 · Version Notes

- **[1.0.0]** — 2026-10-09 · 首个版本
