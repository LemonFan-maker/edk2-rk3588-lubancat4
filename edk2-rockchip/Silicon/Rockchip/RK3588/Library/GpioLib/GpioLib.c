/** @file
 *
 *  RK3588 GPIO Library.
 *
 *  Copyright (c) 2021, Jared McNeill <jmcneill@invisible.ca>
 *  Copyright (c) 2023, Molly Sophia <mollysophia379@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/GpioLib.h>

#define GRF_GPIO_IOMUX_REG(Pin)    (((Pin) / 4) * 4)
#define GRF_GPIO_IOMUX_SHIFT(Pin)  (((Pin) % 4) * 4)
#define GRF_GPIO_IOMUX_MASK(Pin)   (0xFU << (GRF_GPIO_IOMUX_SHIFT(Pin) + 16))

/*
 * RK3588 pinconf field widths, per the vendor kernel 6.1
 * (drivers/pinctrl/pinctrl-rockchip.c):
 *
 *   RK3588_PULL_BITS_PER_PIN 2   RK3588_PULL_PINS_PER_REG 8
 *   RK3588_DRV_BITS_PER_PIN  4   RK3588_DRV_PINS_PER_REG  4
 *   RK3588_SMT_BITS_PER_PIN  1   RK3588_SMT_PINS_PER_REG  8
 *
 * All three register groups are write-mask style: the high 16 bits enable
 * the corresponding low 16 bits.
 */
#define GRF_GPIO_P_REG(Pin)    (((Pin) / 8) * 4)
#define GRF_GPIO_P_SHIFT(Pin)  (((Pin) % 8) * 2)
#define GRF_GPIO_P_MASK(Pin)   (0x3U << (GRF_GPIO_P_SHIFT (Pin) + 16))

/*
 * Drive strength is 4 bits per pin on RK3588, 4 pins per register.
 * The previous 0xFF mask was the RK3568 layout (8 bits per pin) and made
 * every write clobber the neighbouring pin's field.
 */
#define GRF_GPIO_DS_REG(Pin)    (((Pin) / 4) * 4)
#define GRF_GPIO_DS_SHIFT(Pin)  (((Pin) % 4) * 4)
#define GRF_GPIO_DS_MASK(Pin)   (0xFU << (GRF_GPIO_DS_SHIFT (Pin) + 16))

/*
 * Schmitt trigger (input) is 1 bit per pin on RK3588, 8 pins per register,
 * and lives in its own register group (PMU1_IOC + 0x0030, VCCIO* + 0x0200).
 */
#define GRF_GPIO_SMT_REG(Pin)    (((Pin) / 8) * 4)
#define GRF_GPIO_SMT_SHIFT(Pin)  ((Pin) % 8)
#define GRF_GPIO_SMT_MASK(Pin)   (0x1U << (GRF_GPIO_SMT_SHIFT (Pin) + 16))

#if 0
/*
 * Kept for reference only. These were the RK3568 slew-rate register offsets
 * (RK3568_SR_GRF_OFFSET == 0x0180) combined with the RK3568 2-bit Schmitt
 * model. Neither applies to RK3588; see mSmtReg[] below.
 */
#define GRF_GPIO_IE_REG(Pin)    (((Pin) / 8) * 4)
#define GRF_GPIO_IE_SHIFT(Pin)  (((Pin) % 8) * 2)
#define GRF_GPIO_IE_MASK(Pin)   (0x3U << (GRF_GPIO_IE_SHIFT (Pin) + 16))
#endif

#define GPIO_SWPORT_DR(Pin)   ((Pin) < 16 ? 0x0000 : 0x0004)
#define GPIO_SWPORT_DDR(Pin)  ((Pin) < 16 ? 0x0008 : 0x000C)

#define GPIO_WRITE_MASK(Pin)         (1U << (((Pin) % 16) + 16))
#define GPIO_VALUE_MASK(Pin, Value)  ((UINT32)Value << ((Pin) % 16))

#define GPIO_EXT_PORT  0x0070

#define GPIO_NGROUPS  5

