/** @file
*
* Copyright (c) 2021, Rockchip Limited. All rights reserved.
* Copyright (c) 2025, OrionisLi(CyanLi) <2254650260@qq.com>
*
* SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include <Base.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/GpioLib.h>
#include <Library/RK806.h>
#include <Library/Rk3588Pcie.h>
#include <Library/PWMLib.h>
#include <Soc.h>
#include <VarStoreData.h>
#include <Library/UefiBootServicesTableLib.h>

// --- PMIC 配置 (基于 LubanCat 4 DTS) ---
static struct regulator_init_data  rk806_init_data[] = {
  /* Master PMIC - Voltage Rails verified against DTS */
  
  /* BUCK1 (vdd_gpu_s0): DTS 550-950mV -> Set 750mV Safe */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK1,  750000),
  
  /* BUCK3 (vdd_log_s0): DTS 675-800mV -> Set 750mV */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK3,  750000),
  
  /* BUCK4 (vdd_vdenc_s0): DTS Init 750mV -> Set 750mV */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK4,  750000),
  
  /* BUCK5 (vdd_ddr_s0): DTS 675-900mV -> Set 850mV */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK5,  850000),
  
  /* BUCK6 (vdd2_ddr_s3): No voltage in DTS (Fixed/Slave) -> Skip */
  // RK8XX_VOLTAGE_INIT(MASTER_BUCK6, 750000),
  
  /* BUCK7 (vcc_2v0_pldo_s3): DTS Fixed 2000mV -> Set 2000mV */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK7,  2000000),
  
  /* BUCK8 (vcc_3v3_s3): DTS Fixed 3300mV -> Set 3300mV */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK8,  3300000),
  
  /* BUCK10 (vcc_1v8_s3): DTS Fixed 1800mV -> Set 1800mV */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK10, 1800000),

  /* PLDO1 (vcc_1v8_s0): DTS Fixed 1800mV -> Set 1800mV */
  RK8XX_VOLTAGE_INIT (MASTER_NLDO1,  750000), // 注意：EDK2 宏定义命名可能偏移，此处对应 NLDO1 (vdd_0v75_s3) 750mV
  RK8XX_VOLTAGE_INIT (MASTER_NLDO2,  850000), // NLDO2 (avdd_ddr_pll_s0) 850mV
  RK8XX_VOLTAGE_INIT (MASTER_NLDO3,  750000), // NLDO3 (avdd_0v75_s0) 750mV
  RK8XX_VOLTAGE_INIT (MASTER_NLDO4,  850000), // NLDO4 (avdd_0v85_s0) 850mV
  // RK8XX_VOLTAGE_INIT (MASTER_NLDO5,  750000), // DTS 中不存在 NLDO5，直接移除

  /* PLDO Configuration */
  RK8XX_VOLTAGE_INIT (MASTER_PLDO1,  1800000), // vcc_1v8_s0
  RK8XX_VOLTAGE_INIT (MASTER_PLDO2,  1800000), // avcc_1v8_s0
  RK8XX_VOLTAGE_INIT (MASTER_PLDO3,  1200000), // avdd_1v2_s0 (1.2V Verified)
  RK8XX_VOLTAGE_INIT (MASTER_PLDO4,  3300000), // avcc_3v3_s0
  RK8XX_VOLTAGE_INIT (MASTER_PLDO5,  3300000), // vccio_sd_s0 (Start at 3.3V for SD)
  RK8XX_VOLTAGE_INIT (MASTER_PLDO6,  1800000), // pldo6_s3
};

VOID
EFIAPI
SdmmcIoMux (
  VOID
  )
{
  /* TF Card IOMUX - Standard RK3588 */
  BUS_IOC->GPIO4D_IOMUX_SEL_L  = (0xFFFFUL << 16) | (0x1111); // D0-D3
  BUS_IOC->GPIO4D_IOMUX_SEL_H  = (0x00FFUL << 16) | (0x0011); // CLK, CMD
  PMU1_IOC->GPIO0A_IOMUX_SEL_H = (0x000FUL << 16) | (0x0001); // DET
}

