/** @file
*
* Copyright (c) 2021, Rockchip Limited. All rights reserved.
* Copyright (c) 2025, OrionisLi(CyanLi) <2254650260@qq.com>
* Copyright (c) 2026, OrionisLi <cyan2254650260@gmail.com>
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
#include <Library/TimerLib.h>
#include <Library/BaseLib.h>

#define I2C_BANK        0
#define I2C_SCL_PIN     GPIO_PIN_PD4
#define I2C_SDA_PIN     GPIO_PIN_PD5
#define I2C_SPEED_KHZ   100
#define I2C_HALF_DELAY  (500 / I2C_SPEED_KHZ)
#define I2C_RETRY_CNT   3

STATIC VOID SdaOut (BOOLEAN Level) {
  if (Level) {
    GpioPinSetDirection (I2C_BANK, I2C_SDA_PIN, GPIO_PIN_INPUT);
  } else {
    GpioPinWrite (I2C_BANK, I2C_SDA_PIN, FALSE);
    GpioPinSetDirection (I2C_BANK, I2C_SDA_PIN, GPIO_PIN_OUTPUT);
  }
  MicroSecondDelay (I2C_HALF_DELAY);
}

STATIC VOID SclOut (BOOLEAN Level) {
  if (Level) {
    GpioPinSetDirection (I2C_BANK, I2C_SCL_PIN, GPIO_PIN_INPUT);
  } else {
    GpioPinWrite (I2C_BANK, I2C_SCL_PIN, FALSE);
    GpioPinSetDirection (I2C_BANK, I2C_SCL_PIN, GPIO_PIN_OUTPUT);
  }
  MicroSecondDelay (I2C_HALF_DELAY);
}

STATIC VOID I2cStart (VOID) {
  SdaOut(TRUE); SclOut(TRUE); SdaOut(FALSE); MicroSecondDelay(I2C_HALF_DELAY); SclOut(FALSE);
}

STATIC VOID I2cStop (VOID) {
  SdaOut(FALSE); MicroSecondDelay(I2C_HALF_DELAY); SclOut(TRUE); MicroSecondDelay(I2C_HALF_DELAY); SdaOut(TRUE);
}

STATIC BOOLEAN I2cSendByte (UINT8 Data) {
  UINT8 i;
  BOOLEAN Ack;
  for (i = 0; i < 8; i++) {
    SdaOut((Data & 0x80) != 0);
    SclOut(TRUE); SclOut(FALSE);
    Data <<= 1;
  }
  SdaOut(TRUE);
  MicroSecondDelay(I2C_HALF_DELAY);
  SclOut(TRUE);
  Ack = !GpioPinRead(I2C_BANK, I2C_SDA_PIN);
  SclOut(FALSE);
  SdaOut(TRUE);
  return Ack;
}

STATIC EFI_STATUS Rk8602WriteReg (UINT8 Reg, UINT8 Val) {
  UINTN Try;
  BOOLEAN Ack;
  for (Try = 0; Try < I2C_RETRY_CNT; Try++) {
    I2cStart();
    Ack = I2cSendByte(0x42 << 1); if (!Ack) { I2cStop(); continue; }
    Ack = I2cSendByte(Reg);       if (!Ack) { I2cStop(); continue; }
    Ack = I2cSendByte(Val);       I2cStop();
    if (Ack) return EFI_SUCCESS;
    MicroSecondDelay(200);
  }
  return EFI_DEVICE_ERROR;
}

EFI_STATUS SafeInitNpu (VOID) {
  EFI_STATUS Status;

  DEBUG ((DEBUG_INFO, "SafeInitNpu: Configuring NPU power\n"));

  /* Configure GPIO as open-drain, initially input. */
  GpioPinSetFunction (I2C_BANK, I2C_SCL_PIN, 0);
  GpioPinSetFunction (I2C_BANK, I2C_SDA_PIN, 0);
  GpioPinSetDirection (I2C_BANK, I2C_SCL_PIN, GPIO_PIN_INPUT);
  GpioPinSetDirection (I2C_BANK, I2C_SDA_PIN, GPIO_PIN_INPUT);
  MicroSecondDelay(10);

  /* Set voltage to 0.8V. */
  Status = Rk8602WriteReg(0x06, 0x30);
  if (EFI_ERROR(Status)) {
    DEBUG ((DEBUG_ERROR, "SafeInitNpu: Set voltage failed - %r\n", Status));
    goto Exit;
  }
  DEBUG ((DEBUG_INFO, "SafeInitNpu: VOUT set to 0.8V\n"));
  MicroSecondDelay(200);

  /* Enable output. */
  Status = Rk8602WriteReg(0x05, 0x20);
  if (EFI_ERROR(Status)) {
    DEBUG ((DEBUG_ERROR, "SafeInitNpu: Enable output failed - %r\n", Status));
    goto Exit;
  }
  DEBUG ((DEBUG_INFO, "SafeInitNpu: Output enabled\n"));
  MicroSecondDelay(1000);