#define PMU1_IOC_BASE      (0xFD5F0000U)
#define PMU2_IOC_BASE      (PMU1_IOC_BASE + 0x4000)
#define BUS_IOC_BASE       (PMU1_IOC_BASE + 0x8000)
#define VCCIO1_4_IOC_BASE  (PMU1_IOC_BASE + 0x9000)
#define VCCIO3_5_IOC_BASE  (PMU1_IOC_BASE + 0xA000)
#define VCCIO2_IOC_BASE    (PMU1_IOC_BASE + 0xB000)
#define VCCIO6_IOC_BASE    (PMU1_IOC_BASE + 0xC000)
#define EMMC_IOC_BASE      (PMU1_IOC_BASE + 0xD000)

typedef struct {
  UINT8                   Group;
  UINT8                   Pin;
  EFI_PHYSICAL_ADDRESS    Addr;
} GPIO_REG;

STATIC GPIO_REG  mDsReg[] = {
  { 0, GPIO_PIN_PA0, PMU1_IOC_BASE + 0x0010     },
  { 0, GPIO_PIN_PA4, PMU1_IOC_BASE + 0x0014     },
  { 0, GPIO_PIN_PB0, PMU1_IOC_BASE + 0x0018     },
  { 0, GPIO_PIN_PB4, PMU2_IOC_BASE + 0x0014     },
  { 0, GPIO_PIN_PC0, PMU2_IOC_BASE + 0x0018     },
  { 0, GPIO_PIN_PC4, PMU2_IOC_BASE + 0x001C     },
  { 0, GPIO_PIN_PD0, PMU2_IOC_BASE + 0x0020     },
  { 0, GPIO_PIN_PD4, PMU2_IOC_BASE + 0x0024     },
  { 1, GPIO_PIN_PA0, VCCIO1_4_IOC_BASE + 0x0020 },
  { 1, GPIO_PIN_PA4, VCCIO1_4_IOC_BASE + 0x0024 },
  { 1, GPIO_PIN_PB0, VCCIO1_4_IOC_BASE + 0x0028 },
  { 1, GPIO_PIN_PB4, VCCIO1_4_IOC_BASE + 0x002C },
  { 1, GPIO_PIN_PC0, VCCIO1_4_IOC_BASE + 0x0030 },
  { 1, GPIO_PIN_PC4, VCCIO1_4_IOC_BASE + 0x0034 },
  { 1, GPIO_PIN_PD0, VCCIO1_4_IOC_BASE + 0x0038 },
  { 1, GPIO_PIN_PD4, VCCIO1_4_IOC_BASE + 0x003C },
  { 2, GPIO_PIN_PA0, EMMC_IOC_BASE + 0x0040     },
  { 2, GPIO_PIN_PA4, VCCIO3_5_IOC_BASE + 0x0044 },
  { 2, GPIO_PIN_PB0, VCCIO3_5_IOC_BASE + 0x0048 },
  { 2, GPIO_PIN_PB4, VCCIO3_5_IOC_BASE + 0x004C },
  { 2, GPIO_PIN_PC0, VCCIO3_5_IOC_BASE + 0x0050 },
  { 2, GPIO_PIN_PC4, VCCIO3_5_IOC_BASE + 0x0054 },
  { 2, GPIO_PIN_PD0, EMMC_IOC_BASE + 0x0058     },
  { 2, GPIO_PIN_PD4, EMMC_IOC_BASE + 0x005C     },
  { 3, GPIO_PIN_PA0, VCCIO3_5_IOC_BASE + 0x0060 },
  { 3, GPIO_PIN_PA4, VCCIO3_5_IOC_BASE + 0x0064 },
  { 3, GPIO_PIN_PB0, VCCIO3_5_IOC_BASE + 0x0068 },
  { 3, GPIO_PIN_PB4, VCCIO3_5_IOC_BASE + 0x006C },
  { 3, GPIO_PIN_PC0, VCCIO3_5_IOC_BASE + 0x0070 },
  { 3, GPIO_PIN_PC4, VCCIO3_5_IOC_BASE + 0x0074 },
  { 3, GPIO_PIN_PD0, VCCIO3_5_IOC_BASE + 0x0078 },
  { 3, GPIO_PIN_PD4, VCCIO3_5_IOC_BASE + 0x007C },
  { 4, GPIO_PIN_PA0, VCCIO6_IOC_BASE + 0x0080   },
  { 4, GPIO_PIN_PA4, VCCIO6_IOC_BASE + 0x0084   },
  { 4, GPIO_PIN_PB0, VCCIO6_IOC_BASE + 0x0088   },
  { 4, GPIO_PIN_PB4, VCCIO6_IOC_BASE + 0x008C   },
  { 4, GPIO_PIN_PC0, VCCIO6_IOC_BASE + 0x0090   },
  { 4, GPIO_PIN_PC2, VCCIO3_5_IOC_BASE + 0x0090 },
  { 4, GPIO_PIN_PC4, VCCIO3_5_IOC_BASE + 0x0094 },
  { 4, GPIO_PIN_PD0, VCCIO2_IOC_BASE + 0x0098   },
};

