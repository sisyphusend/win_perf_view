# PerfOverlay

![License: Unlicense](https://img.shields.io/badge/License-Unlicense-blue.svg)
![Platform: Windows](https://img.shields.io/badge/platform-Windows%2010%2F11-blue.svg)
[![Latest Release](https://img.shields.io/github/v/release/sisyphusend/win_perf_view)](https://github.com/sisyphusend/win_perf_view/releases/latest)

[English](README.md) | **简体中文**

一个轻量的 Windows 置顶悬浮窗（HUD），实时显示 CPU、内存和每块 GPU 的使用率。

- 纯 Win32 + GDI + PDH（性能数据助手）+ DXGI —— 无 .NET、无 Electron、无注入。
- 单个源文件，无第三方依赖。
- 空闲时资源占用约 15MB 内存、~0% CPU（实测），每秒一个定时器节拍。
- GPU 使用率来自与任务管理器相同的 `GPU Engine` 性能计数器，因此无需任何
  厂商 SDK 即可支持 NVIDIA/AMD/Intel 显卡。
- **按名称分别显示每一块物理 GPU**（例如核显 + 独显的笔记本会各自显示为
  独立条目）—— 显卡名称来自 DXGI，引擎使用率计数器按 LUID 匹配回各适配器。
- 仪表盘风格：等宽字体，每行为 `标签 | 圆角进度条（带滑块式手柄）| 数值`，
  各行列对齐，统一强调色而非红黄绿阈值。可选实心圆角卡片背景，或完全透明
  的浮动文字/进度条 —— 菜单一键切换。
- 两种布局：**纵向**（多行堆叠）与**横向**（单行、细分隔符分隔），运行时
  可切换 —— 窗口随之缩放并始终保持完整在屏幕内。
- 记住一切：布局、背景、锁定状态、迷你图、热键、自动隐藏以及窗口位置都会
  在多次运行之间自动保存和恢复。
- 可选的每行历史迷你图（纵向布局）、免找托盘的全局热键（显示/隐藏、锁定/
  解锁）、以及全屏应用/游戏获得焦点时自动隐藏 —— 全部在一个原生设置窗口中
  配置（菜单 → Settings...）。

## 下载

不想自己编译？从
**[Releases](https://github.com/sisyphusend/win_perf_view/releases/latest)** 页面
下载最新构建 —— 下载 `PerfOverlay.exe` 直接运行，无需编译器。

> **首次运行时 Windows 很可能弹出警告**（"Windows 已保护你的电脑"），因为
> exe 没有代码签名。小型未签名免费软件出现这个提示很正常，不代表有什么
> 问题 —— 点击 **更多信息 → 仍要运行** 即可。（如果你不想信任未签名的
> 二进制文件，可以按下面的步骤从源码自行编译 —— 你可以逐行阅读自己正在
> 编译的每一行代码。）

## 环境要求

- Windows 10 或 11，64 位。
- 无需安装任何运行时依赖 —— 编译出的 exe 是自包含的。

## 编译

你需要一个 C++ 编译器，二选一：

**MSVC（Visual Studio Build Tools）** —— 如果你装了 Visual Studio 2022 但
没有 C++ 工作负载，通过 Visual Studio Installer 添加（"使用 C++ 的桌面
开发"），或安装独立的
[Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)。
然后运行：

```bash
build.bat
```

**MinGW-w64** —— 例如 [w64devkit](https://github.com/skeeto/w64devkit)
（便携版，解压后把 `bin` 加入 PATH 即可）或 MSYS2。然后运行：

```bash
build_mingw.bat
```

两者都会在本目录产出 `PerfOverlay.exe`。

## 运行

```bash
PerfOverlay.exe
```

首次运行时 HUD 出现在屏幕右上角（之后会在你上次放置的位置重新打开）。

- 在 HUD 上任意位置**拖拽**即可重新定位。
- **右键**点击 HUD 或托盘图标打开菜单：
  - **Hide / Show** —— 左键点击托盘图标可不开菜单直接切换。
  - **Horizontal Layout / Vertical Layout** —— 在多行堆叠与单行之间切换；
    窗口会调整大小并保持完整在屏幕内。
  - **Add Background / Remove Background** —— 开关实心卡片外观。
  - **Lock Position / Unlock Position** —— 锁定后 HUD 变为点击穿透
    （`WS_EX_TRANSPARENT`）：鼠标点击直接传给下层窗口。锁定期间无法再
    拖拽或右键 HUD 本体 —— 请用托盘图标解锁。
  - **Enable/Disable Start with Windows** —— 添加或移除指向本 exe 的用户级
    `HKCU\...\Run` 注册表项。无安装器、无计划任务。
  - **Settings...** —— 打开一个小型原生设置窗口（纯 Win32 复选框），包含
    上述所有开关，另有：
    - **Show history graphs** —— 每行一个小迷你图，显示最近约 40 秒的走势，
      仅纵向布局（横向太窄放不下）。
    - **Global hotkey Ctrl+Alt+O: Show/Hide** 与
      **Global hotkey Ctrl+Alt+L: Lock/Unlock** —— 在任何地方都有效，无需
      先找到托盘图标。
    - **Auto-hide when a game is fullscreen** —— 任何独占全屏/无边框全屏
      应用获得焦点时自动隐藏 HUD（通过检测前台窗口的矩形是否恰好覆盖整个
      显示器判断），应用失去焦点后自动恢复 —— 除非你自己另行手动隐藏过，
      那样会保持隐藏。
  - **Exit**。
- 它还会在通知区域（任务栏的"隐藏图标"折叠区）添加一个图标，即使 HUD 本体
  被隐藏或移出屏幕也能找到并控制它。
- 不会抢占当前应用/游戏的焦点（`WS_EX_NOACTIVATE`）。

## 设置

上述所有开关加上窗口位置，每次变更（以及退出时）都会写入
`HKCU\Software\PerfOverlay`，下次启动时重新加载 —— 上次怎么离开的，这次就
怎么打开。删除该注册表项即可把一切恢复为默认值。

## 工作原理

- **CPU** —— PDH 计数器 `\Processor Information(_Total)\% Processor Utility`，
  每秒采样一次。与任务管理器使用的是同一个计数器（不是旧的
  `% Processor Time` —— 那个在 CPU 处于降频省电或超频加速状态时，读数会
  远低于或远高于任务管理器）。
- **内存** —— `GlobalMemoryStatusEx`（`dwMemoryLoad`，以及以 GB 为单位的
  已用/总量）。
- **GPU** —— DXGI（`IDXGIFactory1::EnumAdapters1`）在启动时枚举每块物理
  适配器，获取其名称和 LUID。PDH 通配符展开
  `\GPU Engine(*)\Utilization Percentage` 得到引擎实例（这些实例随进程使用
  GPU 而增减 —— 实例列表每 5 秒重扫一次而非每秒，因为每秒重建整个 PDH
  查询实测约耗 8% CPU）。每个引擎实例的 LUID（内嵌在计数器路径中）被匹配
  回对应的物理适配器；每块适配器先取各引擎类型内的最大利用率，再取所有
  引擎类型的最大值作为该 GPU 的整体负载 —— 与任务管理器 GPU 图表的汇总
  方式相同。
- **透明** —— 窗口为分层窗口（`WS_EX_LAYERED`）+ 颜色键透明
  （`LWA_COLORKEY`）：先整体涂一层保留色，Windows 把这种颜色渲染成一个
  透出的"洞"，只有其后绘制的文字和进度条像素保持可见。文字使用灰度抗锯齿
  而非 ClearType，因为 ClearType 的次像素混合假设背景是已知的纯色，而这里
  的"背景"实际是透出的任意桌面内容，会产生彩色镶边。开启实心背景时，窗口
  还会被裁剪为圆角区域（`SetWindowRgn`），与内部绘制的圆角卡片吻合。
- **窗口尺寸** —— 两种布局都会在窗口创建或调整大小之前实测文字宽度（
  `GetTextExtentPoint32`），因此 GPU 名称永不被截断，卡片宽度也永远恰好
  够用。纵向布局中，标签列与数值列按所有行中最宽的标签/数值对齐，进度条
  因此整齐成列。
- **GPU 标签** —— 每行显示 `GPU 0 · <型号名>`、`GPU 1 · <型号名>` 等，
  即使两块适配器恰好重名也能区分。

## 备注

- 计数器名称按英文匹配；在非英文显示语言的 Windows 上，GPU 计数器对象名
  可能需要本地化（CPU 使用与区域设置无关的 `PdhAddEnglishCounterW`）。

## 许可

[Unlicense](LICENSE) —— 公共领域。任意使用、修改、分发，无需署名。

## 参考

Fork 自 [Vinodhariharan/PerfOverlay](https://github.com/Vinodhariharan/PerfOverlay)
—— 感谢原作者。本 fork 额外修复了 MSVC 编译问题（`build.bat` 缺少
`/std:c++17`，以及用 `NOMINMAX` 解决 `windows.h` 的 min/max 宏冲突）。
