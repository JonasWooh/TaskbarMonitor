<p align="center">
  <img src="docs/images/icon.png" width="96" alt="TaskbarMonitor 图标">
</p>

<h1 align="center">TaskbarMonitor</h1>

<p align="center">
  一个嵌在 <b>Windows 11 任务栏里</b>、紧贴通知区左侧的原生性能监控小工具。<br>
  <a href="README.md">English</a> | <b>简体中文</b>
</p>

![任务栏上的指标条](docs/images/strip.png)

TaskbarMonitor 在托盘 `^` 按钮左侧的空白处显示 CPU、GPU、内存和网速，每秒刷新。左键点击会弹出 Win11 风格的详情面板，带 60 秒历史图；点网速那一块可以看各个程序分别占了多少网速。

它是一个约 115 KB 的单文件 exe，纯 C++/Win32 实现。平时约占整颗 CPU 的 0.006%，内存只有几 MB。

## 功能

- **任务栏指标条**：
  - CPU：利用率、频率、封装温度、功耗；
  - GPU：利用率、频率、温度、整卡功耗；
  - 内存：占用率；
  - 网络：上传、下载速率。
  
  每一项都能单独开关，指标条按显示内容自动收窄。
- **详情面板（左键）**：双栏显示 CPU（含每个逻辑处理器的负载）、GPU（含显存）、内存（含已提交）、磁盘读写和网络，每项都带 60 秒历史图。面板跟随系统深浅色和强调色。
- **按进程统计网速（点网速区域）**：按程序列出下载和上传排行，同一程序的多个进程会合并显示，例如 `chrome.exe (5)`。
- **主动让位**：任务栏按钮挤满时，按 GPU → 内存 → 网络 → CPU 的顺序逐组收起，放不下就整体隐藏，空间够了再自动恢复。任务栏左对齐时，"小组件"按钮也会被避让。
- **托盘图标**：左键打开面板，右键菜单里有"退出"。即使指标条被收起，也随时能操作。
- **自适应**：跟随深浅色、DPI、托盘宽度变化，Explorer 重启后会自动重新挂载。
- **开机自启**：在管理员模式下勾选，会创建"最高权限"的登录计划任务，开机后直接显示温度和功耗，不弹 UAC。

| 总览（浅色） | 总览（深色） |
|---|---|
| ![浅色详情面板](docs/images/panel-overview.png) | ![深色详情面板](docs/images/panel-overview-dark.png) |

| 按程序的网络活动 | 显示项目 |
|---|---|
| ![网络进程排行](docs/images/panel-network.png) | ![显示项目设置](docs/images/settings.png) |

任务栏空间不够时，指标条会收起，不会盖住任务栏按钮。上图是正常状态；下图多开了 17 个窗口，GPU 组已经收起。

![任务栏挤满时自动收起](docs/images/taskbar-crowded.png)

## 系统要求