STATIC GPIO_REG  mPullReg[] = {
  { 0, GPIO_PIN_PA0, PMU1_IOC_BASE + 0x0020     },
  { 0, GPIO_PIN_PB0, PMU1_IOC_BASE + 0x0024     },
  { 0, GPIO_PIN_PB5, PMU2_IOC_BASE + 0x0028     },
  { 0, GPIO_PIN_PC0, PMU2_IOC_BASE + 0x002C     },
  { 0, GPIO_PIN_PD0, PMU2_IOC_BASE + 0x0030     },
  { 1, GPIO_PIN_PA0, VCCIO1_4_IOC_BASE + 0x0110 },
  { 1, GPIO_PIN_PB0, VCCIO1_4_IOC_BASE + 0x0114 },
  { 1, GPIO_PIN_PC0, VCCIO1_4_IOC_BASE + 0x0118 },
  { 1, GPIO_PIN_PD0, VCCIO1_4_IOC_BASE + 0x011C },
  { 2, GPIO_PIN_PA0, EMMC_IOC_BASE + 0x0120     },
  { 2, GPIO_PIN_PA4, VCCIO3_5_IOC_BASE + 0x0120 },
  { 2, GPIO_PIN_PB0, VCCIO3_5_IOC_BASE + 0x0124 },
  { 2, GPIO_PIN_PC0, VCCIO3_5_IOC_BASE + 0x0128 },
  { 2, GPIO_PIN_PD0, EMMC_IOC_BASE + 0x012C     },
  { 3, GPIO_PIN_PA0, VCCIO3_5_IOC_BASE + 0x0130 },
  { 3, GPIO_PIN_PB0, VCCIO3_5_IOC_BASE + 0x0134 },
  { 3, GPIO_PIN_PC0, VCCIO3_5_IOC_BASE + 0x0138 },
  { 3, GPIO_PIN_PD0, VCCIO3_5_IOC_BASE + 0x013C },
  { 4, GPIO_PIN_PA0, VCCIO6_IOC_BASE + 0x0140   },
  { 4, GPIO_PIN_PB0, VCCIO6_IOC_BASE + 0x0144   },
  { 4, GPIO_PIN_PC0, VCCIO6_IOC_BASE + 0x0148   },
  { 4, GPIO_PIN_PC2, VCCIO3_5_IOC_BASE + 0x0148 },
  { 4, GPIO_PIN_PD0, VCCIO2_IOC_BASE + 0x014C   },
};

/*
 * RK3588 Schmitt trigger register boundaries, transcribed from
 * rk3588_smt_regs[] in the vendor kernel 6.1
 * (drivers/pinctrl/pinctrl-rockchip.c).
 */
