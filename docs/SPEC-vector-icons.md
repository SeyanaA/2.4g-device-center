# SPEC · 矢量图标与抗锯齿绘制

> 状态：v1.0.9 开发中（未发布）。关联：`SPEC-ui-shell-motion.md`、`SPEC-mouse-test.md`。

## 1. 背景

v1.0.9 的界面里，图标与圆角来自三条不同路径：

| 来源 | 实现 | 问题 |
| --- | --- | --- |
| 卡片/胶囊/徽章圆角 | `uitheme::FillRounded` → GDI `RoundRect` | 角部 1px 阶梯，缩放后更明显 |
| 测试页点击速度环 | GDI `Arc()` + `PS_SOLID` 画笔 | 圆弧内外缘明显锯齿 |
| 导航「日志/测试」 | GDI `MoveToEx/LineTo/Ellipse` | 20px 下线条发虚、比例粗糙 |
| 托盘图标 | 自写逐像素光栅器（含 5x7 点阵数字） | 16/32px 下边缘硬、数字发毛 |
| 充电闪电 | GDI `Polygon` | 斜边锯齿 |
| 设备类型图形 | GDI+ 路径（已抗锯齿） | 唯一达标的一类 |

结论：问题不在“图片分辨率低”，而在**绘制没有抗锯齿、几何用整数像素硬边**。

## 2. 方案

新增 `src/svg_icon.{h,cpp}`：

- 图标以 **SVG path `d` 字符串**为唯一数据源，viewBox 固定 `0 0 24 24`。
- 运行时解析为 `Gdiplus::GraphicsPath`，在目标矩形内等比缩放居中，用
  `SmoothingModeAntiAlias` + `PixelOffsetModeHalf` 渲染。
- 支持 path 子集：`M L H V C S Q T A Z`（含相对指令）。不支持的指令跳过，
  不崩溃、不吞掉后续指令。
- 线宽以 **viewBox 单位**给出（默认 1.8），随目标矩形缩放，因此 100%/125%/150%
  下视觉粗细一致。

同时把既有的非抗锯齿绘制替换掉：

- `uitheme::FillRounded` → `svgicon::FillRoundRect`（所有卡片、徽章、导航胶囊、
  滑杆轨道因此统一获得抗锯齿）。
- 测试页点击速度环 → `svgicon::DrawRing`。
- 导航图标、设备类型图形 → `svgicon::Draw`。
- 充电闪电 → `svgicon::FillPolygon`。
- 托盘图标 → 4x 超采样后 `HighQualityBicubic` 缩小到目标尺寸。

## 3. 约束

1. **不引入第三方 SVG 库**：目标是单文件静态 exe，不新增构建依赖。
2. **不新增位图资源**：`resource.rc` 不增加 PNG/ICO 图标；托盘 ICO 仍由代码生成。
3. **文本仍走 GDI**：`DrawTextW` + ClearType 的正文渲染质量优于 GDI+，
   vector 层只负责图形，不接管文字。
4. **每帧 GDI+ 对象只在函数作用域内创建**：避免与同 DC 上的 GDI 调用产生
   状态/顺序问题；如后续出现性能问题，再评估共享 `Graphics` 上下文。
5. **颜色仍由 `ui_theme` 提供**：vector 层不定义主题色。

## 4. 验收

- 100% / 125% / 150% 缩放下，卡片圆角、导航图标、点击速度环、托盘图标
  均无可见阶梯；截图对比不应出现 1px 硬边。
- 导航三个图标在 20px 下可辨识，线宽与文字视觉重量匹配。
- 点击速度环的起止端为圆头，0% 时只有轨道，100% 时闭合无缺口。
- 托盘图标在 16/32px 下数字与圆环边缘平滑，且不模糊到不可读。
- `pnpm` 无关；本项目验证入口为 `build.ps1` 与 10+1 个测试套件。
- 新增 `tests/test_svg_icon.cpp` 覆盖 path 解析（绝对/相对、曲线、圆弧、
  非法指令容错）。

## 5. 非目标

- 不重绘 Windows 原生控件（按钮、下拉框、滚动条、ListView）。
- 不改主题配色、字号与布局。
- 不做图标动画。
