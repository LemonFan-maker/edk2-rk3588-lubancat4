/** @file
  LubanCat4 read/write USB Mass Storage mode application.

  The application takes exclusive ownership of USB3OTG0, configures the
  FUSB302 Type-C port controller as a sink/device, and exports each supported
  whole physical disk as one Mass Storage LUN.  Returning to BDS after another
  computer has changed a mounted filesystem would leave stale firmware cache
  state, so the only supported exit path is a cold reset.

  Copyright (c) 2026, OrionisLi <cyan2254650260@gmail.com>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/NonDiscoverableDevice.h>

#include <Protocol/DevicePath.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/RockchipI2cMasterProtocol.h>
#include <Protocol/SimpleFileSystem.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/GpioLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "Dwc3Device.h"
#include "MassStorage.h"

#define LUBANCAT4_OTG_DWC3_BASE  0xfc000000ULL

#define LUBANCAT4_TYPEC_I2C_BUS      1U
#define LUBANCAT4_FUSB302_ADDRESS    0x22U
#define LUBANCAT4_TYPEC_VBUS_BANK    1U
#define LUBANCAT4_TYPEC_VBUS_PIN     GPIO_PIN_PC1

#define FUSB302_REG_DEVICE_ID  0x01U
#define FUSB302_REG_SWITCHES0  0x02U
#define FUSB302_REG_CONTROL2   0x08U
#define FUSB302_REG_POWER      0x0bU
#define FUSB302_REG_RESET      0x0cU

#define FUSB302_RESET_SW_RES   BIT0
#define FUSB302_POWER_ALL      0x0fU
#define FUSB302_SWITCH_PDWN1   BIT0
#define FUSB302_SWITCH_PDWN2   BIT1

STATIC BOOLEAN  mExitRequested;

STATIC
BOOLEAN
PollExitKey (
  VOID
  )
{
  EFI_INPUT_KEY  Key;

  while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
    if ((Key.ScanCode == SCAN_ESC) ||
        (Key.UnicodeChar == L'q') ||
        (Key.UnicodeChar == L'Q'))
    {
      mExitRequested = TRUE;
    }
  }

  return mExitRequested;
}

STATIC
BOOLEAN
EFIAPI
CancelTransfer (
  IN VOID  *Context
  )
{
  (VOID)Context;
  return PollExitKey ();
}

STATIC
VOID
FlushFirmwareFileSystems (
  VOID
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_STATUS                       Status;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (FileSystem->OpenVolume == NULL) {
      continue;
    }

    Root   = NULL;
    Status = FileSystem->OpenVolume (FileSystem, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
      continue;
    }

    // A driver may leave optional members of EFI_FILE_PROTOCOL unset; calling
    // through them would branch to address 0.
    if (Root->Flush != NULL) {
      Root->Flush (Root);
    }

    if (Root->Close != NULL) {
      Root->Close (Root);
    }
  }

  FreePool (Handles);
}

STATIC
BOOLEAN
NonDiscoverableDeviceOwnsAddress (
  IN NON_DISCOVERABLE_DEVICE  *Device,
  IN EFI_PHYSICAL_ADDRESS     Address
  )
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Resource;

  if ((Device == NULL) || (Device->Resources == NULL)) {
    return FALSE;
  }

  for (Resource = Device->Resources;
       Resource->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR;
       Resource++)
  {
    if ((Address >= Resource->AddrRangeMin) &&
        (Address <= Resource->AddrRangeMax))
    {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
EFI_STATUS
DisconnectOtgHostController (
  VOID
  )
{
  EFI_HANDLE               *Handles;
  UINTN                    HandleCount;
  UINTN                    Index;
  NON_DISCOVERABLE_DEVICE  *Device;
  EFI_STATUS               Status;
  EFI_STATUS               DisconnectStatus;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; Index++) {
    DisconnectStatus = gBS->HandleProtocol (
                              Handles[Index],
                              &gEdkiiNonDiscoverableDeviceProtocolGuid,
                              (VOID **)&Device
                              );
    if (EFI_ERROR (DisconnectStatus) ||
        !CompareGuid (Device->Type, &gEdkiiNonDiscoverableXhciDeviceGuid) ||
        !NonDiscoverableDeviceOwnsAddress (Device, LUBANCAT4_OTG_DWC3_BASE))
    {
      continue;
    }

    DisconnectStatus = gBS->DisconnectController (
                              Handles[Index],
                              NULL,
                              NULL
                              );
    if ((DisconnectStatus == EFI_NOT_FOUND) ||
        (DisconnectStatus == EFI_NOT_STARTED))
    {
      DisconnectStatus = EFI_SUCCESS;
    }

    Status = DisconnectStatus;
    break;
  }

  FreePool (Handles);
  return Status;
}

STATIC
EFI_STATUS
LocateI2cMaster (
  IN  UINT8                    Bus,
  OUT EFI_I2C_MASTER_PROTOCOL  **I2cMaster
  )
{
  EFI_HANDLE                        *Handles;
  UINTN                             HandleCount;
  UINTN                             Index;
  ROCKCHIP_I2C_MASTER_PROTOCOL      *RockchipI2c;
  EFI_I2C_MASTER_PROTOCOL           *Candidate;
  EFI_STATUS                        HandleStatus;
  EFI_STATUS                        Status;

  *I2cMaster = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gRockchipI2cMasterProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; Index++) {
    HandleStatus = gBS->HandleProtocol (
                          Handles[Index],
                          &gRockchipI2cMasterProtocolGuid,
                          (VOID **)&RockchipI2c
                          );
    if (EFI_ERROR (HandleStatus) || (RockchipI2c->Bus != Bus)) {
      continue;
    }

    Candidate = NULL;
    Status    = gBS->HandleProtocol (
                       Handles[Index],
                       &gEfiI2cMasterProtocolGuid,
                       (VOID **)&Candidate
                       );
    if (!EFI_ERROR (Status) && (Candidate != NULL)) {
      *I2cMaster = Candidate;
      break;
    }

    if (!EFI_ERROR (Status)) {
      Status = EFI_NOT_FOUND;
    }
  }

  FreePool (Handles);
  return Status;
}

STATIC
EFI_STATUS
Fusb302WriteRegister (
  IN EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN UINT8                    Register,
  IN UINT8                    Value
  )
{
  EFI_I2C_REQUEST_PACKET  Request;
  UINT8                   Data[2];

  Data[0] = Register;
  Data[1] = Value;

  ZeroMem (&Request, sizeof (Request));
  Request.OperationCount             = 1;
  Request.Operation[0].Flags         = 0;
  Request.Operation[0].LengthInBytes = sizeof (Data);
  Request.Operation[0].Buffer        = Data;

  return I2cMaster->StartRequest (
                      I2cMaster,
                      LUBANCAT4_FUSB302_ADDRESS,
                      &Request,
                      NULL,
                      NULL
                      );
}

STATIC
EFI_STATUS
Fusb302ReadRegister (
  IN  EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN  UINT8                    Register,
  OUT UINT8                    *Value
  )
{
  typedef struct {
    UINTN                OperationCount;
    EFI_I2C_OPERATION    Operation[2];
  } TWO_OPERATION_REQUEST;

  TWO_OPERATION_REQUEST  Request;

  // I2cDxe currently returns EFI_SUCCESS even after a low-level NACK.  Leave
  // a recognizable value behind so callers can validate known registers.
  *Value = 0xff;
  ZeroMem (&Request, sizeof (Request));
  Request.OperationCount             = 2;
  Request.Operation[0].Flags         = 0;
  Request.Operation[0].LengthInBytes = sizeof (Register);
  Request.Operation[0].Buffer        = &Register;
  Request.Operation[1].Flags         = I2C_FLAG_READ;
  Request.Operation[1].LengthInBytes = sizeof (*Value);
  Request.Operation[1].Buffer        = Value;

  return I2cMaster->StartRequest (
                      I2cMaster,
                      LUBANCAT4_FUSB302_ADDRESS,
                      (EFI_I2C_REQUEST_PACKET *)&Request,
                      NULL,
                      NULL
                      );
}

STATIC
EFI_STATUS
Fusb302VerifyRegister (
  IN EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN UINT8                    Register,
  IN UINT8                    Expected
  )
{
  EFI_STATUS  Status;
  UINT8       Value;

  Status = Fusb302ReadRegister (I2cMaster, Register, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Value != Expected) {
    DEBUG ((
      DEBUG_ERROR,
      "MassStorage: FUSB302 register 0x%02x read 0x%02x, expected 0x%02x\n",
      Register,
      Value,
      Expected
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ConfigureTypecDeviceRole (
  VOID
  )
{
  EFI_I2C_MASTER_PROTOCOL  *I2cMaster;
  EFI_STATUS               Status;
  UINT8                    DeviceId;

  // A USB device must never source VBUS.  Disable the board's Type-C 5 V
  // regulator before changing the CC pull resistors.
  GpioPinWrite (
    LUBANCAT4_TYPEC_VBUS_BANK,
    LUBANCAT4_TYPEC_VBUS_PIN,
    FALSE
    );
  GpioPinSetFunction (
    LUBANCAT4_TYPEC_VBUS_BANK,
    LUBANCAT4_TYPEC_VBUS_PIN,
    0
    );
  GpioPinSetPull (
    LUBANCAT4_TYPEC_VBUS_BANK,
    LUBANCAT4_TYPEC_VBUS_PIN,
    GPIO_PIN_PULL_NONE
    );
  GpioPinSetDirection (
    LUBANCAT4_TYPEC_VBUS_BANK,
    LUBANCAT4_TYPEC_VBUS_PIN,
    GPIO_PIN_OUTPUT
    );
  MicroSecondDelay (1000);

  Status = LocateI2cMaster (LUBANCAT4_TYPEC_I2C_BUS, &I2cMaster);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MassStorage: I2C1 is unavailable: %r\n", Status));
    return Status;
  }

  Status = Fusb302ReadRegister (I2cMaster, FUSB302_REG_DEVICE_ID, &DeviceId);
  if (EFI_ERROR (Status) || (DeviceId == 0) || (DeviceId == 0xff)) {
    DEBUG ((
      DEBUG_ERROR,
      "MassStorage: FUSB302 is unavailable: status %r, device ID 0x%02x\n",
      Status,
      DeviceId
      ));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "MassStorage: FUSB302 device ID 0x%02x\n", DeviceId));
  Status = Fusb302WriteRegister (
             I2cMaster,
             FUSB302_REG_RESET,
             FUSB302_RESET_SW_RES
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (1000);
  Status = Fusb302WriteRegister (
             I2cMaster,
             FUSB302_REG_POWER,
             FUSB302_POWER_ALL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Fusb302VerifyRegister (
             I2cMaster,
             FUSB302_REG_POWER,
             FUSB302_POWER_ALL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Disable automatic DRP toggling, then present Rd on both CC pins.  USB2
  // D+/D- is orientation independent, so no SuperSpeed mux is required.
  Status = Fusb302WriteRegister (I2cMaster, FUSB302_REG_CONTROL2, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Fusb302VerifyRegister (I2cMaster, FUSB302_REG_CONTROL2, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Fusb302WriteRegister (
             I2cMaster,
             FUSB302_REG_SWITCHES0,
             FUSB302_SWITCH_PDWN1 | FUSB302_SWITCH_PDWN2
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Fusb302VerifyRegister (
           I2cMaster,
           FUSB302_REG_SWITCHES0,
           FUSB302_SWITCH_PDWN1 | FUSB302_SWITCH_PDWN2
           );
}

STATIC
VOID
ResetAfterMassStorage (
  VOID
  )
{
  Print (L"\nFlushed all exported disks. Rebooting...\n");
  gBS->Stall (500000);
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  MASS_STORAGE_CONTEXT  *MassStorage;
  DWC3_DEVICE           *UsbDevice;
  EFI_STATUS            Status;
  BOOLEAN               ControllerOwned;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  // BDS normally arms a five-minute watchdog around boot applications.  This
  // mode is intentionally long-running, so it must not reset underneath an
  // active host write.
  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  MassStorage    = NULL;
  UsbDevice      = NULL;
  ControllerOwned = FALSE;
  mExitRequested = FALSE;

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"LubanCat4 UEFI Mass Storage Mode\n");
  Print (L"================================\n\n");
  Print (L"The connected computer receives raw, read/write access to each disk.\n");
  Print (L"All GPT/MBR partitions on those disks will become visible there.\n\n");

  Status = MassStorageCreate (&MassStorage);
  if (EFI_ERROR (Status)) {
    Print (L"No supported physical eMMC/SD/NVMe/SATA disk was found: %r\n", Status);
    return Status;
  }

  Print (L"Exporting:\n");
  MassStoragePrintLuns (MassStorage);
  Print (L"\nFlushing firmware filesystem and block caches...\n");
  Print (L"  [1/5] filesystem flush...\n");
  FlushFirmwareFileSystems ();
  Print (L"  [2/5] block cache flush...\n");
  MassStorageFlushAll (MassStorage);

  Print (L"  [3/5] releasing xHCI...\n");
  Status = DisconnectOtgHostController ();
  if (EFI_ERROR (Status)) {
    Print (L"Could not take ownership of USB3OTG0: %r\n", Status);
    MassStorageDestroy (MassStorage);
    return Status;
  }

  ControllerOwned = TRUE;
  Print (L"  [4/5] Type-C device role...\n");
  Status = ConfigureTypecDeviceRole ();
  if (EFI_ERROR (Status)) {
    Print (L"Could not configure the Type-C port as a device: %r\n", Status);
    goto Exit;
  }

  Print (L"  [5/5] starting DWC3 device controller...\n");
  Status = Dwc3DeviceStart (
             LUBANCAT4_OTG_DWC3_BASE,
             MassStorageGetMaxLun (MassStorage),
             MassStorageReset,
             MassStorage,
             CancelTransfer,
             NULL,
             &UsbDevice
             );
  if (EFI_ERROR (Status)) {
    Print (L"Could not start the DWC3 device controller: %r\n", Status);
    goto Exit;
  }

  Print (L"\nConnect the Type-C port to a computer.\n");
  Print (L"Use the computer's safe-eject function before leaving this mode.\n");
  Print (L"Press Esc or Q here to flush all media and reboot.\n\n");
  Print (L"Waiting for USB host...\n");

  while (!mExitRequested) {
    PollExitKey ();
    if (mExitRequested) {
      break;
    }

    Status = MassStorageProcessCommand (MassStorage, UsbDevice);
    if ((Status == EFI_SUCCESS) ||
        (Status == EFI_NOT_READY) ||
        (Status == EFI_ABORTED) ||
        (Status == EFI_PROTOCOL_ERROR))
    {
      MicroSecondDelay (100);
      continue;
    }

    DEBUG ((DEBUG_ERROR, "MassStorage command loop: %r\n", Status));
    Print (L"Fatal USB transfer error: %r. Flushing and rebooting.\n", Status);
    break;
  }

Exit:
  if (UsbDevice != NULL) {
    Dwc3DeviceStop (UsbDevice);
  }

  if (MassStorage != NULL) {
    MassStorageDestroy (MassStorage);
  }

  if (ControllerOwned) {
    // Never return to BDS with filesystem state that may be stale after host
    // writes or with USB3OTG0 no longer in xHCI mode.
    ResetAfterMassStorage ();
  }

  return Status;
}