STATIC GPIO_REG  mSmtReg[] = {
  { 0, GPIO_PIN_PA0, PMU1_IOC_BASE + 0x0030     },
  { 0, GPIO_PIN_PB0, PMU1_IOC_BASE + 0x0034     },
  { 0, GPIO_PIN_PB5, PMU2_IOC_BASE + 0x0040     },
  { 0, GPIO_PIN_PC0, PMU2_IOC_BASE + 0x0044     },
  { 0, GPIO_PIN_PD0, PMU2_IOC_BASE + 0x0048     },
  { 1, GPIO_PIN_PA0, VCCIO1_4_IOC_BASE + 0x0210 },
  { 1, GPIO_PIN_PB0, VCCIO1_4_IOC_BASE + 0x0214 },
  { 1, GPIO_PIN_PC0, VCCIO1_4_IOC_BASE + 0x0218 },
  { 1, GPIO_PIN_PD0, VCCIO1_4_IOC_BASE + 0x021C },
  { 2, GPIO_PIN_PA0, EMMC_IOC_BASE + 0x0220     },
  { 2, GPIO_PIN_PA4, VCCIO3_5_IOC_BASE + 0x0220 },
  { 2, GPIO_PIN_PB0, VCCIO3_5_IOC_BASE + 0x0224 },
  { 2, GPIO_PIN_PC0, VCCIO3_5_IOC_BASE + 0x0228 },
  { 2, GPIO_PIN_PD0, EMMC_IOC_BASE + 0x022C     },
  { 3, GPIO_PIN_PA0, VCCIO3_5_IOC_BASE + 0x0230 },
  { 3, GPIO_PIN_PB0, VCCIO3_5_IOC_BASE + 0x0234 },
  { 3, GPIO_PIN_PC0, VCCIO3_5_IOC_BASE + 0x0238 },
  { 3, GPIO_PIN_PD0, VCCIO3_5_IOC_BASE + 0x023C },
  { 4, GPIO_PIN_PA0, VCCIO6_IOC_BASE + 0x0240   },
  { 4, GPIO_PIN_PB0, VCCIO6_IOC_BASE + 0x0244   },
  { 4, GPIO_PIN_PC0, VCCIO6_IOC_BASE + 0x0248   },
  { 4, GPIO_PIN_PC2, VCCIO3_5_IOC_BASE + 0x0248 },
  { 4, GPIO_PIN_PD0, VCCIO2_IOC_BASE + 0x024C   },
};

#if 0
/*
 * Kept for reference only. This table was derived from the RK3568 slew-rate
 * register block (RK3568_SR_GRF_OFFSET == 0x0180, bank stride 0x10), not from
 * any RK3588 register group. No RK3588 reference implementation (vendor 6.1,
 * mainline 6.12, U-Boot) defines a per-pin input-enable table; the Schmitt
 * trigger registers in mSmtReg[] are what pinconf actually drives.
 */
STATIC GPIO_REG  mIEReg[] = {
  { 0, GPIO_PIN_PA0, PMU1_IOC_BASE + 0x0028     },
  { 0, GPIO_PIN_PB0, PMU1_IOC_BASE + 0x002C     },
  { 0, GPIO_PIN_PB5, PMU2_IOC_BASE + 0x0034     },
  { 0, GPIO_PIN_PC0, PMU2_IOC_BASE + 0x0038     },
  { 0, GPIO_PIN_PD0, PMU2_IOC_BASE + 0x003C     },
  { 1, GPIO_PIN_PA0, VCCIO1_4_IOC_BASE + 0x0180 },
  { 1, GPIO_PIN_PB0, VCCIO1_4_IOC_BASE + 0x0184 },
  { 1, GPIO_PIN_PC0, VCCIO1_4_IOC_BASE + 0x0188 },
  { 1, GPIO_PIN_PD0, VCCIO1_4_IOC_BASE + 0x018C },
  { 2, GPIO_PIN_PA0, EMMC_IOC_BASE + 0x0190     },
  { 2, GPIO_PIN_PA4, VCCIO3_5_IOC_BASE + 0x0190 },
  { 2, GPIO_PIN_PB0, VCCIO3_5_IOC_BASE + 0x0194 },
  { 2, GPIO_PIN_PC0, VCCIO3_5_IOC_BASE + 0x0198 },
  { 2, GPIO_PIN_PD0, EMMC_IOC_BASE + 0x019C     },
  { 3, GPIO_PIN_PA0, VCCIO3_5_IOC_BASE + 0x01A0 },
  { 3, GPIO_PIN_PB0, VCCIO3_5_IOC_BASE + 0x01A4 },
  { 3, GPIO_PIN_PC0, VCCIO3_5_IOC_BASE + 0x01A8 },
  { 3, GPIO_PIN_PD0, VCCIO3_5_IOC_BASE + 0x01AC },
  { 4, GPIO_PIN_PA0, VCCIO6_IOC_BASE + 0x01B0   },
  { 4, GPIO_PIN_PB0, VCCIO6_IOC_BASE + 0x01B4   },
  { 4, GPIO_PIN_PC0, VCCIO6_IOC_BASE + 0x01B8   },
  { 4, GPIO_PIN_PC2, VCCIO3_5_IOC_BASE + 0x01B8 },
  { 4, GPIO_PIN_PD0, VCCIO2_IOC_BASE + 0x01BC   },
};
#endif

