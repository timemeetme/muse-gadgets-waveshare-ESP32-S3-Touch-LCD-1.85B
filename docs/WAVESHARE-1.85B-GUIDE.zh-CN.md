# Waveshare ESP32-S3-Touch-LCD-1.85B 固件刷机与使用指南（中文）

本仓库基于 Meta 开源的 [Muse Gadgets](https://gadgets.muse.ai/) ESP32 Device SDK
（[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)），
为 **微雪 Waveshare ESP32-S3-Touch-LCD-1.85B** 圆屏开发板增加了官方板级支持。
刷入本固件后，这块 1.85 英寸圆形触控手表板就变成一个 Muse 智能小硬件（gadget）：
完整的屏幕头像 UI、按住说话的语音对话、触屏设置、图片显示、电池电量上报和 OTA 升级。

- 硬件：[Waveshare 产品页](https://www.waveshare.com/esp32-s3-touch-lcd-1-85b.htm) ·
  [Waveshare Wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85B)（逐步完善中）
- 上游协议与功能文档：[esp32/README.md](../esp32/README.md)、[esp32/AGENTS.md](../esp32/AGENTS.md)
- 板卡条目（英文）：[esp32/devices/README.md](../esp32/devices/README.md)

---

## 目录

1. [这是什么 / 与 1.85C 的区别](#1-这是什么--与-185c-的区别)
2. [准备工作](#2-准备工作)
3. [安装 ESP-IDF v6.0.1（仅支持此版本）](#3-安装-esp-idf-v601仅支持此版本)
4. [获取源码并配置 SDK Token](#4-获取源码并配置-sdk-token)
5. [编译固件](#5-编译固件)
6. [备份原厂固件（强烈建议）](#6-备份原厂固件强烈建议)
7. [刷机](#7-刷机)
8. [查看运行日志（串口监视）](#8-查看运行日志串口监视)
9. [首次开机与配对](#9-首次开机与配对)
10. [日常使用](#10-日常使用)
11. [恢复原厂固件 / 恢复出厂设置](#11-恢复原厂固件--恢复出厂设置)
12. [常见问题](#12-常见问题)

---

## 1. 这是什么 / 与 1.85C 的区别

1.85B 与已获上游支持的 [1.85C](https://www.waveshare.com/esp32-s3-touch-lcd-1-85c.htm)
是同系列圆屏手表板，屏幕、触摸、音频部分完全相同；区别集中在 IO 扩展与电池检测：

| 部件 | 1.85C | 1.85B |
|---|---|---|
| 圆屏 360×360 ST77916（QSPI） | 相同 | 相同 |
| 触摸 CST816S | 相同 | 相同 |
| 音频 ES8311（扬声器）+ ES7210（双麦克风） | 相同 | 相同 |
| 屏幕复位 | TCA9554 扩展芯片 EXIO2 | **直连 GPIO3** |
| 触摸复位 | TCA9554 EXIO1 | **直连 GPIO1** |
| 功放使能 | GPIO15 | **GPIO9** |
| 电池检测 | GPIO8 ADC 电阻分压 | **BQ27220 电量计（I2C，地址 0x55）** |
| TCA9554 IO 扩展芯片 | 有 | **无** |
| Flash / PSRAM | 16 MB / 8 MB 八线 | 相同 |
| USB | 芯片原生 USB Serial/JTAG（`303a:1001`） | 相同 |

本移植的固件改动全部在：

- 板级驱动：[`esp32/components/muse/boards/board_waveshare_s3_185b.c`](../esp32/components/muse/boards/board_waveshare_s3_185b.c)
- 屏幕厂商初始化序列：[`esp32/components/muse/boards/waveshare_s3_185b/st77916_init_185b.h`](../esp32/components/muse/boards/waveshare_s3_185b/st77916_init_185b.h)
- 编译配置覆盖层：[`esp32/devices/sdkconfig.muse-waveshare-s3-185b`](../esp32/devices/sdkconfig.muse-waveshare-s3-185b)
- 接线（Kconfig / CMake / 组件规则 / `board.sh` / `ports.py` / `avatar.py`）与文档表格。

电池读数来自 BQ27220 的标准命令：电压 `0x08`、瞬时电流 `0x0C`（正=充电）、
相对电量 `0x2C`。电量计由电池供电，**未装电池时读数返回 `-1`**，属正常降级，不影响其他功能。

## 2. 准备工作

**硬件**

- Waveshare ESP32-S3-Touch-LCD-1.85B 一块
- 一根 **USB 数据线**（很多 USB 线只能充电，不能传数据；刷机必须走数据线）
- 电脑（Windows 10/11 为主，macOS / Linux 命令见各节附注）

**账号与 App**

1. 在 [gadgets.muse.ai](https://gadgets.muse.ai/) 注册/登录 Muse 账号，
   进入 **Account → SDK tokens**（<https://gadgets.muse.ai/settings/sdk-tokens>），
   创建一个 token，形如 `mgst_...`。**每个 gadget 配对都需要 token。**
   并阅读 [Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms)。
2. 手机安装 **Muse App**（iOS / Android），在 **Settings（设置）→ Devices（设备）**
   里先打开 **Developer mode（开发者模式）**，之后才能看到社区设备。

**电脑软件**

- Python 3.9+（命令行能运行 `python`）
- Git
- ESP-IDF **v6.0.1**（下一节安装，其他版本不受支持）

## 3. 安装 ESP-IDF v6.0.1（仅支持此版本）

本固件只在 ESP-IDF **v6.0.1** 下编译。若提示找不到 `gcm.h`、`gpio_ll.h` 等头文件，
几乎都是 IDF 版本不对。

### Windows

方式一（命令行，推荐）：

```powershell
# 1) 克隆 IDF（浅克隆 + 子模块，需要一些时间）
git clone --depth 1 --recursive -b v6.0.1 https://github.com/espressif/esp-idf.git D:\esp\esp-idf-v6.0.1

# 2) 安装 xtensa-esp32s3 工具链（只需一次；会下载编译器、cmake、ninja 等）
cd D:\esp\esp-idf-v6.0.1
install.bat esp32s3
```

方式二：使用 [ESP-IDF Windows 安装器](https://dl.espressif.com/dl/esp-idf/)，
在版本列表里选 **v6.0.1** 安装。

之后**每个新开的终端**在编译/刷机前先激活环境：

```powershell
D:\esp\esp-idf-v6.0.1\export.bat
```

> `export.bat` 是批处理，在 PowerShell 里直接调用不会保留环境变量。
> 请在 **cmd** 里运行它，或用 PowerShell 的 `& D:\esp\esp-idf-v6.0.1\export.ps1`（如带），
> 或者像第 5 节那样把激活和编译放进同一条 cmd 命令。

### macOS / Linux

```sh
git clone --depth 1 --recursive -b v6.0.1 https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.1
cd ~/esp/esp-idf-v6.0.1 && ./install.sh esp32s3
. ~/esp/esp-idf-v6.0.1/export.sh
```

## 4. 获取源码并配置 SDK Token

```powershell
git clone https://github.com/timemeetme/muse-gadgets-waveshare-ESP32-S3-Touch-LCD-1.85B.git
cd muse-gadgets-waveshare-ESP32-S3-Touch-LCD-1.85B\esp32
```

每个应用构建都需要你自己的 SDK token（`mgst_...`）。token 写在**构建目录的
sdkconfig** 里（该文件被 Git 忽略，不会被提交；也**不要**把它写进任何 overlay）。

流程：先编译一次生成 `build-muse-waveshare-s3-185b/sdkconfig`，再填 token，重新编译：

```powershell
# （先完成第 5 节的第一次编译，然后回到这里）
notepad build-muse-waveshare-s3-185b\sdkconfig
```

找到（或追加）这一行并填入你的 token：

```ini
CONFIG_GADGET_SDK_TOKEN="mgst_你的token"
```

保存后重新执行第 5 节的编译命令。没有 token 也能编译（会有警告），但设备在
Muse 强制校验 token 后将无法配对。

## 5. 编译固件

在仓库的 `esp32` 目录下。

### 方式一：board.sh 一条龙（需要 bash；macOS / Linux / Git Bash）

```sh
tools/muse/board.sh build 185b
```

`board.sh` 会自动设置目标芯片与三层配置
（`sdkconfig.defaults` → `devices/sdkconfig.muse` → `devices/sdkconfig.muse-waveshare-s3-185b`），
在独立的 `build-muse-waveshare-s3-185b/` 目录里构建。
它按 `~/.espressif/esp-idf-v6.0.1` → `IDF_PATH` → `IDF_EXPORT` 的顺序寻找 ESP-IDF。

### 方式二：idf.py 直接构建（Windows 无 bash 时推荐）

把 IDF 激活和构建放进同一条 cmd 命令（PowerShell 中执行）：

```powershell
cmd /c "call D:\esp\esp-idf-v6.0.1\export.bat && idf.py -B build-muse-waveshare-s3-185b -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-waveshare-s3-185b/sdkconfig -DSDKCONFIG_DEFAULTS=""sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-185b"" build"
```

（macOS / Linux 上等价于：

```sh
idf.py -B build-muse-waveshare-s3-185b -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-waveshare-s3-185b/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-185b" build
```

）

第一次构建会由组件管理器自动下载依赖到 `managed_components/`，需要联网，耗时较长。
构建成功后产物在 `build-muse-waveshare-s3-185b/muse-gadget.bin`。

**验证**：构建日志里的 `check_sizes` 行显示各分区未超限即为通过；
另外可以确认生成的 `build-muse-waveshare-s3-185b/sdkconfig` 里
`CONFIG_MUSE_BOARD_WAVESHARE_S3_185B=y`、`CONFIG_IDF_TARGET="esp32s3"`、
Flash 为 16MB。

> 注意：`sdkconfig` 一旦生成，之后再改 overlay 不会自动生效。改了 overlay 或
> `sdkconfig.defaults` 后，删除 `build-muse-waveshare-s3-185b/sdkconfig`
> （或整个构建目录）再重新编译。

## 6. 备份原厂固件（强烈建议）

刷机会覆盖原厂出厂固件。第一次刷之前先把整颗 16 MB flash 备份下来，
并**保存在 Git 仓库之外**：

USB 连接开发板，在设备管理器中确认端口号（本板是芯片原生 USB，
显示为「USB 串行设备(COM x)」；下文以 `COM7` 为例，请换成你的端口）：

```powershell
python -m esptool --chip esp32s3 -p COM7 -b 460800 read-flash 0 0x1000000 waveshare-s3-185b-backup.bin
```

备份文件约 16 MB，妥善保存。

## 7. 刷机

### 方式一：idf.py（最简单）

```powershell
cmd /c "call D:\esp\esp-idf-v6.0.1\export.bat && idf.py -B build-muse-waveshare-s3-185b -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-waveshare-s3-185b/sdkconfig -DSDKCONFIG_DEFAULTS=""sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-185b"" -p COM7 flash"
```

### 方式二：esptool + flash_args（等价）

```powershell
cd build-muse-waveshare-s3-185b
python -m esptool --chip esp32s3 -p COM7 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
cd ..
```

### 方式三：board.sh（bash）

```sh
tools/muse/board.sh flash 185b        # 不带端口时自动按 USB 设备识别串口
tools/muse/board.sh flash 185b COM7   # 或显式指定
```

**说明**

- 刷机写入 bootloader、分区表、otadata、phy_init 和应用程序，**不动 NVS**——
  重刷升级不会丢配对信息和 Wi-Fi 凭据。
- `idf.py -p COM7 erase-flash` 会清空全部闪存（包括配对），只在想彻底重来时使用。
- 进不了 bootloader（一直 `Connecting...`）：**按住 BOOT 键，点按一下 RESET，松开 BOOT**，
  再刷。刷完按一次 RESET 或重新上电运行固件。
- 固件使用仓库自带的开发签名密钥，未启用 Secure Boot，不会烧 eFuse；
  请勿在还想反复刷机的板子上开启 Secure Boot / flash 加密。

## 8. 查看运行日志（串口监视）

```powershell
cmd /c "call D:\esp\esp-idf-v6.0.1\export.bat && idf.py -B build-muse-waveshare-s3-185b -p COM7 monitor"
```

`Ctrl-]` 退出。健康启动的标志是日志里出现：

```
link.main: Muse Gadget starting
```

随后还有一行 `muse: board: Waveshare ESP32-S3-Touch-LCD-1.85B`。
如需非交互抓取（自动复位后采集若干秒）：

```sh
python tools/muse/monitor.py COM7 10
```

## 9. 首次开机与配对

1. 刷机完成后固件自动运行，屏幕点亮并显示头像 UI。未配对时屏幕上会显示
   设备名 **`MuseGadget-XXXXXX`**（XXXXXX 为设备 MAC 尾号），方便你在 App 里辨认。
2. 打开手机 **Muse App → Settings → Devices**（确认 Developer mode 已开），
   扫描并选择该 `MuseGadget-XXXXXX` 设备。
3. App 通过 BLE 给设备配网（选你的 Wi-Fi、输入密码）并完成配对。
   当 App 要求**在设备上确认**时，**短按一下 BOOT 键**确认配对。
4. 配对成功后设备加入 Wi-Fi，与 Muse 建立加密会话，屏幕显示绿色/已连接状态，
   设备名消失，麦克风图标出现。

**BOOT 键的两个功能**：

- **短按**：确认待处理的配对；若配置未完成，重新打开设置窗口
- **长按 5 秒**：重置设置（解除配对并忘记 Wi-Fi）

**状态颜色**（本板显示在屏幕状态区/边缘）：

| 颜色 | 含义 |
|---|---|
| 橙色，呼吸 | BLE 广播中，等待配网 |
| 蓝色，呼吸 | 配对需要设备按键确认 |
| 蓝色 | Wi-Fi / 会话建立中 |
| 绿色 | 已连接 |
| 黄色，闪烁 | 会话切换或连接断开 |
| 紫色 | 未配对 |
| 红色，闪烁 | 出错 |

本设备使用 community pairing v5，手机 App 需支持 v5 并显示社区设备。

## 10. 日常使用

- **语音对话（按住说话）**：手指**按住屏幕上的头像**约半秒即开始录音，
  松开发送；或按住 **BOOT** 键作为 push-to-talk。Muse 转文字作答，回复可从
  扬声器播放（TTS）并在屏幕显示。
- **触屏设置**：屏幕上点开设置界面，可调亮度等（设置项随固件版本演进，
  以屏幕实际显示为准）。
- **图片显示**：Muse 可以向设备发送图片，直接显示在圆屏上。
- **电池电量**：Muse 会读取 BQ27220 电量计的电压/百分比/充电状态
  （`device.health`）。插 USB 充电时状态为「充电中」。
- **OTA 升级**：本板默认开启 OTA（Muse 可远程推送固件更新）。
  本地重新刷机（第 7 节）随时可用，且不影响配对。
- **待机**：电池供电、息屏且无语音活动时设备进入低功耗休眠，
  按 BOOT 或触摸唤醒；USB 供电时全速运行。
- **更换头像**：接好串口、设备已联网配对后，在 `esp32` 目录运行
  `python tools/muse/avatar.py`，按提示让 Muse 为你绘制专属头像并自动刷入。

## 11. 恢复原厂固件 / 恢复出厂设置

- **恢复原厂固件**：用第 6 节的备份原样写回：

  ```powershell
  python -m esptool --chip esp32s3 -p COM7 -b 460800 write-flash 0 waveshare-s3-185b-backup.bin
  ```

- **只解除配对/清除 Wi-Fi**：BOOT 长按 5 秒，或 `idf.py -p COM7 erase-flash`
  （后者全清，包括配对、Wi-Fi 与 NVS 数据，之后需重新走配对流程）。

## 12. 常见问题

**Q：`python -m esptool` 连不上板子？**
用 USB 数据线（不是充电线）；设备管理器里确认端口；必要时按住 BOOT 再点 RESET
进入下载模式。本板是原生 USB Serial/JTAG（USB ID `303a:1001`），
无需安装驱动；若被识别成其他设备，换一条线/一个 USB 口。

**Q：编译报找不到 `gcm.h` / `gpio_ll.h`？**
ESP-IDF 版本不是 v6.0.1。按第 3 节安装正确版本。

**Q：改了 overlay 但配置没变？**
生成的 `build-muse-waveshare-s3-185b/sdkconfig` 优先生效。删除它或整个构建目录
再编译。

**Q：设备一直显示紫色/未配对？**
检查 SDK token 是否已写入 `build-muse-waveshare-s3-185b/sdkconfig` 并重新编译刷机；
确认 App 已开 Developer mode 且支持 community pairing v5。

**Q：电池显示 `-1` 或没有读数？**
本板电池读数来自 BQ27220 电量计，电量计仅由电池供电。未装电池（仅 USB 供电）
时属正常现象。装电池后仍无读数，请检查电池排线。

**Q：屏幕显示异常/花屏？**
确认刷的是 185B 固件而不是 185C（两者复位脚不同，刷错会导致屏幕异常）。
在监视日志中核对 `muse: board: Waveshare ESP32-S3-Touch-LCD-1.85B`。

**Q：想同时编译其他板子？**
各板共用 `esp32/managed_components/` 与 `dependencies.lock`，
在带屏板与不带屏板之间切换构建若报组件管理错误，删除这两个文件/目录再构建。

---

## 致谢与许可

- 上游 SDK：Meta Platforms, Inc. 及贡献者，Apache License 2.0（见 [LICENSE](../LICENSE)）。
- 本移植基于上游对 1.85C 的板级实现与
  [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 公开的 ST77916 面板初始化序列。
- 刷机有风险：可能失去保修、变砖等，请自担风险（上游原话：proceed at your own risk）。
