# Changelog

## 1.2.0 — 2026-10-03

- English and Simplified Chinese UI: follows the Windows display language, or pick one via right-click → Language / 语言; switches live and is remembered.
- `/lang en|zh|auto` command-line switch.

## 1.1.0 — 2026-10-03

- Details flyout (left click): CPU with per-logical-processor load, GPU with VRAM, memory with commit charge, disk read/write and network, each with 60-second history; light/dark theme and accent color.
- Per-process network view (click the network figures), based on Kernel-Network ETW. Runs only while open; needs administrator rights.
- The strip collapses group by group, and finally hides, when task buttons need the room. Taskbar layout is read via UI Automation and re-checked on shell-hook events instead of every second.
- Notification-area icon (left click: flyout, right click: menu with Exit). The flyout shows a taskbar button while open.
- Application icon, version 1.1.0, `package.ps1` release packaging.
- `/show [network]` and `/replace` command-line switches.
- Removed the hover tooltip, since the flyout covers it.

## 1.0.0 — 2026-10-02

- Metrics strip inside the Windows 11 taskbar, left of the notification area: CPU utilization, clock, temperature and power; GPU utilization, clock, temperature and power; RAM; upload/download.
- CPU temperature and power via the PawnIO driver (Intel), GPU via NVML.
- Per-item display selection, start with Windows (elevated logon task), light/dark taskbar support.