#define GPIO_BASE(n)  ((n) == 0 ? 0xFD8A0000UL : (0xFEC20000UL + ((n) - 1) * 0x10000))

/**
  Resolve the IOC register that owns a pin's pinconf field.

  Walks the boundary table backwards and stops at the first entry covering the
  pin, mirroring rk3588_calc_*_reg_and_bit() in the vendor kernel. Stopping is
  mandatory: several groups are split across separate IOC blocks (GPIO0 between
  PMU1 and PMU2, GPIO2 between EMMC and VCCIO3_5, GPIO4 between VCCIO6,
  VCCIO3_5 and VCCIO2). Continuing past the match walks back into a lower
  entry and yields an address in the wrong IOC block.

  @param[in]  Table       Boundary table, ascending by Group then Pin.
  @param[in]  TableCount  Number of entries in Table.
  @param[in]  Group       GPIO group.
  @param[in]  Pin         Pin within the group.
  @param[in]  PinsPerReg  Pins covered by one register in this group.
  @param[out] Reg         Resolved register address. Untouched on miss.

  @retval TRUE   Reg holds the address owning this pin.
  @retval FALSE  No entry covers the pin; Reg is not valid.
**/
STATIC
BOOLEAN
GpioPinconfLookupReg (
  IN  CONST GPIO_REG        *Table,
  IN  UINTN                 TableCount,
  IN  UINT8                 Group,
  IN  UINT8                 Pin,
  IN  UINTN                 PinsPerReg,
  OUT EFI_PHYSICAL_ADDRESS  *Reg
  )
{
  INTN  Idx;

  for (Idx = (INTN)TableCount - 1; Idx >= 0; Idx--) {
    if ((Group == Table[Idx].Group) && (Pin >= Table[Idx].Pin)) {
      *Reg = Table[Idx].Addr + (((Pin - Table[Idx].Pin) / PinsPerReg) * 4);
      return TRUE;
    }
  }

  return FALSE;
}

VOID
GpioPinSetDirection (
  IN UINT8               Group,
  IN UINT8               Pin,
  IN GPIO_PIN_DIRECTION  Direction
  )
{
  DEBUG ((
    DEBUG_INFO,
    "GpioPinSetDirection Group:%d Pin:%d Direction:%d\n",
    Group,
    Pin,
    Direction
    ));
  MmioWrite32 (
    GPIO_BASE (Group) + GPIO_SWPORT_DDR (Pin),
    GPIO_WRITE_MASK (Pin) | GPIO_VALUE_MASK (Pin, Direction)
    );
}

VOID
GpioPinWrite (
  IN UINT8    Group,
  IN UINT8    Pin,
  IN BOOLEAN  Value
  )
{
  DEBUG ((
    DEBUG_VERBOSE,
    "GpioPinWrite Group:%d Pin:%d Value:%d\n",
    Group,
    Pin,
    Value
    ));
  MmioWrite32 (
    GPIO_BASE (Group) + GPIO_SWPORT_DR (Pin),
    GPIO_WRITE_MASK (Pin) | GPIO_VALUE_MASK (Pin, Value)
    );
}

BOOLEAN
GpioPinRead (
  IN UINT8  Group,
  IN UINT8  Pin
  )
{
  CONST UINT32  Value = MmioRead32 (GPIO_BASE (Group) + GPIO_SWPORT_DR (Pin));

  return (Value & GPIO_VALUE_MASK (Pin, 1)) != 0;
}

BOOLEAN
GpioPinReadActual (
  IN UINT8  Group,
  IN UINT8  Pin
  )
{
  CONST UINT32  Value = MmioRead32 (GPIO_BASE (Group) + GPIO_EXT_PORT);

  return (Value & (1 << Pin)) != 0;
}

