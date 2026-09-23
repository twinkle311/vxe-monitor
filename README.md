<div align="center">
  <h1>vxe-monitor</h1>
  <p><b>Windows 任务栏托盘实时显示 VXE / ATK 无线鼠标电量</b></p>
  <p>纯 Win32 API 原生实现 · 免安装单文件 · 无需打开官方 Web 驱动</p>
  <p>当前版本:<b>V1.0.1</b></p>

  <p>
    <a href="#"><img src="https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-blue?style=flat-square&logo=windows" alt="Platform" /></a>
    <img src="https://img.shields.io/badge/Language-C%2B%2B17%20%2F%20Win32-00599C?style=flat-square&logo=c%2B%2B" alt="Language" />
    <img src="https://img.shields.io/badge/License-MIT-green?style=flat-square" alt="License" />
  </p>
</div>

---

## 💡 项目简介

**vxe-monitor** 是一个轻量级托盘小工具:在 Windows 任务栏托盘直接以数字图标显示 VXE/ATK 无线鼠标的剩余电量,不用打开浏览器访问官方 Web 驱动 [ATK HUB](https://v3-hub.atkgear.com/)。通过 HID 协议与鼠标接收器直接通信,与官方驱动同一命令通道。

设计参考 [rapoo-tray](https://github.com/Iris-0109/rapoo-tray)(雷柏鼠标托盘电量工具),托盘图标、OSD 悬浮与自启动实现改编自该项目。

---

## ✨ 主要功能

- 🔋 **托盘数字电量图标**:任务栏图标内嵌实时电量百分比,电量条分 3 档颜色预警(>20% 绿 / 11~20% 黄 / ≤10% 红),充电时显示横向 ⚡ 闪电图标(多边形绘制,完整不裁切)。
- 🖱️ **左键 OSD 悬浮提示**:单击托盘图标弹出电量 / 型号 / 连接方式 / 电压面板,1.6 秒后平滑淡出。
- 📋 **右键菜单详情**:电池电量、充放电状态、电池电压 (mV)、连接方式(2.4G 1K/2K/4K/8K、有线、Nearlink)、固件版本一目了然;"atk-hub" 一键打开官方 Web 驱动。
- 🚀 **开机自启动**:右键菜单一键切换(基于 `HKCU` 注册表,无需管理员权限)。
- ⚡ **极简轻量**:纯 Win32 API,单个 exe,无第三方运行时依赖。
- 🔌 **自动重连 / 有线切换**:接收器插拔、鼠标开关机自动感知;插 USB 充电时自动切换到有线直连通道,实时显示充电状态;鼠标休眠/关机充电(不连电脑)时醒则实时、睡则保留最后已知电量并标注"已离线"。
- ⏱️ **可控轮询**:启动立即查询,默认每 30 秒刷新;右键菜单"刷新间隔"可选 15 秒 ~ 1 小时(记住设置);也支持 `vxe-monitor.exe [秒数]` 命令行覆盖(5~3600 秒)。

---

## 🖱️ 支持设备

| 设备 | 连接方式 | VID | 实测状态 |
| :--- | :--- | :--- | :---: |
| **VXE R1SE 长续航版**(显示为 VXE R1SE+) | 2.4G 接收器 (`3554:F58E` 1K Dongle) | `0x3554` | ✅ 已实测 |
| VXE R1SE / R1SE+ / R1 系列 | 2.4G / 有线 | `0x3554` | ✅ 同协议 |
| 其他 COMPX 方案 VXE / ATK 鼠标 | 2.4G / 有线 | `0x3554` / `0x373B` | 🧪 理论通用,欢迎反馈 |

> 设备识别按 (VID + 命令通道集合) 动态匹配,不锁定具体 PID;型号名称由鼠标 CID/MID 查询获得。

---

## 📥 下载与使用

从 [Releases](../../releases) 下载 `vxe-monitor-v*-win64.zip`,解压得到:

1. **`vxe-monitor.exe`** — 托盘主程序,双击即可运行,右下角图标显示电量数字。
2. **`probe.exe`** — 协议诊断工具(控制台),用于排查设备识别/读取问题:
   ```cmd
   probe.exe          # 找命令通道并实测电量/型号/版本
   probe.exe --all    # 额外打印全部 HID 接口
   ```

> 💡 Windows 11 默认会把新托盘图标收进溢出区(▲ 隐藏图标),点开后把图标拖到任务栏即可常驻显示;也可在 `设置 → 个性化 → 任务栏 → 其他系统托盘图标` 中设为"始终显示"。

---

## 🔬 技术协议

命令经 Report ID 8 的 HID 报文收发 16 字节命令包(带校验和),与官方 Web 驱动同一命令通道;电量查询为命令 `0x04`,响应含电量百分比、充电状态(`1`=充电)与电池电压 mV(大端 u16)。协议逆向自 ATK HUB Web 驱动并在真机实测验证。

> 📖 完整字节级协议解析(HID 通道选取、命令表、CID/MID 型号对照、实测报文样例)整理于 `doc/protocol.md`——本地文档,按仓库约定不随代码发布。

---

## 🛠️ 源码编译

```
src/vxe_hid.h   HID + COMPX 协议层(设备枚举、命令包构造/解析、命令封装)
src/main.cpp    托盘程序(图标渲染 / OSD / 菜单 / 自启动 / 轮询线程)
tools/probe.cpp 协议诊断工具
```

### 一键脚本(推荐)

```cmd
build.bat
```

自动探测 `g++` (MinGW) 或 `cl.exe` (MSVC),产物输出到 `bin/`。

### CMake

```cmd
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

### CI / CD

推送 `main` 触发 [CI](.github/workflows/ci.yml) 构建;推送 `v*` 标签(如 `v1.0.0`)自动构建、打包并创建 GitHub Release([release.yml](.github/workflows/release.yml))。

---

## 🙏 致谢

- [rapoo-tray](https://github.com/Iris-0109/rapoo-tray) — 托盘数字图标、OSD 悬浮窗与自启动实现改编自该项目([MIT License](http://opensource.org/licenses/MIT), Copyright (c) Iris-0109)。
- [ATK HUB](https://v3-hub.atkgear.com/) — 协议逆向来源(仅用于互操作,与其无隶属关系)。

## ⚠️ 免责声明

1. 本项目为个人业余开源作品,**非 VXE / ATK 官方出品**;"VXE"、"ATK" 及相关产品名称的权利归其各自所有者。
2. 程序仅发送只读查询命令,不修改鼠标任何配置;按"原样"(AS IS)提供,不承担由此产生的任何责任。

## 📄 开源许可证

本项目基于 [MIT License](LICENSE) 协议开源。
