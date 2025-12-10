##
### 📋 核心架构图

这张 SD 卡将包含两个部分：

1.  **隐藏区域 (0 - 32MB)**: 存放 UEFI 固件 (`RK3588_NOR_FLASH.img`)，这是 MaskROM 启动的位置。
2.  **系统分区 (32MB - 结尾)**: 存放 Ubuntu 根文件系统、内核和配置文件。

-----

### 🛠️ 第一阶段：准备工作
**你需要：**
1.  **Linux 环境** (PC 或虚拟机)。
2.  **MicroSD 卡** (建议 32GB+，当然16GB也可以, Class 10及以上)。
3.  **核心文件**：
      * `RK3588_NOR_FLASH.img` (编译好的 UEFI 固件)。
      * `ubuntu-base-2x.04.x-base-arm64.tar.gz` (从 Ubuntu 官网下载)。
      * **内核文件** (从鲁班猫4制作的linux-image-6.1.99-rk3588中提取)：
          * `Image-6.1.99-rk3588` (重命名为 `vmlinuz-6.1.99-rk3588`)
          * `rk3588s-lubancat-4.dtb` (放在 `dtb` 文件夹)
          * `/lib/modules/` (驱动模块文件夹)

-----

### 💾 第二阶段：烧录固件与分区

这是最关键的一步，实现了“先占坑，后盖楼”。

**1. 烧录 UEFI 到卡头**
*(假设 TF 卡设备名为`/dev/sda`，请务必确认！)*

```bash
sudo dd if=RK3588_NOR_FLASH.img of=/dev/sda bs=1M status=progress conv=fsync
```

**2. 创建安全分区**
使用 `fdisk` 在固件之后建立分区。

```bash
sudo fdisk /dev/sda
```
  * 输入 `g` (新建 GPT 表)。*(如果提示有旧签名，选 No 保留固件)*
  * 输入 `n` (新建分区)。
  * **分区号**：回车 (默认)。
  * **起始扇区 (First sector)**：输入 **`65536`** (即 32MB 处，避开 UEFI)。
  * **结束扇区**：回车 (默认全选)。
  * 输入 `w` (保存)。

**3. 格式化系统盘**

```bash
sudo mkfs.ext4 /dev/sda2 -L "LubanOS"
```

-----

### 📦 第三阶段：部署 Ubuntu 系统

**1. 挂载与解压**

```bash
sudo mkdir -p /mnt/sdcard
sudo mount /dev/sda2 /mnt/sdcard

# 解压 Ubuntu Base (根文件系统)
sudo tar -xvpzf ubuntu-base-2x.04.2-base-arm64.tar.gz -C /mnt/sdcard/
```

**2. 植入内核 (移植野火 BSP 内核)**
将你的内核文件复制到 SD 卡的 `/boot` 目录。目录结构应如下：

```bash
/mnt/sdcard/boot/
├── vmlinuz-6.1.99-rk3588          # 内核 (Image 改名)
└── dtb/
    └── rk3588s-lubancat-4.dtb     # 官方设备树
```

*把模块文件夹复制到 `/mnt/sdcard/lib/modules/`*。

**3. 基础配置 (Chroot)**
为了能登录和联网，需要简单配置一下。

```bash
# 复制 QEMU 和网络配配置
# 这里需要安装 qemu-aarch64-static
sudo cp /usr/bin/qemu-aarch64-static /mnt/sdcard/usr/bin/
sudo cp /etc/resolv.conf /mnt/sdcard/etc/

# 进入系统
sudo chroot /mnt/sdcard

# --- 以下在 chroot 内执行 ---
apt update
apt install systemd-sysv network-manager net-tools nano vim openssh-server rfkill  net-tools lm-sensors
passwd root  # 设置密码
echo "lubancat" > /etc/hostname
exit
# --- 退出 chroot ---
```

-----

### 🚀 第四阶段：配置自动启动 (关键)

因为我们没有标准的 EFI 分区结构，利用 **UEFI Shell 脚本 (`startup.nsh`)** 是最稳妥的自动启动方式。

**1. 在 SD 卡根目录创建脚本**
在 `/mnt/sdcard/` 下创建一个名为 `startup.nsh` 的文件。

```bash
sudo nano /mnt/sdcard/startup.nsh
```

**2. 填入启动命令**
*(根据之前的验证，这是必定成功的命令)*

```text
\boot\vmlinuz-6.1.99-rk3588 dtb=\boot\dtb\rk3588s-lubancat-4.dtb root=PARTUUID=<改成自己的PARTUUID> rw rootwait console=ttyS2,1500000n8 earlycon=uart8250,mmio32,0xfeb50000
```

  * **注意 1**：请务必使用 `blkid /dev/sdb2` 获取你自己的 **PARTUUID** 并替换上面的值，**不是UUID，是PARTUUID！**
  * **注意 2**：Rockchip 巨型内核**不需要** `initrd=` 参数。

**3. 卸载 SD 卡**

```bash
sudo umount /mnt/sdcard
```

-----

### ⚡️ 第五阶段：上电使用

1.  **插入 TF 卡** 到鲁班猫4。
2.  **连接 USB 转 TTL 串口** (波特率 1500000)。
3.  **上电**。
  * *如果板载 eMMC 有旧系统，需擦除UBOOT之后上电以强制读 TF 卡*。
4.  **观察流程**：
      * 串口显示 LuBanCat Logo。
      * UEFI 进入 Shell，倒计时 5 秒。
      * **自动执行 `startup.nsh`**。
      * Linux 内核启动 -\> 挂载 Rootfs -\> 进入 Login 界面。

-----

### ✅ 这张卡的特性

* **救援神器**：板载 eMMC 刷坏了？插上这张卡就能启动系统。
* **安装盘**：启动后，可以使用 `dd` 或 `rsync` 把这个系统把自己克隆到 eMMC 里。
* **硬件全开**：因为使用了 BSP 内核，**NPU、GPU、VPU、Wi-Fi、HDMI** *理论上*全部可用。