VOID
EFIAPI
SdhciEmmcIoMux (
  VOID
  )
{
  /* eMMC IOMUX - Standard RK3588 */
  BUS_IOC->GPIO2A_IOMUX_SEL_L = (0xFFFFUL << 16) | (0x1111);
  BUS_IOC->GPIO2D_IOMUX_SEL_L = (0xFFFFUL << 16) | (0x1111);
  BUS_IOC->GPIO2D_IOMUX_SEL_H = (0xFFFFUL << 16) | (0x1111);
}

#define NS_CRU_BASE       0xFD7C0000
#define CRU_CLKSEL_CON59  0x03EC
#define CRU_CLKSEL_CON78  0x0438

VOID
EFIAPI
Rk806SpiIomux (
  VOID
  )
{
  /* RK806 SPI Connection Init
   * Matches RK3588S EVB reference which LubanCat follows.
   * Configures PMU IO for PMIC control.
   */
  PMU1_IOC->GPIO0A_IOMUX_SEL_H = (0x0FF0UL << 16) | 0x0110;
  PMU1_IOC->GPIO0B_IOMUX_SEL_L = (0xF0FFUL << 16) | 0x1011;
  MmioWrite32 (NS_CRU_BASE + CRU_CLKSEL_CON59, (0x00C0UL << 16) | 0x0080);
}

VOID
EFIAPI
Rk806Configure (
  VOID
  )
{
  UINTN  RegCfgIndex;
  RK806Init ();
  RK806PinSetFunction (MASTER, 1, 2); 
  for (RegCfgIndex = 0; RegCfgIndex < ARRAY_SIZE (rk806_init_data); RegCfgIndex++) {
    RK806RegulatorInit (rk806_init_data[RegCfgIndex]);
  }
}

VOID
EFIAPI
SetCPULittleVoltage (
  IN UINT32  Microvolts
  )
{
  /* BUCK2: vdd_cpu_lit_s0 */
  struct regulator_init_data  Rk806CpuLittleSupply =
    RK8XX_VOLTAGE_INIT (MASTER_BUCK2, Microvolts);
  RK806RegulatorInit (Rk806CpuLittleSupply);
}

VOID
EFIAPI
NorFspiIomux (
  VOID
  )
{
  /* FSPI Init - Safe defaults for RK3588 */
  MmioWrite32 (NS_CRU_BASE + CRU_CLKSEL_CON78,
    (((0x3 << 12) | (0x3f << 6)) << 16) | (0x0 << 12) | (0x3f << 6));
  
  /* FSPI M1 */
  BUS_IOC->GPIO2A_IOMUX_SEL_H = (0xFF00UL << 16) | (0x3300);
  BUS_IOC->GPIO2B_IOMUX_SEL_L = (0xF0FFUL << 16) | (0x3033);
  BUS_IOC->GPIO2B_IOMUX_SEL_H = (0xF << 16) | (0x3);
}

VOID
EFIAPI
GmacIomux (
  IN UINT32  Id
  )
{
  switch (Id) {
    case 1:
      /* GMAC1 IOMUX (RGMII) */
      BUS_IOC->GPIO3B_IOMUX_SEL_H = (0x0FFFUL << 16) | 0x0111;
      BUS_IOC->GPIO3A_IOMUX_SEL_L = (0xFFFFUL << 16) | 0x1111;
      BUS_IOC->GPIO3B_IOMUX_SEL_L = (0xF0FFUL << 16) | 0x1011;
      BUS_IOC->GPIO3A_IOMUX_SEL_H = (0xF0FFUL << 16) | 0x1011;
      BUS_IOC->GPIO3C_IOMUX_SEL_L = (0xFF00UL << 16) | 0x1100;

      /* PHY1 Reset Pin: GPIO3_PB2 */
      GpioPinSetDirection (3, GPIO_PIN_PB2, GPIO_PIN_OUTPUT);
      break;
    default:
      break;
  }
}

