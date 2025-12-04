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
  PLATFORM_NAME                  = LubanCat4
  PLATFORM_VENDOR                = EmbedFire
  PLATFORM_GUID                  = d6741299-c347-4115-9f1a-1207fdf16ab0
  PLATFORM_VERSION               = 0.2
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  VENDOR_DIRECTORY               = Platform/$(PLATFORM_VENDOR)
  PLATFORM_DIRECTORY             = $(VENDOR_DIRECTORY)/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

  #
  # Platform based on LubanCat4 board
  #

!include Platform/EmbedFire/LubanCat4/LubanCat4.dsc.inc

################################################################################
#
# Pcd Section
#
################################################################################

[PcdsFixedAtBuild.common]
  # SMBIOS platform config

  gRockchipTokenSpaceGuid.PcdPlatformName|"LubanCat4"
  gRockchipTokenSpaceGuid.PcdPlatformVendorName|"EmbedFire"
  gRockchipTokenSpaceGuid.PcdFamilyName|"LubanCat"
  gRockchipTokenSpaceGuid.PcdProductUrl|"https://lubancat.embedfire.com/"
  gRockchipTokenSpaceGuid.PcdDeviceTreeName|"rk3588s-lubancat-4"