VOID
GpioPinSetFunction (
  IN UINT8  Group,
  IN UINT8  Pin,
  IN UINT8  Function
  )
{
  ASSERT (Group < GPIO_NGROUPS);

  EFI_PHYSICAL_ADDRESS  Reg;
  UINT32                Value = GRF_GPIO_IOMUX_MASK (Pin) | ((UINT32)Function << GRF_GPIO_IOMUX_SHIFT (Pin));

  DEBUG ((DEBUG_INFO, "GpioPinSetFunction Group:%d Pin:%d Function:%d\n", Group, Pin, Function));

  if (Group == 0) {
    if (Pin >= GPIO_PIN_PB4) {
      if (Function >= 8) {
        Reg   = PMU2_IOC_BASE - 0xC + GRF_GPIO_IOMUX_REG (Pin);
        Value = GRF_GPIO_IOMUX_MASK (Pin) | ((UINT32)8 << GRF_GPIO_IOMUX_SHIFT (Pin));
        MmioWrite32 (Reg, Value);

        Reg   = BUS_IOC_BASE + GRF_GPIO_IOMUX_REG (Pin);
        Value = GRF_GPIO_IOMUX_MASK (Pin) | ((UINT32)Function << GRF_GPIO_IOMUX_SHIFT (Pin));
        MmioWrite32 (Reg, Value);
      } else {
        Reg   = PMU2_IOC_BASE - 0xC + GRF_GPIO_IOMUX_REG (Pin);
        Value = GRF_GPIO_IOMUX_MASK (Pin) | ((UINT32)Function << GRF_GPIO_IOMUX_SHIFT (Pin));
        MmioWrite32 (Reg, Value);

        Reg   = BUS_IOC_BASE + GRF_GPIO_IOMUX_REG (Pin);
        Value = GRF_GPIO_IOMUX_MASK (Pin);
        MmioWrite32 (Reg, Value);
      }

      DEBUG ((DEBUG_VERBOSE, "Reg - Value: 0x%lX = 0x%08X\n", Reg, Value));
      return;
    } else {
      Reg = PMU1_IOC_BASE + GRF_GPIO_IOMUX_REG (Pin);
    }
  } else {
    Reg = BUS_IOC_BASE + Group * 0x20 + GRF_GPIO_IOMUX_REG (Pin);
  }

  DEBUG ((DEBUG_VERBOSE, "Reg - Value: 0x%lX = 0x%08X\n", Reg, Value));
  MmioWrite32 (Reg, Value);
}

VOID
GpioPinSetPull (
  IN UINT8          Group,
  IN UINT8          Pin,
  IN GPIO_PIN_PULL  Pull
  )
{
  ASSERT (Group < GPIO_NGROUPS);

  EFI_PHYSICAL_ADDRESS  Reg;
  CONST UINT32          Value = GRF_GPIO_P_MASK (Pin) | ((UINT32)Pull << GRF_GPIO_P_SHIFT (Pin));

  DEBUG ((DEBUG_INFO, "GpioPinSetPull Group:%d Pin:%d Pull:%d\n", Group, Pin, Pull));

  if (!GpioPinconfLookupReg (
         mPullReg,
         ARRAY_SIZE (mPullReg),
         Group,
         Pin,
         8,
         &Reg
         ))
  {
    DEBUG ((DEBUG_ERROR, "GpioPinSetPull: no pull register for Group:%d Pin:%d\n", Group, Pin));
    ASSERT (FALSE);
    return;
  }

  DEBUG ((DEBUG_INFO, "  pull reg 0x%lX <- 0x%08X\n", Reg, Value));
  MmioWrite32 (Reg, Value);
  DEBUG ((DEBUG_INFO, "  pull reg 0x%lX -> 0x%08X\n", Reg, MmioRead32 (Reg)));
}

