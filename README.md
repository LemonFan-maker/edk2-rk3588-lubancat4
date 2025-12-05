# 💻 鲁班猫4 (RK3588S) UEFI 固件移植

这是野火鲁班猫4 (LubanCat 4) 开发板的 UEFI 固件移植项目，基于 `edk2-porting` 社区代码，旨在实现对标准 AArch64 操作系统 (如 Windows on ARM, Ubuntu Server) 的完美引导。

**项目状态:** 编译成功，启动并进入 UEFI Shell / BIOS Setup。

-----

## ✨ 核心特性与适配情况

| 模块 | 状态 | 详细信息 |
| :--- | :--- | :--- |
| **SoC** | ✅ 匹配 | Rockchip RK3588S |
| **内存** | ✅ 完美 | 识别并使用全部 16GB LPDDR4X (四通道) |
| **PMIC** | ✅ 稳定 | RK806/RK860x 电压轨配置严格匹配 DTS，启动稳定 |
| **存储** | ✅ 工作 | eMMC (三星BJNB4R) 和 TF 卡 (SDHCI) 驱动正常 |
| **网络** | ✅ 工作 | GMAC1 (RTL8211F) 千兆以太网，TX Delay 修正 |
| **USB** | ✅ 正常 | USB 2.0 / USB 3.0 Host 接口供电和驱动正常 |
| **I2C/RTC** | ✅ 修复 | 解决了 `I2C Timeout` 报错，HYM8563 RTC 时钟总线修正至 I2C0 |
| **ACPI/SMBIOS** | ✅ 启用 | ACPI 表和 SMBIOS (CPU/RAM/Board Info) 正常加载 |

-----

## 🛠️ 编译环境准备 (Build Prerequisites)

你需要一个支持 AArch64 交叉编译的 Linux 环境 (推荐 Ubuntu 20.04/22.04)。

### 1\. 安装依赖

```bash
sudo apt update
# 安装交叉编译器、iasl (ACPI 编译器) 和通用构建工具
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

### 2\. 配置仓库 (Git Setup)

如果你是从本仓库克隆，请确保所有子模块都被正确拉取：

```bash
# 克隆本仓库
git clone https://github.com/LemonFan-maker/edk2-rk3588-lubancat4.git
cd edk2-rk3588-lubancat4

# 递归更新所有子模块 (包括 edk2, arm-trusted-firmware 等)
git submodule update --init --recursive
```

-----

## 🚀 固件编译指南 (Building the Firmware)

使用仓库自带的 `build.sh` 脚本进行编译。

### 1\. 执行编译

我们使用 `lbc-4` 作为设备标识符。

```bash
# 编译 DEBUG 版本 (推荐用于首次测试和调试)
./build.sh -d lbc-4 -r DEBUG

