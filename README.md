# 鲁班猫4 (RK3588S) UEFI 固件移植

这是野火鲁班猫4 (LubanCat 4) 开发板的 UEFI 固件移植项目，基于 `edk2-porting` 社区代码，目标是为标准 AArch64 操作系统提供可用的引导支持，例如 Windows on ARM 与 Ubuntu Server。

固件本身负责平台初始化与引导，操作系统内的硬件支持取决于内核与驱动。镜像同时内置 Vendor 与 Mainline 两套设备树：Vendor 面向 Rockchip SDK 6.1 内核，Mainline 面向上游 6.10 及以上内核，默认使用 Mainline。可在 UEFI Setup 的 ACPI / Device Tree 页面切换，详见下文设备树选择一节。

详细的部署流程请参考文章[给鲁班猫 4 移植一个 UEFI 进来有多享受](https://posts.orionisli.cn/2025/12/16/RK3588S移植UEFI实战指北/)。

## 分支说明

仓库目前有两个分支，用途不同：

| 分支 | 用途 | 与另一分支的差异 |
| :--- | :--- | :--- |
| master | 主分支，常规 UEFI 固件，CI 构建与发布以此为准 | 不包含 XNU/AMP Wi-Fi 专用改动 |
| amp-wifi | 面向 HakiOS/XNU 的 AMP Wi-Fi 开发分支 | 截至 2026 年 11 月 12 日比 master 多一个 Mali-G610 预初始化提交 |

amp-wifi 以 master 为基础，额外改动是在 ExitBootServices 前完成 Mali-G610 的上电、选频与复位释放。XNU 的 g610 驱动要求交接时 GPU 区块已经上电，否则读取关闭区块的寄存器会挂起总线，因此这项改动暂未合入 master。

普通引导场景请使用 master。只有参与 XNU/AMP Wi-Fi 开发时才需要 amp-wifi。常规推送构建只在 master 上触发，向 amp-wifi 推送不会产生自动构建镜像。

## 核心特性与适配情况

| 模块 | 状态 | 详细信息 |
| :--- | :--- | :--- |
| SoC | 匹配 | Rockchip RK3588S |
| 内存 | 正常 | 识别并使用全部 16GB LPDDR4X，四通道 |
| PMIC | 稳定 | RK806/RK860x 电压轨配置与 DTS 一致，启动稳定 |
| 存储 | 正常 | eMMC 与 TF 卡 (SDHCI) 驱动工作正常 |
| 网络 | 正常 | GMAC1 (RTL8211F) 千兆以太网，TX Delay 已修正 |
| USB | 正常 | USB 2.0 与 USB 3.0 Host 接口供电和驱动正常 |
| I2C/RTC | 修复 | 解决 I2C Timeout 报错，HYM8563 RTC 时钟总线修正至 I2C0 |
| 设备树 | 双 DTB | Vendor 与 Mainline 同时打包，Setup 可切换，默认 Mainline |
| GPU | 预初始化 | 仅 amp-wifi 分支：ExitBootServices 前完成 Mali-G610 上电、选频与复位释放 |
| 温控与风扇 | 启用 | TSADC 通道 0 与 PWM0 组成 ACPI 闭环，四档调速，95 摄氏度关机 |
| ACPI/SMBIOS | 启用 | ACPI 表与 SMBIOS (CPU/RAM/Board Info) 正常加载 |

## 编译环境准备

需要一个支持 AArch64 交叉编译的 Linux 环境，Arch Linux、Ubuntu 22.04,24.04,26.04等均可，`build.sh` 会为 TF-A 与 BaseTools 追加必要的兼容参数。

### 安装依赖

```bash
sudo apt update
sudo apt-get install -y \
    acpica-tools \
    binutils-aarch64-linux-gnu \
    build-essential \
    device-tree-compiler \
    gettext \
    git \
    gcc-aarch64-linux-gnu \
    libc6-dev-arm64-cross \
    python3 \
    python3-pyelftools \
    uuid-dev
```

### 配置仓库

如果从本仓库克隆，请确保所有子模块都被正确拉取：

```bash
git clone https://github.com/LemonFan-maker/edk2-rk3588-lubancat4.git
cd edk2-rk3588-lubancat4
git submodule update --init --recursive
```

克隆后默认检出 master。如需 XNU/AMP Wi-Fi 相关改动，执行 `git checkout amp-wifi`。

## 固件编译指南

使用仓库自带的 `build.sh` 脚本进行编译，设备标识符为 `lbc-4`。

```bash
# 编译 DEBUG 版本，推荐用于首次测试和调试
./build.sh -d lbc-4 -r DEBUG

# 编译 RELEASE 版本，体积更小
./build.sh -d lbc-4 -r RELEASE
```

编译成功后，完整的固件镜像会生成在仓库根目录：

```text
RK3588_NOR_FLASH.img
```

## 设备树选择

固件在同一镜像中打包了两套 DTB，由 UEFI 变量 `FdtCompatMode` 决定向操作系统暴露哪一套：

| 模式 | 适用内核 | 说明 |
| :--- | :--- | :--- |
| Vendor | Rockchip SDK 6.1 | 保留 rockchip-amp、rpmsg 共享内存、rknpu、rkvdec、rkisp 等厂商节点 |
| Mainline | 上游 6.10 及以上 | 默认模式，使用上游节点命名与 PCIe Wi-Fi 配置 |

切换方法：启动时按 ESC 或 F2 进入 UEFI Setup，依次进入 ACPI / Device Tree、Device Tree Configuration、Compatibility Mode。Config Table Mode 需要包含 Device Tree，默认值为 Both。该选项带 RESET_REQUIRED 标记，修改后需要重启生效。

设备树只在 Device Tree 模式下使用。Windows 等仅使用 ACPI 的系统不受该选项影响。若引导程序自行提供设备树，例如 GRUB 的 `devicetree` 命令，固件 DTB 会被覆盖。

## 烧录与启动

首次测试强烈建议使用 TF 卡，不会影响板载 eMMC 中的系统。

### 启动优先级警告

如果板载 eMMC 中已经存在 Linux 或 Android 系统，即已刷入 rootfs、boot 和 uboot 分区，MaskROM 会优先从 eMMC 启动。为确保从 TF 卡启动，必须先擦除 eMMC 中的 uboot 分区。

### USB 与 MaskROM 烧录

如果选择通过 USB 线刷入 `RK3588_NOR_FLASH.img`：

1. 按住 MaskROM 键上电，进入 MaskROM 模式。
2. 先下载官方 Loader 文件，再进行后续写入：

```bash
sudo rkdeveloptool db MiniLoaderAll.bin
```

3. 烧录固件：

```bash
sudo rkdeveloptool wl 0 RK3588_NOR_FLASH.img
```

### 状态锁定警告

刷入本 UEFI 固件后，如果再次尝试通过软件命令进入 Loader 模式，例如 `rkdeveloptool rd 3`，设备不会进入 Loader 状态，而是直接回退到 MaskROM 模式。这意味着刷入 UEFI 后，如需再次使用 `rkdeveloptool` 刷写，必须通过硬件按键操作进入 MaskROM 模式。

### TF 卡烧录

这是最安全的方法，不会影响板载 eMMC 里的系统。

1. 确认 TF 卡在 Linux 下的设备名，例如 `/dev/sda`，务必小心确认。
2. 执行写入：

```bash
sudo dd if=RK3588_NOR_FLASH.img of=/dev/sda bs=1M status=progress conv=fsync
```

### 启动流程

1. 将 USB 转 TTL 串口线连接到 Debug Port，波特率设为 1500000。
2. 插入烧录好的 TF 卡，按住 Recovery 键，插入 Type-C 供电，保持 3 到 5 秒后松开。
3. 观察串口日志，出现 UEFI firmware 与 Shell (F1) 提示即为成功。

### 引导系统测试

启动 Ubuntu 22.04 ARM64 Server 时需要在 GRUB 界面按 `e` 修改启动参数，将原有的 console 参数替换为：

```text
console=ttyS2 1500000n8 earlycon=uart8250,mmio32,0xfeb50000 acpi=force
```

## 验证状态与待办事项

固件已能启动并加载核心驱动，以下功能尚未在操作系统环境下完成全部验证，或仍存在已知问题。

### 核心功能验证状态

| 功能模块 | 配置状态 | 实际验证状态 | 待办事项与备注 |
| :--- | :--- | :--- | :--- |
| 引导启动 | 启用 | 正常 | 操作系统可以正常加载 |
| Ethernet | 启用 | 待测试 | 驱动已加载，MAC 地址已获取，尚未进行网络数据传输测试 |
| mini-PCIE | 启用 | 正常 | PCIe 链路已连接，已测试 Wi-Fi 网卡模块 |
| USB 2.0/3.0 | 启用 | 正常 | 已验证键盘输入与 U 盘枚举，仍需测试所有 Type-A 接口的稳定性 |
| GPIO | 启用 | 待测试 | RK3588 pinconf 位宽与 SMT 寄存器已修正，40-Pin 扩展接口尚未验证 |
| cpufreq | 启用 | 正常 | policy0 到 policy6 均可切换 |

### 硬件功能

| 功能模块 | 配置状态 | 实际验证状态 | 待办事项与备注 |
| :--- | :--- | :--- | :--- |
| HDMI/DP | 启用 | 正常 | HDMI 可以正常显示，Logo 可以正常显示 |
| Type-C | 启用 | 待测试 | DP 输出和 OTG 模式的切换功能尚未验证 |
| 音频 (ES8388) | 启用 | 待测试 | Codec 驱动已加载，I2C 通信正常，音频输出与输入功能需在操作系统中测试 |
| Fan (PWM) | 启用 | 待测试 | ACPI 温控已实现：42/55/60/65 摄氏度对应 39/59/78/100% 占空比，95 摄氏度关机，需在操作系统中实测 |

### 已知残留问题

TFTP Assert：日志中可能出现 `ASSERT_EFI_ERROR (Status = Not Found)`，这是 TftpDynamicCommand 找不到网络连接所致，属于正常现象，不影响启动。

FTW/NVRAM Corruption：首次启动时出现的 `Firmware Volume for Variable Store is corrupted` 错误是正常的，系统会自动修复。

烧录失败：在 Linux 环境下执行 `rkdeveloptool db` 时可能遇到以下报错并导致设备断开连接，VID 为 0x350b：

```text
Creating Comm Object failed!
```

大概率是因为没有足够的权限操作 USB 节点，可使用 `sudo chmod 777 -R /dev/bus/usb` 解决。

## 实验性功能：USB Mass Storage Mode

该功能把板载 eMMC 通过 USB-C 以 U 盘形式暴露给主机，代码位于 `Silicon/Rockchip/Applications/MassStorageMode`，默认不编译。启用需要同时修改三处：

1. `LubanCat4.dsc.inc` 中将 `PcdMassStorageModeSupported` 设为 TRUE。
2. `LubanCat4.dsc.inc` 的 Components.common 中取消 `MassStorageMode.inf` 的注释。
3. `LubanCat4.Modules.fdf.inc` 中取消 `MassStorageMode.inf` 的注释。

启用后启动页会显示 Mass Storage (F5)。该功能默认关闭，尚未纳入常规验证。

## 更新历史

### 2026/11/12

镜像同时携带 Vendor 与 Mainline 设备树，Setup 可选择，默认 Mainline。PWM0 风扇改为 TSADC 闭环 ACPI 温控，四档调速，95 摄氏度关机。amp-wifi 分支新增 Mali-G610 预初始化，在 ExitBootServices 前完成 GPU 上电、选频与复位释放，该改动尚未合入 master。新增 USB Mass Storage Mode，默认由 PCD 关闭。修正 RK3588 GPIO pinconf 位宽与 SMT 寄存器布局。适配 GCC 16 主机工具链。

### 2026/9/7

目前内核已经进入主线 7.3-rc1，但 NPU 和 PWM 风扇调速的 patch 由于提交较晚，没有赶上 7.3 的补丁提交，大概需要等到 10 月份 7.4 内核的提交之后一起进行合并。基本功能已经实现，如果不在意 NPU 和 PWM 调速，可以选择尝试。

已经解决 Windows 11 24H2 启动提示 inaccessible_boot_device 的问题，后续更新 UEFI。WoR 驱动的实现也基本完毕，准备开源。由于部署 Windows 过于困难，暂时不开源部署教程。

### 2026/7/7

已经向 Linux 主线提出将鲁班猫4的设备树合入主线的请求。项目状态为编译成功，启动并进入 UEFI Shell 或 BIOS Setup。请不要尝试使用这个项目引导启动 Windows 系统，因为驱动问题导致无法识别 eMMC，或者出现 inaccessible_boot_device。我会在鲁班猫4合入主线之后着手开始 Windows 驱动的开发，争取在 9 月之前实现 WoR 驱动的正常使用。

## 许可证与鸣谢

本项目主要基于 `edk2-porting` 社区的 RK3588 移植成果。

- EDK2 核心：TianoCore、Linaro、ARM
- 许可证：本项目遵循 BSD-2-Clause 许可证

欢迎提出 Issue 或 Pull Request，共同完善鲁班猫4的 UEFI 固件。