- Windows 11 x64（指标条挂在主显示器的任务栏上）
- 可选，**CPU 温度和功耗**：需要安装 [PawnIO](https://pawnio.eu/) 驱动，并以管理员身份运行。目前仅支持 Intel CPU。
- 可选，**按进程统计网速**：需要以管理员身份运行
- GPU 指标：NVIDIA 显卡和较新的驱动（使用驱动自带的 NVML）

## 使用

1. 从 [`release/`](release/) 或 Releases 页面下载 `TaskbarMonitor.exe`，不需要安装。
2. 运行后，指标条会出现在托盘 `^` 按钮左侧。
3. 想看 CPU 温度和功耗：
   1. 安装 [PawnIO](https://github.com/namazso/PawnIO.Setup/releases/latest)；
   2. 右键指标条，选"以管理员身份重启"；
   3. 在管理员模式下勾选"开机自动启动"，以后开机就直接以这种方式运行，不再弹提示。

| 操作 | 效果 |
|---|---|
| 左键点指标条 | 打开详情面板；点外面或按 Esc 关闭 |
| 左键点网速区域 | 打开网络活动面板 |
| 右键指标条或托盘图标 | 菜单：性能详情、任务管理器、显示项目、开机自动启动、退出 |
| `TaskbarMonitor.exe /show` | 切换正在运行实例的详情面板（可以绑定到快捷键） |
| `TaskbarMonitor.exe /show network` | 切换网络面板 |
| `TaskbarMonitor.exe /replace` | 用这个 exe 接管正在运行的实例（管理员权限的也可以） |

显示项目的选择保存在 `HKCU\Software\TaskbarMonitor`。

## 资源占用

在 i9-13900KF / RTX 5080、Windows 11 26200 上实测：

| 场景 | CPU（单核占比 / 整颗 CPU 占比） |
|---|---|
| 平时（面板关闭） | 约 0.18% / 0.006% |
| 详情面板打开 | 约 0.8% / 0.02% |
| 网络面板打开且约 40 MB/s 满速下载 | 单核约 0–3%，仅在面板打开期间 |

- **内存**：任务管理器"内存"列（专用工作集）约 2–9 MB。提交大小约 29–37 MB，其中约 20 MB 是 NVIDIA 的 `nvml.dll` 预留的，基本不占物理内存。
- **泄漏测试**：详情面板和网络面板交替开关 200 次（含 100 次 ETW 会话启停）。首次打开之后，提交内存一直在 36.3–36.7 MB，句柄 478、GDI 对象 18、USER 对象 26 始终不变，结束后也没有残留的 ETW 会话。

## 实现原理

- **定位**：Windows 11 取消了任务栏工具栏（DeskBand），所以指标条是挂在 `Shell_TrayWnd` 下的分层子窗口，按 `TrayNotifyWnd` 的实际位置对齐。思路与 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 相同，也借鉴了它对触屏设备的修正：触屏设备上任务栏窗口比可见区域高。
- **拥挤检测**：Win11 的任务栏按钮是没有窗口句柄的 XAML 元素，`MSTaskSwWClass` 的位置已经和它们对不上，所以通过 UI 自动化读取 `TaskbarFrame` 子元素的真实边界。查询放在采样线程（COM MTA）里，只在以下情况触发：shell hook 报告窗口新建或关闭、设置或显示变化、托盘宽度变化，另外每 30 秒兜底刷新一次。
- **传感器**：
  - **CPU 负载和频率**：PDH，与任务管理器使用的计数器相同。
  - **CPU 温度和功耗**：通过仍在维护、已签名的 [PawnIO](https://pawnio.eu/) 驱动读取 `IA32_PACKAGE_THERM_STATUS` 和 RAPL `PKG_ENERGY_STATUS`，不用会被 Defender 报毒的 WinRing0。
  - **GPU**：NVML。
  - **网络合计**：`GetIfTable2`，只统计物理网卡，避免过滤驱动和虚拟交换机重复计数。
  - **磁盘**：PDH `PhysicalDisk(_Total)`。
- **按进程网速**：`Microsoft-Windows-Kernel-Network` 的私有实时 ETW 会话，与资源监视器同源。在内核侧按事件 ID 过滤，跳过环回流量，只在网络面板打开时运行。
- **绘制**：只用 GDI，不加载 Direct2D 或 XAML。
  - 指标条按实际显示的明暗极性绘制灰度抗锯齿文字，再转换为逐像素 Alpha，通过 `UpdateLayeredWindow` 输出，字体与系统时钟相同（Segoe UI Variable）。
  - 面板用双缓冲自绘：ClearType 文字、柱状图，以及 DWM 圆角和阴影。
- **线程**：UI 线程只负责绘制。它的窗口与 Explorer 共享输入队列，所以采样、UI 自动化、`schtasks`、UAC 这类耗时操作都放在别的线程。

## 构建

需要 [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)（`winget install MartinStorsjo.LLVM-MinGW.UCRT`），不需要 Visual Studio。

```powershell
.\build.ps1      # -> bin\TaskbarMonitor.exe
.\package.ps1    # -> release\TaskbarMonitor.exe 和 release\TaskbarMonitor-<版本>-win-x64.zip
```

`res\make_icon.ps1` 会重新生成 `res\app.ico`。每个尺寸单独绘制，所以 16–24 像素的小图标也清晰。

## 目录结构

```
├─ src/                    C++ 源码
│  ├─ main.cpp             任务栏指标条、托盘图标、菜单、设置窗口、采样线程
│  ├─ panel.cpp            详情 / 网络面板
│  ├─ metrics.cpp          PDH、NVML、内存、网络、PawnIO 传感器
│  ├─ netproc.cpp          按进程网速（ETW）
│  ├─ tbspace.cpp          任务栏占用检测（UI 自动化）
│  └─ pawnio.cpp、format.cpp、*.h
├─ res/                    图标、清单、资源脚本、图标生成脚本
├─ third_party/
│  └─ pawnio-modules/      已签名的 IntelMSR 模块（LGPL-2.1）及其许可证
├─ docs/images/            README 用的截图
├─ release/                预编译的 exe 和 zip
├─ build.ps1、package.ps1
└─ README.md、README.zh-CN.md、CHANGELOG.md、LICENSE
```

## 已知限制

- 只挂在主显示器的任务栏上。
- CPU 温度和功耗目前只支持 Intel。
- GPU 指标需要 NVIDIA 显卡。
- 界面只有简体中文。

## 致谢

- [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)：验证了在 Win11 任务栏内显示的可行性。
- [PawnIO](https://pawnio.eu/) 与 [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules)（作者 namazso）。

## 许可证

TaskbarMonitor 源代码使用 [MIT](LICENSE) 许可证。[`third_party/pawnio-modules`](third_party/pawnio-modules) 中内嵌的 PawnIO 模块使用 LGPL-2.1-or-later。