# 编译 RELEASE 版本 (更小更快)
./build.sh -d lbc-4 -r RELEASE
```

**编译成果:** 编译成功后，完整的固件镜像文件将生成在仓库根目录下：

```
RK3588_NOR_FLASH.img
```

-----

## 💾 烧录与启动 (Flashing & Booting)

**强烈建议使用 TF 卡 (MicroSD) 方式进行首次测试。**

**⚠️ 启动优先级特别警告 (必读)**

> 如果您的板载 eMMC 中已经存在 Linux 或 Android 系统（即已刷入 `rootfs`、`boot` 和 `uboot` 分区），MaskROM 会优先从 eMMC 启动。

**为确保能从 TF 卡启动，您必须先擦除 eMMC 中的 `uboot` 分区。**

### 1\. USB/MaskROM 烧录步骤

如果你选择通过 USB 线刷入 **`RK3588_NOR_FLASH.img`**：

1.  **进入 MaskROM 模式** (按住 MaskROM 键上电)。
2.  **【必须步骤】初始化 DDR：** 必须先下载官方的 Loader 文件，才能进行后续的 Flash 写入。
```bash
# 这一步是关键！需要使用官方的 MiniLoaderAll.bin
sudo rkdeveloptool db MiniLoaderAll.bin
```
3.  **烧录固件：**
```bash
sudo rkdeveloptool wl 0 RK3588_NOR_FLASH.img
```

### 2\. 运行状态变化的重要警告

> **🚨 注意！状态锁定警告 🚨**
> 一旦您成功刷入本 UEFI 固件后，**如果再次尝试通过软件命令 (`rkdeveloptool rd 3` 等) 进入 Loader 模式**，设备将不再进入 Loader 状态，而是直接回退到 **MaskROM 模式**。
>
> **这意味着：** 刷入 UEFI 后，如果您想再次使用 `rkdeveloptool` 刷写，**必须先通过硬件按键操作 (MaskROM 键/短接) 才能连接电脑。**

### 3\. TF 卡烧录 (推荐)

这是最安全的方法，不会影响板载 eMMC 里的系统。

1. **找到设备：** 确认你的 TF 卡在 Linux 下的设备名，例如 `/dev/sda` (⚠️ **务必小心确认设备名**)。
2.  **执行 DD 写入：**
    ```bash
    # 假设 TF 卡设备名为 /dev/sda
    sudo dd if=RK3588_NOR_FLASH.img of=/dev/sda bs=1M status=progress conv=fsync
    ```

### 4\. 启动流程

1.  **连接串口：** 将 USB 转 TTL 串口线连接到 Debug Port (UART)。波特率设为 **`1500000`**。
2.  **强制启动：**
      * 插入烧录好的 TF 卡。
      * **按住鲁班猫4上的 Recovery 键。**
      * 插入 Type-C 供电。
      * 保持 3-5 秒后松开 Recovery 键。
3.  **观察日志：** 观察串口日志，看到 `UEFI firmware` 和 `Shell (F1)` 提示即为成功。

### 5\. 引导系统测试

1. 启动Ubuntu22.04 ARM64 Server 需要添加启动参数console=ttyS2 1500000n8 earlycon=uart8250,mmio32,0xfeb50000 acpi=force

-----

## ✅ 验证状态与待办事项 (Status & TODOs)

固件已成功启动并加载核心驱动，但以下功能尚未在操作系统环境下进行完整验证或仍存在已知问题：

### 1\. 核心功能验证状态 (Functionality Status)

| 功能模块 | 配置状态 | 实际验证状态 | 待办事项 / 备注 |
| :--- | :--- | :--- | :--- |
| **引导启动** | ✅ 启用 | ✅ | OS 可以正常加载 |
| **Ethernet** | ✅ 启用 | **待测试** | 驱动已加载，MAC 地址已获取，但未进行网络数据传输测试。|
| **mini-PCIE** | ✅ 启用 | ✅ | PCIe 链路已连接 (Link up)，测试了Wi-Fi网卡模块的使用。|
| **USB 2.0/3.0** | ✅ 启用 | ✅ | 仅验证了键盘输入和 U 盘枚举，需在 OS 中测试所有 Type-A 接口的稳定性。|
| **GPIO (通用)** | ✅ 启用 | **待测试** | 40-Pin 扩展接口的通用 GPIO 功能尚未验证。|
| **cpufreq** | ✅ 启用 | ❌ | 虽然无法切换频率，但是CPU似乎已经保持在一个较高的频率下工作 |

### 2\. 硬件功能 (I/O & Multimedia)

| 功能模块 | 配置状态 | 实际验证状态 | 待办事项 / 备注 |
| :--- | :--- | :--- | :--- |
| **HDMI/DP** | ✅ 启用 | **待测试** | UEFI 启动日志显示 `Not Found`。需要连接显示器并加载 GOP 驱动进行显示功能验证。|
| **Type-C** | ✅ 启用 | **待测试** | DP 输出和 OTG 模式的切换功能尚未验证。|
| **音频 (ES8388)** | ✅ 启用 | **待测试** | Codec 驱动已加载，I2C 通信正常，但音频输出/输入功能需在 OS 中测试。|
| **Fan (PWM)** | ✅ 启用 | ✅ | PWM 驱动已配置，似乎手动指定占空比无效，转速似乎恒定100% |

### 3\. 已知残留问题 (Known Issues)

- **TFTP Assert：** 日志中可能出现 `ASSERT_EFI_ERROR (Status = Not Found)`，这是 TftpDynamicCommand 找不到网络连接所致，属于正常现象，不影响启动。
- **FTW/NVRAM Corruption：** 首次启动时出现的 `Firmware Volume for Variable Store is corrupted` 错误是正常的，系统会自动修复。

- **烧录失败 (NEW):** 在 Linux 环境下执行 `rkdeveloptool db` 时，可能会遇到以下报错并导致设备断开连接 (VID: 0x350b)：

```text
Creating Comm Object failed!
```
> **解决方案：** 这是 Linux 下 USB 通信不稳定的常见问题（尤其是虚拟机环境）。如果遇到此错误，请切换到 **Windows** 环境，使用官方的 **RKDevTool** 工具进行 Loader 初始化和固件烧写。

*TODO...*

-----

## 📄 许可证与鸣谢 (License & Credits)

本项目主要基于 `edk2-porting` 社区的 RK3588 移植成果。

  * **EDK2 核心:** TianoCore, Linaro, ARM
  * **许可证:** 本项目遵循 BSD-2-Clause 许可证。

欢迎提出 Issue 或 Pull Request，共同完善鲁班猫4的 UEFI 固件！