Exit:
  /* Release I2C bus, switch back to I2C function. */
  GpioPinSetDirection (I2C_BANK, I2C_SCL_PIN, GPIO_PIN_OUTPUT);
  GpioPinSetDirection (I2C_BANK, I2C_SDA_PIN, GPIO_PIN_OUTPUT);
  GpioPinWrite (I2C_BANK, I2C_SCL_PIN, TRUE);
  GpioPinWrite (I2C_BANK, I2C_SDA_PIN, TRUE);
  MicroSecondDelay(5);
  GpioPinSetFunction (I2C_BANK, I2C_SCL_PIN, 9);
  GpioPinSetFunction (I2C_BANK, I2C_SDA_PIN, 9);

  DEBUG ((DEBUG_INFO, "SafeInitNpu: %r\n", Status));

  DEBUG ((DEBUG_INFO, "[NPU] SafeInitNpu finished\n"));

  return Status;
}

/* PMIC configuration */
static struct regulator_init_data  rk806_init_data[] = {
  /* Master PMIC - Voltage Rails verified against DTS. */

  /* BUCK1 (vdd_gpu_s0): DTS range 550-950mV -> Set 750mV. */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK1,  750000),

  /* BUCK3 (vdd_log_s0): DTS range 675-800mV -> Set 750mV. */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK3,  750000),

  /* BUCK4 (vdd_vdenc_s0): DTS init 750mV -> Set 750mV. */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK4,  750000),

  /* BUCK5 (vdd_ddr_s0): DTS range 675-900mV -> Set 850mV. */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK5,  850000),

  /* BUCK6 (vdd2_ddr_s3): No voltage in DTS -> Skip. */
  /* RK8XX_VOLTAGE_INIT(MASTER_BUCK6, 750000), */

  /* BUCK7 (vcc_2v0_pldo_s3): DTS Fixed 2000mV -> Set 2000mV. */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK7,  2000000),

  /* BUCK8 (vcc_3v3_s3): DTS Fixed 3300mV -> Set 3300mV. */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK8,  3300000),

  /* BUCK10 (vcc_1v8_s3): DTS Fixed 1800mV -> Set 1800mV. */
  RK8XX_VOLTAGE_INIT (MASTER_BUCK10, 1800000),

  /* PLDO1 (vcc_1v8_s0): DTS Fixed 1800mV -> Set 1800mV. */
  RK8XX_VOLTAGE_INIT (MASTER_NLDO1,  750000),
  RK8XX_VOLTAGE_INIT (MASTER_NLDO2,  850000),   /* NLDO2 (avdd_ddr_pll_s0) 850mV. */
  RK8XX_VOLTAGE_INIT (MASTER_NLDO3,  750000),   /* NLDO3 (avdd_0v75_s0) 750mV. */
  RK8XX_VOLTAGE_INIT (MASTER_NLDO4,  850000),   /* NLDO4 (avdd_0v85_s0) 850mV. */
  /* NLDO5 does not exist in DTS, removed. */

  /* PLDO Configuration */
  RK8XX_VOLTAGE_INIT (MASTER_PLDO1,  1800000),  /* vcc_1v8_s0 */
  RK8XX_VOLTAGE_INIT (MASTER_PLDO2,  1800000),  /* avcc_1v8_s0 */
  RK8XX_VOLTAGE_INIT (MASTER_PLDO3,  1200000),  /* avdd_1v2_s0 */
  RK8XX_VOLTAGE_INIT (MASTER_PLDO4,  3300000),  /* avcc_3v3_s0 */
  RK8XX_VOLTAGE_INIT (MASTER_PLDO5,  3300000),  /* vccio_sd_s0 (Start at 3.3V for SD) */
  RK8XX_VOLTAGE_INIT (MASTER_PLDO6,  1800000),  /* pldo6_s3 */
};

