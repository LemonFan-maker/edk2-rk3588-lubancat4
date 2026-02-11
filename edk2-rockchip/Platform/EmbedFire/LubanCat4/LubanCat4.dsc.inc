## @file
#
#  Copyright (c) 2014-2018, Linaro Limited. All rights reserved.
#  Copyright (c) 2025, OrionisLi(CyanLi) <2254650260@qq.com>
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section
#
################################################################################
[Defines]
  FLASH_DEFINITION                 = Silicon/Rockchip/RK3588/RK3588.fdf
  RK_PLATFORM_FVMAIN_MODULES       = Platform/EmbedFire/LubanCat4/LubanCat4.Modules.fdf.inc

  #
  # HYM8563 RTC support
  #

  DEFINE RK_RTC8563_ENABLE = TRUE

  #
  # RK3588S-based platform
  #

!include Silicon/Rockchip/RK3588/RK3588SPlatform.dsc.inc

################################################################################
#
# Library Class section
#
################################################################################

[LibraryClasses.common]
  RockchipPlatformLib|Platform/EmbedFire/LubanCat4/Library/RockchipPlatformLib/RockchipPlatformLib.inf
  Rk806Lib|Silicon/Rockchip/Library/SpiLib/RK806.inf

################################################################################
#
# Pcd Section
#
################################################################################

[PcdsFixedAtBuild.common]
  # SMBIOS platform config

  gRockchipTokenSpaceGuid.PcdBoardName|"LubanCat4"
  gRockchipTokenSpaceGuid.PcdBoardVendorName|"EmbedFire"

  # ==========================================================
  # I2C Configuration (基于 DTS: &i2c0)
  # ==========================================================
  # 0x42: RK8602 (CPU供电) -> I2C0
  # 0x43: RK8603 (CPU供电) -> I2C0
  # 0x51: HYM8563 (RTC时钟) -> I2C0
  # 0x11: ES8388 (音频)    -> I2C7
  # ==========================================================
  
  # 地址列表

  gRockchipTokenSpaceGuid.PcdI2cSlaveAddresses|{ 0x42, 0x43, 0x51, 0x11 }
  
  # 【FIXED】 总线映射修正
  # 之前是 { 0x0, 0x0, 0x2, 0x7 }，RTC 必须改成 0x0

  gRockchipTokenSpaceGuid.PcdI2cSlaveBuses|{ 0x0, 0x0, 0x0, 0x7 }
  
  gRockchipTokenSpaceGuid.PcdI2cSlaveBusesRuntimeSupport|{ FALSE, FALSE, TRUE, FALSE }
  
  # PMIC Config (CPU电压调节) - (Bus 0)

  gRockchipTokenSpaceGuid.PcdRk860xRegulatorAddresses|{ 0x42, 0x43 }
  gRockchipTokenSpaceGuid.PcdRk860xRegulatorBuses|{ 0x0, 0x0 }
  gRockchipTokenSpaceGuid.PcdRk860xRegulatorTags|{ $(SCMI_CLK_CPUB01), $(SCMI_CLK_CPUB23) }
  
  # 【FIXED】 RTC Config
  # 将总线 ID 从 0x2 改为 0x0

  gPcf8563RealTimeClockLibTokenSpaceGuid.PcdI2cSlaveAddress|0x51
  gRockchipTokenSpaceGuid.PcdRtc8563Bus|0x0

  #
  # PCIe/SATA/USB Combo PIPE PHY (RK3588S Specifics)
  #

  gRK3588TokenSpaceGuid.PcdComboPhy0Switchable|TRUE
  gRK3588TokenSpaceGuid.PcdComboPhy2Switchable|TRUE
  gRK3588TokenSpaceGuid.PcdComboPhy0ModeDefault|$(COMBO_PHY_MODE_PCIE)
  gRK3588TokenSpaceGuid.PcdComboPhy2ModeDefault|$(COMBO_PHY_MODE_USB3)

  #
  # USB/DP Combo PHY
  #

  gRK3588TokenSpaceGuid.PcdUsbDpPhy0Supported|TRUE
  gRK3588TokenSpaceGuid.PcdDp0LaneMux|{ 0x2, 0x3 }

  #
  # GMAC (Ethernet)
  #

  gRK3588TokenSpaceGuid.PcdGmac1Supported|TRUE
  
  # 【FIX】 Changed from 0x43 to 0x1b to match LubanCat 4 DTS (tx_delay)
  
  gRK3588TokenSpaceGuid.PcdGmac1TxDelay|0x1b

  #
  # On-Board fan output
  #

  gRK3588TokenSpaceGuid.PcdHasOnBoardFanOutput|TRUE

  #
  # I2S
  #

  gRK3588TokenSpaceGuid.PcdI2S0Supported|TRUE

  #
  # Display support
  #

  gRK3588TokenSpaceGuid.PcdDisplayConnectors|{CODE({
    VOP_OUTPUT_IF_HDMI0,
    VOP_OUTPUT_IF_DP0
  })}

################################################################################
#
# Components Section
#
################################################################################

[Components.common]
  # ACPI Support

  Platform/EmbedFire/LubanCat4/AcpiTables/AcpiTables.inf

  # Device Tree Support

  Platform/EmbedFire/LubanCat4/DeviceTree/Vendor.inf

  # Splash screen logo
  
  $(VENDOR_DIRECTORY)/Drivers/LogoDxe/LogoDxe.inf