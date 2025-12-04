## @file
#
#  Copyright (c) 2014-2018, Linaro Limited. All rights reserved.
#  Copyright (c) 2025, OrionisLi(CyanLi) <2254650260@qq.com>
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

  # ACPI Support
  
  INF RuleOverride = ACPITABLE Platform/EmbedFire/LubanCat4/AcpiTables/AcpiTables.inf

  # Device Tree Support

  INF RuleOverride = DTB Platform/EmbedFire/LubanCat4/DeviceTree/Vendor.inf

  # Splash screen logo

  INF $(VENDOR_DIRECTORY)/Drivers/LogoDxe/LogoDxe.inf