/*
  Configure one I2C pin: function select, pull-none and Schmitt input.

  Matches the vendor pinctrl macro pcfg_pull_none_smt used by the i2c*_xfer
  nodes in rk3588s-pinctrl.dtsi.  Needed by the FUSB302 Type-C controller on
  I2C1, which MassStorageMode uses to detect the USB-C data role.
*/
STATIC
VOID
ConfigureI2cPin (
  IN UINT8  Group,
  IN UINT8  Pin,
  IN UINT8  Function
  )
{
  GpioPinSetFunction (Group, Pin, Function);
  GpioPinSetPull (Group, Pin, GPIO_PIN_PULL_NONE);
  GpioPinSetInput (Group, Pin, GPIO_PIN_INPUT_SCHMITT);
}

VOID
EFIAPI
SdmmcIoMux (
  VOID
  )
{
  /* TF Card IOMUX - Standard RK3588 */
  BUS_IOC->GPIO4D_IOMUX_SEL_L  = (0xFFFFUL << 16) | (0x1111); /* D0-D3 */
  BUS_IOC->GPIO4D_IOMUX_SEL_H  = (0x00FFUL << 16) | (0x0011); /* CLK, CMD */
  PMU1_IOC->GPIO0A_IOMUX_SEL_H = (0x000FUL << 16) | (0x0001); /* DET */
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

  /*
   * Do NOT add eMMC pinconf here.  The vendor FDT's mmc@fe2e0000 node has no
   * pinctrl-0, so Linux inherits whatever pin state the firmware leaves.
   * Overriding drive/pull on emmc_cmd and emmc_data_strobe caused an
   * "SError Interrupt ... pc : cqhci_request" panic under HS400 Enhanced
   * Strobe at 200 MHz.  eMMC already reaches HS400 with the pre-boot values.
   */
}