VOID
EFIAPI
GmacIoPhyReset (
  UINT32   Id,
  BOOLEAN  Enable
  )
{
  switch (Id) {
    case 1:
      /* PHY1 Reset: GPIO3_PB2 (Active Low)
       * Enable=TRUE -> Assert Reset -> Write LOW (FALSE)
       */
      GpioPinWrite (3, GPIO_PIN_PB2, !Enable);
      break;
    default:
      break;
  }
}

VOID
EFIAPI
NorFspiEnableClock (
  UINT32  *CruBase
  )
{
  MmioWrite32 ((UINTN)CruBase + 0x087C, 0x0E000000);
}

VOID
EFIAPI
I2cIomux (
  UINT32  id
  )
{
  switch (id) {
    case 0:
      /* I2C0 M2 (Generic) */
      GpioPinSetFunction (0, GPIO_PIN_PD1, 3);
      GpioPinSetFunction (0, GPIO_PIN_PD2, 3);
      break;
    
    /* case 6: REMOVED. 
       DTS confirmed NO I2C6.
       Pins Reserved for PWM0_M2 (Fan). 
    */

    case 7: 
      /* ES8388 Audio Codec (I2C7 M0) Verified */
      GpioPinSetFunction (1, GPIO_PIN_PD0, 9); // SCL
      GpioPinSetFunction (1, GPIO_PIN_PD1, 9); // SDA
      break;
    default:
      break;
  }
}

VOID
EFIAPI
UsbPortPowerEnable (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "UsbPortPowerEnable called\n"));

  /* USB Power
   * USB 3.0 Host: GPIO1_PC4 (Active High)
   * USB 2.0 Host: GPIO1_PC6 (Active High)
   */

  // 1. Enable USB 3.0 Host Power
  GpioPinSetDirection (1, GPIO_PIN_PC4, GPIO_PIN_OUTPUT);
  GpioPinWrite (1, GPIO_PIN_PC4, TRUE);

  // 2. Enable USB 2.0 Host Power
  GpioPinSetDirection (1, GPIO_PIN_PC6, GPIO_PIN_OUTPUT);
  GpioPinWrite (1, GPIO_PIN_PC6, TRUE);

  gBS->Stall (50000); 
}

VOID
EFIAPI
Usb2PhyResume (
  VOID
  )
{
  MmioWrite32 (0xfd5d0008, 0x20000000);
  MmioWrite32 (0xfd5d4008, 0x20000000);
  MmioWrite32 (0xfd5d8008, 0x20000000);
  MmioWrite32 (0xfd5dc008, 0x20000000);
  MmioWrite32 (0xfd7f0a10, 0x07000700);
  MmioWrite32 (0xfd7f0a10, 0x07000000);
}

VOID
EFIAPI
PcieIoInit (
  UINT32  Segment
  )
{
  /* Mini-PCIe */
  if (Segment == PCIE_SEGMENT_PCIE20L2) {
    /* Reset: GPIO3_PD1 */
    GpioPinSetDirection (3, GPIO_PIN_PD1, GPIO_PIN_OUTPUT);
    
    /* Disable: GPIO3_PC6 (Set LOW to Enable) */
    GpioPinSetDirection (3, GPIO_PIN_PC6, GPIO_PIN_OUTPUT);
    GpioPinWrite (3, GPIO_PIN_PC6, FALSE);

    /* Power: GPIO1_PD5 */
    GpioPinSetDirection (1, GPIO_PIN_PD5, GPIO_PIN_OUTPUT);
  }
}

VOID
EFIAPI
PciePowerEn (
  UINT32   Segment,
  BOOLEAN  Enable
  )
{
  if (Segment == PCIE_SEGMENT_PCIE20L2) {
    /* vcc3v3_pcie20: GPIO1_PD5 Active High */
    GpioPinWrite (1, GPIO_PIN_PD5, Enable);
  }
}

