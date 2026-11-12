/** @file
 *
 *  LubanCat4 PWM fan closed-loop thermal control.
 *
 *  Fan: PWM0 channel 0 @ 0xFD8B0000 (PWM0_M2 / GPIO1_PA2), 24 MHz counting
 *  clock, period 5000 ns = 120 cycles. Firmware (PlatformEarlyInit +
 *  FanControl) sets pinmux/period/enable and a boot-time duty; once Windows
 *  evaluates this thermal zone it takes over duty selection.
 *
 *  Temperature: TSADC channel 0 ("top" SoC sensor) DATA register, started in
 *  auto-conversion mode by TsadcInit() in RockchipPlatformLib.c. Code-to-temp
 *  mapping follows the Linux rk3588_code_table piecewise-linear segments:
 *  215->-40C, 285->25C, 350->85C, 395->125C (ADC_INCREMENT, mask 0x1FF).
 *
 *  Duty levels mirror the vendor DTS cooling-levels <0 100 150 200 255>:
 *  off / 39% / 59% / 78% / 100% at trips 42 / 55 / 60 / 65 C.
 *
 *  Copyright (c) 2026, OrionisLi(CyanLi) <2254650260@qq.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

Scope (\_SB_) {

  OperationRegion (PWMF, SystemMemory, 0xFD8B0000, 0x10)
  Field (PWMF, DWordAcc, NoLock, Preserve) {
    PCNT, 32,   // 0x00 CNT
    PPRD, 32,   // 0x04 PERIOD_HPR
    PDTY, 32,   // 0x08 DUTY_LPR
    PCTL, 32,   // 0x0C CTRL
  }

  OperationRegion (TSDR, SystemMemory, 0xFEC0002C, 0x4)
  Field (TSDR, DWordAcc, NoLock, Preserve) {
    TDA0, 32,   // TSADC DATA0 (channel 0, "top")
  }

  // Bitmask of requested cooling levels: bit0=low .. bit3=full
  Name (FANL, Zero)

  // Program the highest requested level.
  // CTRL 0x0B = ENABLE | CONTINUOUS mode | DUTY_POL positive
  // (identical to what RkPwmSetConfig + RkPwmEnable leave configured).
  Method (FSET, 0, Serialized) {
    Local0 = Zero
    If (FANL & 0x08) {
      Local0 = 120                    // 100%
    } ElseIf (FANL & 0x04) {
      Local0 = 94                     // ~78%  (200/255)
    } ElseIf (FANL & 0x02) {
      Local0 = 71                     // ~59%  (150/255)
    } ElseIf (FANL & 0x01) {
      Local0 = 47                     // ~39%  (100/255)
    }
    PPRD = 120
    PDTY = Local0
    PCTL = 0x0B
  }

  Method (FLON, 1, Serialized) {
    FANL = FANL | Arg0
    FSET ()
  }

  Method (FLOF, 1, Serialized) {
    FANL = FANL & ~Arg0
    FSET ()
  }

  PowerResource (PFN0, 0, 0) {
    Method (_STA) { Return (FANL & 0x01) }
    Method (_ON)  { FLON (0x01) }
    Method (_OFF) { FLOF (0x01) }
  }
  PowerResource (PFN1, 0, 0) {
    Method (_STA) { Return ((FANL >> 1) & 0x01) }
    Method (_ON)  { FLON (0x02) }
    Method (_OFF) { FLOF (0x02) }
  }
  PowerResource (PFN2, 0, 0) {
    Method (_STA) { Return ((FANL >> 2) & 0x01) }
    Method (_ON)  { FLON (0x04) }
    Method (_OFF) { FLOF (0x04) }
  }
  PowerResource (PFN3, 0, 0) {
    Method (_STA) { Return ((FANL >> 3) & 0x01) }
    Method (_ON)  { FLON (0x08) }
    Method (_OFF) { FLOF (0x08) }
  }

  Device (FAN0) {
    Name (_HID, EisaId ("PNP0C0B"))
    Name (_UID, 0x10)
    Name (_PR0, Package () { \_SB.PFN0 })
  }
  Device (FAN1) {
    Name (_HID, EisaId ("PNP0C0B"))
    Name (_UID, 0x11)
    Name (_PR0, Package () { \_SB.PFN1 })
  }
  Device (FAN2) {
    Name (_HID, EisaId ("PNP0C0B"))
    Name (_UID, 0x12)
    Name (_PR0, Package () { \_SB.PFN2 })
  }
  Device (FAN3) {
    Name (_HID, EisaId ("PNP0C0B"))
    Name (_UID, 0x13)
    Name (_PR0, Package () { \_SB.PFN3 })
  }

  ThermalZone (THM0) {
    // Tenths of Kelvin. Piecewise linear over the Linux rk3588 code table.
    Method (_TMP, 0, Serialized) {
      Local0 = TDA0 & 0x1FF
      If (Local0 < 215) {
        Return (3282)               // implausible reading -> fail-safe 55.0 C
      }
      If (Local0 >= 395) {
        Return (3982)               // clamp at 125.0 C
      }
      If (Local0 < 285) {
        Return (2332 + (((Local0 - 215) * 650) / 70))   // -40..25 C
      }
      If (Local0 < 350) {
        Return (2982 + (((Local0 - 285) * 600) / 65))   // 25..85 C
      }
      Return (3582 + (((Local0 - 350) * 400) / 45))     // 85..125 C
    }

    Method (_AC0) { Return (3382) }   // 65 C -> full
    Method (_AC1) { Return (3332) }   // 60 C -> 78%
    Method (_AC2) { Return (3282) }   // 55 C -> 59%
    Method (_AC3) { Return (3152) }   // 42 C -> 39%
    Name (_AL0, Package () { \_SB.FAN3 })
    Name (_AL1, Package () { \_SB.FAN2 })
    Name (_AL2, Package () { \_SB.FAN1 })
    Name (_AL3, Package () { \_SB.FAN0 })
    Method (_CRT) { Return (3682) }   // 95 C critical shutdown
    Name (_TZP, 30)                   // poll every 3.0 s
  }
}