#define NS_CRU_BASE       0xFD7C0000
#define PMU1_CRU_BASE     0xFD7F0000
#define CRU_CLKSEL_CON41  0x03A4
#define CRU_CLKSEL_CON59  0x03EC
#define CRU_CLKSEL_CON78  0x0438
#define CRU_CLKGATE_CON12 0x0830
#define CRU_SOFTRST_CON12 0x0A30
#define PMU1_CRU_CLKSEL_CON2   0x0308
#define PMU1_CRU_CLKGATE_CON1  0x0804

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
  /* FSPI Init */
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

    case 1:
      /*
       * FUSB302 Type-C controller (I2C1 M2), used by MassStorageMode to read
       * the USB-C data role.  Vendor DTS i2c1m2_xfer is
       * <0 RK_PD4 9 &pcfg_pull_none_smt> and <0 RK_PD5 9 &pcfg_pull_none_smt>.
       */
      ConfigureI2cPin (0, GPIO_PIN_PD4, 9); /* SCL */
      ConfigureI2cPin (0, GPIO_PIN_PD5, 9); /* SDA */
      DEBUG ((DEBUG_INFO,
              "[I2C] Bus1 M2 GPIO0_PD4/PD5 configured (pull-none, Schmitt)\n"));
      break;

    /* case 6: REMOVED.
       DTS confirmed NO I2C6.
       Pins Reserved for PWM0_M2 (Fan). 
    */

    case 7:
      /* ES8388 Audio Codec (I2C7 M0) */
      GpioPinSetFunction (1, GPIO_PIN_PD0, 9); /* SCL */
      GpioPinSetFunction (1, GPIO_PIN_PD1, 9); /* SDA */
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

  /* 1. Enable USB 3.0 Host Power */
  GpioPinSetDirection (1, GPIO_PIN_PC4, GPIO_PIN_OUTPUT);
  GpioPinWrite (1, GPIO_PIN_PC4, TRUE);

  /* 2. Enable USB 2.0 Host Power */
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
      GpioPinSetFunction (4, GPIO_PIN_PC1, 5); /* hdmim0_tx0_cec */
      GpioPinSetPull (4, GPIO_PIN_PC1, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (1, GPIO_PIN_PA5, 5); /* hdmim0_tx0_hpd */
      GpioPinSetPull (1, GPIO_PIN_PA5, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (4, GPIO_PIN_PB7, 5); /* hdmim0_tx0_scl */
      GpioPinSetPull (4, GPIO_PIN_PB7, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (4, GPIO_PIN_PC0, 5); /* hdmim0_tx0_sda */
      GpioPinSetPull (4, GPIO_PIN_PC0, GPIO_PIN_PULL_NONE);
      break;
  }
}

/* Fan Configuration */
PWM_DATA  pwm_data = {
  .ControllerID = PWM_CONTROLLER0, /* DTS: pwm0 */
  .ChannelID    = PWM_CHANNEL0,    /* Channel 0 */
  .PeriodNs     = 5000,
  .DutyNs       = 1000,            /* 20% Duty Cycle */
  .Polarity     = FALSE,
};

VOID
EFIAPI
PwmFanIoSetup (
  VOID
  )
{
  /*
   * pwm0 uses CLK_PMU1PWM and PCLK_PMU1PWM on RK3588S.  PWMLib converts
   * nanoseconds to cycles assuming a 24 MHz source, so select xin24m and
   * explicitly ungate both clocks before touching the controller.
   */
  MmioWrite32 (
    PMU1_CRU_BASE + PMU1_CRU_CLKSEL_CON2,
    ((BIT9 | BIT10) << 16) | BIT10
    );
  MmioWrite32 (
    PMU1_CRU_BASE + PMU1_CRU_CLKGATE_CON1,
    (BIT12 | BIT13) << 16
    );

  /* Fan PinMux:
   * DTS: pinctrl-0 = <&pwm0m2_pins>
   * Map: PWM0_M2 -> GPIO1_PA2
   */
  GpioPinSetFunction (1, GPIO_PIN_PA2, 11);

  /* RK3588S PWM0 pin electrical configuration. */
  GpioPinSetPull (1, GPIO_PIN_PA2, GPIO_PIN_PULL_NONE);

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

#define TSADC_BASE                    0xFEC00000
#define TSADC_AUTO_CON                0x0004
#define TSADC_AUTO_SRC_CON            0x000C
#define TSADC_HSHUT_GPIO_INT_EN       0x0018
#define TSADC_HSHUT_CRU_INT_EN        0x001C
#define TSADC_DATA0                   0x002C
#define TSADC_COMP_SHUT(Channel)      (0x010C + ((Channel) * sizeof (UINT32)))
#define TSADC_HIGHT_INT_DEBOUNCE      0x014C
#define TSADC_HIGHT_TSHUT_DEBOUNCE    0x0150
#define TSADC_AUTO_PERIOD             0x0154
#define TSADC_AUTO_PERIOD_HT          0x0158

#define TSADC_CHANNEL_COUNT           7
#define TSADC_TSHUT_120C_CODE         389
#define TSADC_AUTO_PERIOD_2_5MS       5000

/* TSADC auto-conversion setup for RK3588/RK3588S.  The sequence mirrors
 * Linux v6.6 rockchip_thermal_probe(), rk_tsadcv8_initialize(),
 * rk_tsadcv3_tshut_temp()/mode(), and rk_tsadcv4_control().
 */
STATIC
VOID
TsadcInit (
  VOID
  )
{
  UINT32  Channel;
  UINT32  ChannelEnable;
  UINT32  ChannelMask;
  UINT32  ChannelWriteMask;

  /* CLK_TSADC = xin24m / 12 = 2 MHz (mux bit 8, divider bits 7:0). */
  MmioWrite32 (
    NS_CRU_BASE + CRU_CLKSEL_CON41,
    (0x01FFUL << 16) | BIT8 | 11
    );

  /* Ungate PCLK_TSADC and CLK_TSADC, then reset both APB and TSADC logic. */
  MmioWrite32 (
    NS_CRU_BASE + CRU_CLKGATE_CON12,
    (BIT0 | BIT1) << 16
    );
  MmioWrite32 (
    NS_CRU_BASE + CRU_SOFTRST_CON12,
    ((BIT0 | BIT1) << 16) | BIT0 | BIT1
    );
  MicroSecondDelay (10);
  MmioWrite32 (
    NS_CRU_BASE + CRU_SOFTRST_CON12,
    (BIT0 | BIT1) << 16
    );

  MmioWrite32 (TSADC_BASE + TSADC_AUTO_PERIOD,
               TSADC_AUTO_PERIOD_2_5MS);
  MmioWrite32 (TSADC_BASE + TSADC_AUTO_PERIOD_HT,
               TSADC_AUTO_PERIOD_2_5MS);
  MmioWrite32 (TSADC_BASE + TSADC_HIGHT_INT_DEBOUNCE, 4);
  MmioWrite32 (TSADC_BASE + TSADC_HIGHT_TSHUT_DEBOUNCE, 4);

  /* AUTO_CON uses Rockchip's high-word write mask: clear polarity bit 8. */
  MmioWrite32 (TSADC_BASE + TSADC_AUTO_CON, BIT24);

  /* Match the RK3588S DTS: all seven sensors shut down through CRU at 120 C.
   * Program each safe comparator before enabling its conversion source.
   */
  for (Channel = 0; Channel < TSADC_CHANNEL_COUNT; Channel++) {
    ChannelMask      = 1U << Channel;
    ChannelWriteMask = 1U << (16 + Channel);
    ChannelEnable    = ChannelMask | ChannelWriteMask;
    MmioWrite32 (TSADC_BASE + TSADC_HSHUT_GPIO_INT_EN,
                 ChannelWriteMask);
    MmioWrite32 (TSADC_BASE + TSADC_HSHUT_CRU_INT_EN, ChannelEnable);
    MmioWrite32 (TSADC_BASE + TSADC_COMP_SHUT (Channel),
                 TSADC_TSHUT_120C_CODE);
    MmioWrite32 (TSADC_BASE + TSADC_AUTO_SRC_CON, ChannelEnable);
  }

  /* Enable continuous auto conversion (also a high-word masked register). */
  MmioWrite32 (TSADC_BASE + TSADC_AUTO_CON, BIT16 | BIT0);

  MicroSecondDelay (20000);
  DEBUG ((DEBUG_INFO, "[TSADC] DATA0=%u (expect ~285 at 25C)\n",
          MmioRead32 (TSADC_BASE + TSADC_DATA0) & 0x1FF));
}

VOID
EFIAPI
PlatformInitLeds (
  VOID
  )
{
  /* Sys LED: GPIO4_PB5 (Active Low) */
  GpioPinSetDirection (4, GPIO_PIN_PB5, GPIO_PIN_OUTPUT);
  GpioPinWrite (4, GPIO_PIN_PB5, FALSE); /* Turn ON (Low) */
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
    /* DeviceTree/Vendor.inf */
    0xd58b4028, 0x43d8, 0x4e97, { 0x87, 0xd4, 0x4e, 0x37, 0x16, 0x13, 0x65, 0x80 }
  };
  STATIC CONST EFI_GUID  MainlineDtbFileGuid = {
    /* DeviceTree/Mainline.inf */
    0x84492e97, 0xa10f, 0x49a7, { 0x85, 0xe9, 0x02, 0x5d, 0x19, 0x66, 0xb3, 0x43 }
  };

  switch (CompatMode) {
    case FDT_COMPAT_MODE_VENDOR:
      return &VendorDtbFileGuid;
    case FDT_COMPAT_MODE_MAINLINE:
      return &MainlineDtbFileGuid;
  }

  return NULL;
}

STATIC
EFI_STATUS
SafeInitGpu (
  VOID
  )
{
  UINT32 Timeout;
  UINT32 GpuId;

  DEBUG ((DEBUG_INFO, "[GPU] SafeInitGpu: Powering on Mali-G610...\n"));

  /*
   * Step 1: Power domain PD_GPU (id = 0) via PMU2
   * SYS_PMU_BASE = 0xFD8D8000
   * PMU2_PWR_GATE_SFTCON(0) @ 0x014C: bit 0 active-low (0 = power on)
   */
  MmioWrite32 (SYS_PMU_BASE + 0x014C, (1U << 16) | 0U);

  /* Poll PMU2_PWR_GATE_ST(0) @ 0x0180 bit 0: wait until 0 (powered on) */
  Timeout = 1000;
  while ((MmioRead32 (SYS_PMU_BASE + 0x0180) & BIT0) != 0) {
    MicroSecondDelay (10);
    if (--Timeout == 0) {
      DEBUG ((DEBUG_ERROR, "[GPU] ERROR: PD_GPU power gate timeout!\n"));
      return EFI_DEVICE_ERROR;
    }
  }

  /* Release GPU internal SRAM power gate: PMU2_MEMPWR_GATE_SFTCON(0) @ 0x01A0 */
  MmioWrite32 (SYS_PMU_BASE + 0x01A0, (1U << 16) | 0U);

  /* Release bus idle request: PMU2_BUS_IDLE_SFTCON(0) @ 0x010C: bit 0 = 0 (active) */
  MmioWrite32 (SYS_PMU_BASE + 0x010C, (1U << 16) | 0U);

  /* Poll PMU2_BUS_IDLE_ST(0) @ 0x0120 bit 0: wait until 0 (bus active) */
  Timeout = 1000;
  while ((MmioRead32 (SYS_PMU_BASE + 0x0120) & BIT0) != 0) {
    MicroSecondDelay (10);
    if (--Timeout == 0) {
      DEBUG ((DEBUG_ERROR, "[GPU] ERROR: BUS_ID_GPU idle release timeout!\n"));
      return EFI_DEVICE_ERROR;
    }
  }

  /*
   * Step 2: CRU Clocks
   * Set GPU clock from GPLL (1188MHz / 6 = 198MHz)
   * CLKSEL_CON158 @ 0x0578: bits[4:0]=div(5), bits[7:5]=GPLL(0), bit 14=normal PLL(0)
   */
  DEBUG ((DEBUG_INFO, "[GPU] Enabling Mali-G610 clocks...\n"));
  MmioWrite32 (CRU_BASE + 0x0578, (0x001FU << 16) | 5U);
  MmioWrite32 (CRU_BASE + 0x0578, (0x00E0U << 16) | (0U << 5));
  MmioWrite32 (CRU_BASE + 0x0578, (1U << 30) | (0U << 14));

  /* CLKGATE_CON66 @ 0x0908: Core, NIU, stack clocks */
  MmioWrite32 (CRU_BASE + 0x0908, 0xFFFF0000U);

  /* CLKGATE_CON67 @ 0x090C: GRF & PVTM clocks */
  MmioWrite32 (CRU_BASE + 0x090C, 0xFFFF0000U);
  MicroSecondDelay (20);

  /*
   * Step 3: CRU Soft Reset Deassertion
   * SOFTRST_CON66 @ 0x0B08: GPU Core & NIU resets
   * SOFTRST_CON67 @ 0x0B0C: GPU GRF & PVTM resets
   */
  DEBUG ((DEBUG_INFO, "[GPU] Deasserting Mali-G610 resets...\n"));
  MmioWrite32 (CRU_BASE + 0x0B08, 0xFFFF0000U);
  MmioWrite32 (CRU_BASE + 0x0B0C, 0xFFFF0000U);
  MicroSecondDelay (100);

  /*
   * Step 4: Hardware ID probe at 0xFB000000
   */
  GpuId = MmioRead32 (0xFB000000U);
  DEBUG ((DEBUG_INFO, "[GPU] SUCCESS: Read Mali-G610 GPU_ID @ 0xFB000000 = 0x%08X (Arch=%u, Product=0x%04X)\n",
          GpuId, (GpuId >> 28) & 0xFU, (GpuId >> 16) & 0xFFFFU));

  return EFI_SUCCESS;
}

VOID
EFIAPI
PlatformEarlyInit (
  VOID
  )
{
  EFI_STATUS Status;

  DEBUG ((DEBUG_INFO, "[PLATFORM] PlatformEarlyInit started\n"));

  /*
   * FUSB302 IRQ.  Board DTS fusb302_int (rk3588s-lubancat-4.dts:1517) is
   * <0 RK_PC6 RK_FUNC_GPIO &pcfg_pull_up>, i.e. function 0 + pull-up + input,
   * matching the three calls below.  Note the LubanCat-4IO variant uses PD3
   * instead; PC6 is correct for lbc-4.
   */
  GpioPinSetFunction (0, GPIO_PIN_PC6, 0); /* FUSB302 IRQ */
  GpioPinSetPull (0, GPIO_PIN_PC6, GPIO_PIN_PULL_UP);
  GpioPinSetDirection (0, GPIO_PIN_PC6, GPIO_PIN_INPUT);
  DEBUG ((DEBUG_INFO,
          "[TYPEC] FUSB302 IRQ GPIO0_PC6 configured (pull-up, state=%u)\n",
          GpioPinReadActual (0, GPIO_PIN_PC6)));

  /* Audio Amp Enable */
  GpioPinSetDirection (1, GPIO_PIN_PA6, GPIO_PIN_OUTPUT);
  GpioPinWrite (1, GPIO_PIN_PA6, TRUE);

  /* Fan initialization */
  PwmFanIoSetup();

  /* Start TSADC for the ACPI fan thermal zone. */
  TsadcInit();

  Status = SafeInitNpu();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FATAL: NPU power init failed - system halted.\n"));
    CpuDeadLoop();
  }

  /* NPU clock enable */
  DEBUG ((DEBUG_INFO, "[NPU] Enabling NPU clocks...\n"));

  /* HCLK_NPU_ROOT (Bit 0) + PCLK_NPU_ROOT (Bit 1) + ACLK_NPU_ROOT (Bit 2) */
  MmioWrite32 (CRU_BASE + 0x874, (BIT(0) | BIT(1) | BIT(2)) << 16);

  /* ACLK_NPU0 + HCLK_NPU0 (CLKGATE_CON30 @ 0x878, bits 6 & 8) */
  MmioWrite32 (CRU_BASE + 0x878, (BIT(6) | BIT(8)) << 16);  /* Write 1 to clear gate (enable clock) */

  /* ACLK_NPU1 + HCLK_NPU1 (CLKGATE_CON27 @ 0x86C, bits 0 & 2) */
  MmioWrite32 (CRU_BASE + 0x86C, (BIT(0) | BIT(2)) << 16);

  /* ACLK_NPU2 + HCLK_NPU2 (CLKGATE_CON28 @ 0x870, bits 0 & 2) */
  MmioWrite32 (CRU_BASE + 0x870, (BIT(0) | BIT(2)) << 16);

  MicroSecondDelay (10);

  /* NPU reset release */
  DEBUG ((DEBUG_INFO, "[NPU] Deasserting NPU resets...\n"));

  /* SOFTRST_CON30 @ 0x0A78, bits 6-11 */
  MmioWrite32 (CRU_BASE + 0x0A78, (BIT(6) | BIT(7) | BIT(8) | BIT(9) | BIT(10) | BIT(11)) << 16);

  MicroSecondDelay (100);

  /* GPU (Mali-G610) Initialization */
  Status = SafeInitGpu();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[GPU] WARNING: GPU init failed: %r\n", Status));
  }

  DEBUG ((DEBUG_INFO, "[PLATFORM] PlatformEarlyInit done\n"));

}