VOID
EFIAPI
PciePeReset (
  UINT32   Segment,
  BOOLEAN  Enable
  )
{
  if (Segment == PCIE_SEGMENT_PCIE20L2) {
    /* Reset: GPIO3_PD1 Active High (Logic inverted for Reset func) */
    GpioPinWrite (3, GPIO_PIN_PD1, !Enable);
  }
}

VOID
EFIAPI
HdmiTxIomux (
  IN UINT32  Id
  )
{
  switch (Id) {
    case 0:
      GpioPinSetFunction (4, GPIO_PIN_PC1, 5); // hdmim0_tx0_cec
      GpioPinSetPull (4, GPIO_PIN_PC1, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (1, GPIO_PIN_PA5, 5); // hdmim0_tx0_hpd
      GpioPinSetPull (1, GPIO_PIN_PA5, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (4, GPIO_PIN_PB7, 5); // hdmim0_tx0_scl
      GpioPinSetPull (4, GPIO_PIN_PB7, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (4, GPIO_PIN_PC0, 5); // hdmim0_tx0_sda
      GpioPinSetPull (4, GPIO_PIN_PC0, GPIO_PIN_PULL_NONE);
      break;
  }
}

// --- Fan Configuration ---
PWM_DATA  pwm_data = {
  .ControllerID = PWM_CONTROLLER0, // DTS: pwm0
  .ChannelID    = PWM_CHANNEL0,    // Channel 0
  .PeriodNs     = 5000,
  .DutyNs       = 1000, // 20% Duty Cycle
  .Polarity     = FALSE,
};

VOID
EFIAPI
PwmFanIoSetup (
  VOID
  )
{
  /* Fan PinMux:
   * DTS: pinctrl-0 = <&pwm0m2_pins>
   * Map: PWM0_M2 -> GPIO1_PA2
   */
  GpioPinSetFunction (1, GPIO_PIN_PA2, 11);  
  
  RkPwmSetConfig (&pwm_data);
  RkPwmEnable (&pwm_data);
}

VOID
EFIAPI
PwmFanSetSpeed (
  IN UINT32  Percentage
  )
{
  pwm_data.DutyNs = pwm_data.PeriodNs * Percentage / 100;
  RkPwmSetConfig (&pwm_data);
}

VOID
EFIAPI
PlatformInitLeds (
  VOID
  )
{
  /* Sys LED: GPIO4_PB5 (Active Low)*/
  GpioPinSetDirection (4, GPIO_PIN_PB5, GPIO_PIN_OUTPUT);
  GpioPinWrite (4, GPIO_PIN_PB5, FALSE); // Turn ON (Low)
}

VOID
EFIAPI
PlatformSetStatusLed (
  IN BOOLEAN  Enable
  )
{
  GpioPinWrite (4, GPIO_PIN_PB5, !Enable);
}

CONST EFI_GUID *
EFIAPI
PlatformGetDtbFileGuid (
  IN UINT32  CompatMode
  )
{
  STATIC CONST EFI_GUID  VendorDtbFileGuid = {
    // DeviceTree/Vendor.inf
    0xd58b4028, 0x43d8, 0x4e97, { 0x87, 0xd4, 0x4e, 0x37, 0x16, 0x13, 0x65, 0x80 }
  };

  switch (CompatMode) {
    case FDT_COMPAT_MODE_VENDOR:
      return &VendorDtbFileGuid;
  }

  return NULL;
}

VOID
EFIAPI
PlatformEarlyInit (
  VOID
  )
{
  /* Audio Amp Enable */
  // GPIO1_PA6 (Active High)
  GpioPinSetDirection (1, GPIO_PIN_PA6, GPIO_PIN_OUTPUT);
  GpioPinWrite (1, GPIO_PIN_PA6, TRUE);

  /* 【NEW】 强制初始化风扇 */
  // 这会根据 pwm_data 的设置（目前是 100% 转速）立即启动风扇
  // 避免 CPU 在高负载下过热
  PwmFanIoSetup();
}