VOID
GpioPinSetDrive (
  IN UINT8           Group,
  IN UINT8           Pin,
  IN GPIO_PIN_DRIVE  Drive
  )
{
  ASSERT (Group < GPIO_NGROUPS);

  EFI_PHYSICAL_ADDRESS  Reg;
  UINT32                Level;
  UINT32                Value;

  DEBUG ((DEBUG_INFO, "GpioPinSetDrive Group:%d Pin:%d Drive:0x%X\n", Group, Pin, Drive));

  /*
   * GPIO_PIN_DRIVE_* are one-hot encoded, which is the RK3568 representation
   * ((1 << (level + 1)) - 1). RK3588 takes the raw DTS drive-strength level in
   * a 4-bit field, so translate here rather than writing the one-hot value.
   */
  switch (Drive) {
    case GPIO_PIN_DRIVE_0:
      Level = 0;
      break;
    case GPIO_PIN_DRIVE_1:
      Level = 1;
      break;
    case GPIO_PIN_DRIVE_2:
      Level = 2;
      break;
    case GPIO_PIN_DRIVE_3:
      Level = 3;
      break;
    case GPIO_PIN_DRIVE_4:
      Level = 4;
      break;
    case GPIO_PIN_DRIVE_5:
      Level = 5;
      break;
    default:
      DEBUG ((
        DEBUG_ERROR,
        "GpioPinSetDrive: unsupported drive 0x%X for Group:%d Pin:%d\n",
        Drive,
        Group,
        Pin
        ));
      ASSERT (FALSE);
      return;
  }

  if (!GpioPinconfLookupReg (
         mDsReg,
         ARRAY_SIZE (mDsReg),
         Group,
         Pin,
         4,
         &Reg
         ))
  {
    DEBUG ((DEBUG_ERROR, "GpioPinSetDrive: no drive register for Group:%d Pin:%d\n", Group, Pin));
    ASSERT (FALSE);
    return;
  }

  Value = GRF_GPIO_DS_MASK (Pin) | (Level << GRF_GPIO_DS_SHIFT (Pin));

  DEBUG ((DEBUG_INFO, "  drive reg 0x%lX <- 0x%08X (level %d)\n", Reg, Value, Level));
  MmioWrite32 (Reg, Value);
  DEBUG ((DEBUG_INFO, "  drive reg 0x%lX -> 0x%08X\n", Reg, MmioRead32 (Reg)));
}

VOID
GpioPinSetInput (
  IN UINT8                  Group,
  IN UINT8                  Pin,
  IN GPIO_PIN_INPUT_ENABLE  InputEnable
  )
{
  ASSERT (Group < GPIO_NGROUPS);

  EFI_PHYSICAL_ADDRESS  Reg;
  UINT32                Enable;
  UINT32                Value;

  DEBUG ((DEBUG_INFO, "GpioPinSetInput Group:%d Pin:%d InputEnable:0x%X\n", Group, Pin, InputEnable));

  /*
   * RK3588 models this as a single Schmitt-trigger enable bit per pin. The
   * GPIO_PIN_INPUT_* values are the RK3568 2-bit encoding (1 = non-Schmitt,
   * 2 = Schmitt), so collapse them to that bit.
   */
  switch (InputEnable) {
    case GPIO_PIN_INPUT_DISABLE:
    case GPIO_PIN_INPUT_NON_SCHMITT:
      Enable = 0;
      break;
    case GPIO_PIN_INPUT_SCHMITT:
      Enable = 1;
      break;
    default:
      DEBUG ((
        DEBUG_ERROR,
        "GpioPinSetInput: unsupported value 0x%X for Group:%d Pin:%d\n",
        InputEnable,
        Group,
        Pin
        ));
      ASSERT (FALSE);
      return;
  }

  if (!GpioPinconfLookupReg (
         mSmtReg,
         ARRAY_SIZE (mSmtReg),
         Group,
         Pin,
         8,
         &Reg
         ))
  {
    DEBUG ((DEBUG_ERROR, "GpioPinSetInput: no Schmitt register for Group:%d Pin:%d\n", Group, Pin));
    ASSERT (FALSE);
    return;
  }

  Value = GRF_GPIO_SMT_MASK (Pin) | (Enable << GRF_GPIO_SMT_SHIFT (Pin));

  DEBUG ((DEBUG_INFO, "  schmitt reg 0x%lX <- 0x%08X (enable %d)\n", Reg, Value, Enable));
  MmioWrite32 (Reg, Value);
  DEBUG ((DEBUG_INFO, "  schmitt reg 0x%lX -> 0x%08X\n", Reg, MmioRead32 (Reg)));
}

VOID
GpioSetIomuxConfig (
  IN CONST GPIO_IOMUX_CONFIG  *Configs,
  IN UINT32                   NumConfigs
  )
{
  UINT32  Index;

  for (Index = 0; Index < NumConfigs; Index++) {
    CONST GPIO_IOMUX_CONFIG  *Mux = &Configs[Index];
    DEBUG ((DEBUG_INFO, "GPIO: IOMUX for pin '%a'\n", Mux->Name));
    GpioPinSetFunction (Mux->Group, Mux->Pin, Mux->Function);
    GpioPinSetPull (Mux->Group, Mux->Pin, Mux->Pull);
    if (Mux->Drive != GPIO_PIN_DRIVE_DEFAULT) {
      GpioPinSetDrive (Mux->Group, Mux->Pin, Mux->Drive);
    }
  }
}
