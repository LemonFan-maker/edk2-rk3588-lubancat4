/** @file
  USB Mass Storage Bulk-Only/SCSI bridge for EFI_BLOCK_IO_PROTOCOL devices.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MASS_STORAGE_H_
#define MASS_STORAGE_H_

#include <Uefi.h>

#include "Dwc3Device.h"

typedef struct _MASS_STORAGE_CONTEXT MASS_STORAGE_CONTEXT;

/**
  Enumerate supported physical disks and allocate the transfer buffer.

  Logical partitions are deliberately excluded.  Each whole physical disk is
  exposed as one USB LUN, allowing the connected host to discover every
  partition from the disk's GPT/MBR without duplicate overlapping devices.
**/
EFI_STATUS
MassStorageCreate (
  OUT MASS_STORAGE_CONTEXT  **Context
  );

/**
  Flush all exported media and release the context.
**/
VOID
MassStorageDestroy (
  IN MASS_STORAGE_CONTEXT  *Context
  );

/**
  Print the LUN-to-physical-disk mapping to the console.
**/
VOID
MassStoragePrintLuns (
  IN MASS_STORAGE_CONTEXT  *Context
  );

/**
  Return the highest valid LUN number.
**/
UINT8
MassStorageGetMaxLun (
  IN MASS_STORAGE_CONTEXT  *Context
  );

/**
  Flush every exported physical disk.
**/
VOID
MassStorageFlushAll (
  IN MASS_STORAGE_CONTEXT  *Context
  );

/**
  Reset callback passed to Dwc3DeviceStart().
**/
VOID
EFIAPI
MassStorageReset (
  IN VOID  *Context
  );

/**
  Receive and execute one Bulk-Only command.

  EFI_NOT_READY means the host has not selected configuration 1 yet.
  EFI_ABORTED is expected when USB is reset/disconnected or the user requests
  exit; callers may resume unless their own cancel flag is set.
**/
EFI_STATUS
MassStorageProcessCommand (
  IN MASS_STORAGE_CONTEXT  *Context,
  IN DWC3_DEVICE           *Device
  );

#endif // MASS_STORAGE_H_
