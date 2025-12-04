/** @file
 *
 *  Differentiated System Definition Table (DSDT)
 *
 *  Copyright (c) 2020, Pete Batard <pete@akeo.ie>
 *  Copyright (c) 2018-2020, Andrey Warkentin <andrey.warkentin@gmail.com>
 *  Copyright (c) Microsoft Corporation. All rights reserved.
 *  Copyright (c) 2021, ARM Limited. All rights reserved.
 *  Copyright (c) 2025, OrionisLi(CyanLi) <2254650260@qq.com>

 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include "AcpiTables.h"

// =============================================================
// 音频配置 (基于 I2C7 DTS)
// =============================================================
#define BOARD_I2S0_TPLG "i2s-jack"
#define BOARD_AUDIO_CODEC_HID "ESSX8388"
#define BOARD_CODEC_I2C "\\_SB.I2C7"      // 挂载 I2C7
#define BOARD_CODEC_I2C_ADDR 0x11          // 地址 0x11

// GPIO 暂时保留占位符，不影响启动
#define BOARD_CODEC_GPIO "\\_SB.GPI1"
#define BOARD_CODEC_GPIO_PIN GPIO_PIN_PA6

DefinitionBlock ("Dsdt.aml", "DSDT", 2, "RKCP  ", "RK3588  ", 2)
{
  Scope (\_SB_)
  {
    // --- 基础定义 ---
    include ("DsdtCommon.asl")
    include ("Cpu.asl")

    // --- 存储 (规格表确认) ---
    // 鲁班猫4有板载 eMMC (32-128GB) -> 必须开启
    include ("Emmc.asl")
    // 支持 TF 卡启动 -> 必须开启
    include ("Sdhc.asl")

    // --- 扩展接口 ---
    // 规格表提到 Mini-PCIE 接口 (WiFi/4G) -> 必须开启 PCIe
    // RK3588S PCIe 资源较少，但猜测对应 Combo 接口
    include ("Pcie.asl")
    
    // Sata.asl 通常用于 M.2 SATA 或 PCIe 复用，先保留，如果报错再注释
    include ("Sata.asl") 

    // --- 系统外设 ---
    include ("Dma.asl")
    include ("Uart.asl")   // 1500000 波特率调试串口
    include ("Gpio.asl")
    include ("I2c.asl")    // 开启 I2C 总线控制器
    // include ("Spi.asl") // 40Pin 上有 SPI，如果没接屏幕可暂时注释

    // --- 网络 ---
    // 规格表：1000M 以太网 -> 对应 GMAC1
    include ("Gmac1.asl")

    // --- 音频 ---
    include ("I2s.asl")

    // =============================================================
    // USB 配置
    // 规格表：USB2.0 x3 (Type-A) + USB3.0 x1 (Type-A) + Type-C
    // DTS确认：host0_ehci/ohci 和 host1_ehci/ohci 全部开启
    // =============================================================
    
    // 1. 开启原生 USB 2.0 控制器 (驱动板载的 Hub 和 2.0 接口)
    include ("Usb2Host.asl") 

    // 2. 开启 USB 3.0 控制器
    // Usb3Host0 通常对应 Type-C 口 (OTG/DP)
    include ("Usb3Host0.asl") 
    
    // Usb3Host2 通常对应唯一的那个 USB 3.0 Type-A 口
    // (RK3588S 的第二个 USB3 控制器在 ACPI 表中映射为 Host2)
    include ("Usb3Host2.asl")

    // =============================================================
    // 具体设备挂载 Scope
    // =============================================================
    
    // 音频芯片挂载
    Scope (I2C7) {
      include ("Es8388.asl")
    }
  }
}