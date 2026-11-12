/** @file
  USB Mass Storage Bulk-Only Transport and SCSI transparent command bridge.

  Copyright (c) 2026, OrionisLi <cyan2254650260@gmail.com>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DmaLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "MassStorage.h"

#define MASS_STORAGE_SIGNATURE  SIGNATURE_32 ('M', 'S', 'C', 'X')
#define MASS_STORAGE_MAX_LUNS   16U
#define MASS_STORAGE_IO_BUFFER_SIZE  SIZE_1MB
#define MASS_STORAGE_CBW_RECEIVE_SIZE 512U

#define USB_MSC_CBW_SIGNATURE  0x43425355U
#define USB_MSC_CSW_SIGNATURE  0x53425355U
#define USB_MSC_CSW_PASSED     0U
#define USB_MSC_CSW_FAILED     1U
#define USB_MSC_CSW_PHASE_ERROR 2U

#define USB_MSC_CBW_FLAG_IN  BIT7

#define SCSI_TEST_UNIT_READY          0x00U
#define SCSI_REQUEST_SENSE            0x03U
#define SCSI_READ_6                   0x08U
#define SCSI_WRITE_6                  0x0aU
#define SCSI_INQUIRY                  0x12U
#define SCSI_MODE_SENSE_6             0x1aU
#define SCSI_START_STOP_UNIT          0x1bU
#define SCSI_PREVENT_ALLOW_MEDIUM     0x1eU
#define SCSI_READ_FORMAT_CAPACITIES   0x23U
#define SCSI_READ_CAPACITY_10         0x25U
#define SCSI_READ_10                  0x28U
#define SCSI_WRITE_10                 0x2aU
#define SCSI_VERIFY_10                0x2fU
#define SCSI_SYNCHRONIZE_CACHE_10     0x35U
#define SCSI_MODE_SENSE_10            0x5aU
#define SCSI_READ_16                  0x88U
#define SCSI_WRITE_16                 0x8aU
#define SCSI_VERIFY_16                0x8fU
#define SCSI_SYNCHRONIZE_CACHE_16     0x91U
#define SCSI_SERVICE_ACTION_IN_16     0x9eU
#define SCSI_REPORT_LUNS              0xa0U
#define SCSI_READ_12                  0xa8U
#define SCSI_WRITE_12                 0xaaU

#define SCSI_SAI_READ_CAPACITY_16  0x10U

#define SCSI_SENSE_NO_SENSE         0x00U
#define SCSI_SENSE_NOT_READY        0x02U
#define SCSI_SENSE_MEDIUM_ERROR      0x03U
#define SCSI_SENSE_ILLEGAL_REQUEST   0x05U
#define SCSI_SENSE_UNIT_ATTENTION    0x06U
#define SCSI_SENSE_DATA_PROTECT      0x07U

#define SCSI_ASC_INVALID_OPCODE       0x20U
#define SCSI_ASC_LBA_OUT_OF_RANGE     0x21U
#define SCSI_ASC_INVALID_FIELD        0x24U
#define SCSI_ASC_WRITE_PROTECTED      0x27U
#define SCSI_ASC_MEDIUM_CHANGED       0x28U
#define SCSI_ASC_POWER_ON_RESET       0x29U
#define SCSI_ASC_MEDIUM_NOT_PRESENT   0x3aU
#define SCSI_ASC_LOGICAL_UNIT_NOT_READY 0x04U
#define SCSI_ASC_UNRECOVERED_READ     0x11U
#define SCSI_ASC_WRITE_ERROR          0x0cU

#pragma pack (1)
typedef struct {
  UINT32    Signature;
  UINT32    Tag;
  UINT32    DataTransferLength;
  UINT8     Flags;
  UINT8     Lun;
  UINT8     CommandLength;
  UINT8     CommandBlock[16];
} USB_MSC_CBW;

typedef struct {
  UINT32    Signature;
  UINT32    Tag;
  UINT32    DataResidue;
  UINT8     Status;
} USB_MSC_CSW;
#pragma pack ()

typedef enum {
  ScsiDataNone,
  ScsiDataIn,
  ScsiDataOut
} SCSI_DATA_DIRECTION;

typedef struct {
  EFI_HANDLE             Handle;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINT8                  Priority;
  CONST CHAR16           *TypeName;
  UINT8                  SenseKey;
  UINT8                  AdditionalSenseCode;
  UINT8                  AdditionalSenseQualifier;
  UINT32                 MediaId;
  BOOLEAN                MediaPresent;
} MASS_STORAGE_LUN;

typedef struct {
  USB_MSC_CBW          *Cbw;
  USB_MSC_CSW          Csw;
  MASS_STORAGE_LUN     *Lun;
  SCSI_DATA_DIRECTION  HostDirection;
  UINT32               BytesTransferred;
  UINT32               ResetGeneration;
} MASS_STORAGE_COMMAND;

struct _MASS_STORAGE_CONTEXT {
  UINT32             Signature;
  MASS_STORAGE_LUN   Luns[MASS_STORAGE_MAX_LUNS];
  UINTN              LunCount;
  VOID               *IoBuffer;
  UINTN              IoBufferSize;
  UINTN              IoBufferPages;
  UINT8              CbwBuffer[MASS_STORAGE_CBW_RECEIVE_SIZE];
  BOOLEAN            NeedsResetRecovery;
  UINT32             ResetGeneration;
};

STATIC
UINT16
ReadBigEndian16 (
  IN CONST UINT8  *Buffer
  )
{
  return (UINT16)(((UINT16)Buffer[0] << 8) | Buffer[1]);
}

STATIC
UINT32
ReadBigEndian32 (
  IN CONST UINT8  *Buffer
  )
{
  return ((UINT32)Buffer[0] << 24) |
         ((UINT32)Buffer[1] << 16) |
         ((UINT32)Buffer[2] << 8) |
         Buffer[3];
}

STATIC
UINT64
ReadBigEndian64 (
  IN CONST UINT8  *Buffer
  )
{
  return ((UINT64)ReadBigEndian32 (Buffer) << 32) |
         ReadBigEndian32 (Buffer + 4);
}

STATIC
VOID
WriteBigEndian16 (
  OUT UINT8   *Buffer,
  IN  UINT16  Value
  )
{
  Buffer[0] = (UINT8)(Value >> 8);
  Buffer[1] = (UINT8)Value;
}

STATIC
VOID
WriteBigEndian24 (
  OUT UINT8   *Buffer,
  IN  UINT32  Value
  )
{
  Buffer[0] = (UINT8)(Value >> 16);
  Buffer[1] = (UINT8)(Value >> 8);
  Buffer[2] = (UINT8)Value;
}

STATIC
VOID
WriteBigEndian32 (
  OUT UINT8   *Buffer,
  IN  UINT32  Value
  )
{
  Buffer[0] = (UINT8)(Value >> 24);
  Buffer[1] = (UINT8)(Value >> 16);
  Buffer[2] = (UINT8)(Value >> 8);
  Buffer[3] = (UINT8)Value;
}

STATIC
VOID
WriteBigEndian64 (
  OUT UINT8   *Buffer,
  IN  UINT64  Value
  )
{
  WriteBigEndian32 (Buffer, (UINT32)(Value >> 32));
  WriteBigEndian32 (Buffer + 4, (UINT32)Value);
}

STATIC
BOOLEAN
GetPhysicalStorageType (
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  OUT UINT8                     *Priority,
  OUT CONST CHAR16              **TypeName
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  for (Node = DevicePath;
       (Node != NULL) && !IsDevicePathEnd (Node);
       Node = NextDevicePathNode (Node))
  {
    if (DevicePathType (Node) != MESSAGING_DEVICE_PATH) {
      continue;
    }

    switch (DevicePathSubType (Node)) {
      case MSG_EMMC_DP:
        *Priority = 0;
        *TypeName = L"eMMC";
        return TRUE;

      case MSG_UFS_DP:
        *Priority = 1;
        *TypeName = L"UFS";
        return TRUE;

      case MSG_NVME_NAMESPACE_DP:
      case MSG_NVME_OF_NAMESPACE_DP:
        *Priority = 2;
        *TypeName = L"NVMe";
        return TRUE;

      case MSG_SATA_DP:
        *Priority = 3;
        *TypeName = L"SATA";
        return TRUE;

      case MSG_SD_DP:
        *Priority = 4;
        *TypeName = L"SD";
        return TRUE;

      case MSG_ATAPI_DP:
        *Priority = 5;
        *TypeName = L"ATAPI";
        return TRUE;

      default:
        break;
    }
  }

  return FALSE;
}

STATIC
VOID
SortLuns (
  IN OUT MASS_STORAGE_CONTEXT  *Context
  )
{
  MASS_STORAGE_LUN  Temporary;
  UINTN             Index;
  UINTN             Other;

  for (Index = 0; Index < Context->LunCount; Index++) {
    for (Other = Index + 1; Other < Context->LunCount; Other++) {
      if ((Context->Luns[Other].Priority < Context->Luns[Index].Priority) ||
          ((Context->Luns[Other].Priority == Context->Luns[Index].Priority) &&
           ((UINTN)Context->Luns[Other].Handle < (UINTN)Context->Luns[Index].Handle)))
      {
        CopyMem (&Temporary, &Context->Luns[Index], sizeof (Temporary));
        CopyMem (&Context->Luns[Index], &Context->Luns[Other], sizeof (Temporary));
        CopyMem (&Context->Luns[Other], &Temporary, sizeof (Temporary));
      }
    }
  }
}

STATIC
EFI_STATUS
EnumeratePhysicalDisks (
  IN OUT MASS_STORAGE_CONTEXT  *Context
  )
{
  EFI_HANDLE                *Handles;
  UINTN                     HandleCount;
  UINTN                     Index;
  EFI_BLOCK_IO_PROTOCOL     *BlockIo;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_STATUS                Status;
  UINT8                     Priority;
  CONST CHAR16              *TypeName;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0;
       (Index < HandleCount) && (Context->LunCount < MASS_STORAGE_MAX_LUNS);
       Index++)
  {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );
    if (EFI_ERROR (Status) ||
        (BlockIo->Media == NULL) ||
        BlockIo->Media->LogicalPartition ||
        (BlockIo->Media->BlockSize == 0))
    {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&DevicePath
                    );
    if (EFI_ERROR (Status) ||
        !GetPhysicalStorageType (DevicePath, &Priority, &TypeName))
    {
      continue;
    }

    Context->Luns[Context->LunCount].Handle     = Handles[Index];
    Context->Luns[Context->LunCount].BlockIo    = BlockIo;
    Context->Luns[Context->LunCount].Priority   = Priority;
    Context->Luns[Context->LunCount].TypeName   = TypeName;
    Context->Luns[Context->LunCount].SenseKey   = SCSI_SENSE_UNIT_ATTENTION;
    Context->Luns[Context->LunCount].AdditionalSenseCode = SCSI_ASC_POWER_ON_RESET;
    Context->Luns[Context->LunCount].MediaId = BlockIo->Media->MediaId;
    Context->Luns[Context->LunCount].MediaPresent = BlockIo->Media->MediaPresent;
    Context->LunCount++;
  }

  FreePool (Handles);

  if (Context->LunCount == 0) {
    return EFI_NOT_FOUND;
  }

  SortLuns (Context);
  return EFI_SUCCESS;
}

STATIC
VOID
SetSense (
  IN OUT MASS_STORAGE_LUN  *Lun,
  IN UINT8                 SenseKey,
  IN UINT8                 AdditionalSenseCode,
  IN UINT8                 AdditionalSenseQualifier
  )
{
  Lun->SenseKey                    = SenseKey;
  Lun->AdditionalSenseCode         = AdditionalSenseCode;
  Lun->AdditionalSenseQualifier    = AdditionalSenseQualifier;
}

STATIC
VOID
ClearSense (
  IN OUT MASS_STORAGE_LUN  *Lun
  )
{
  SetSense (Lun, SCSI_SENSE_NO_SENSE, 0, 0);
}

STATIC
VOID
SaveMediaState (
  IN OUT MASS_STORAGE_LUN  *Lun
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;

  Media = Lun->BlockIo->Media;
  if (Media == NULL) {
    Lun->MediaId      = 0;
    Lun->MediaPresent = FALSE;
    return;
  }

  Lun->MediaId      = Media->MediaId;
  Lun->MediaPresent = Media->MediaPresent;
}

STATIC
VOID
DetectMediaChange (
  IN OUT MASS_STORAGE_LUN  *Lun,
  IN BOOLEAN               ProbeRemovableMedia
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  BOOLEAN             Changed;
  EFI_STATUS          Status;

  Media = Lun->BlockIo->Media;
  if (ProbeRemovableMedia &&
      (Media != NULL) &&
      Media->RemovableMedia)
  {
    // Removable Block I/O drivers commonly refresh MediaPresent from Reset().
    // TUR and capacity discovery are natural probe points, avoiding a reset
    // of the removable controller before every data transfer.
    Status = Lun->BlockIo->Reset (Lun->BlockIo, FALSE);
    if (EFI_ERROR (Status) &&
        (Status != EFI_MEDIA_CHANGED) &&
        (Status != EFI_NO_MEDIA))
    {
      DEBUG ((DEBUG_WARN, "MassStorage: removable-media probe failed: %r\n", Status));
    }

    Media = Lun->BlockIo->Media;
  }

  if (Media == NULL) {
    Changed = Lun->MediaPresent;
  } else {
    Changed = (Lun->MediaPresent != Media->MediaPresent) ||
              (Lun->MediaPresent && Media->MediaPresent &&
               (Lun->MediaId != Media->MediaId));
  }

  SaveMediaState (Lun);
  if (Changed) {
    SetSense (
      Lun,
      SCSI_SENSE_UNIT_ATTENTION,
      SCSI_ASC_MEDIUM_CHANGED,
      0
      );
  }
}

STATIC
VOID
CommandFailed (
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN UINT8                     SenseKey,
  IN UINT8                     AdditionalSenseCode,
  IN UINT8                     AdditionalSenseQualifier
  )
{
  SetSense (
    Command->Lun,
    SenseKey,
    AdditionalSenseCode,
    AdditionalSenseQualifier
    );
  if (Command->Csw.Status != USB_MSC_CSW_PHASE_ERROR) {
    Command->Csw.Status = USB_MSC_CSW_FAILED;
  }
}

STATIC
VOID
CommandFailedFromBlockIo (
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN EFI_STATUS                Status,
  IN UINT8                     DefaultSenseKey,
  IN UINT8                     DefaultAdditionalSenseCode,
  IN UINT8                     DefaultAdditionalSenseQualifier
  )
{
  SaveMediaState (Command->Lun);

  switch (Status) {
    case EFI_MEDIA_CHANGED:
      CommandFailed (
        Command,
        SCSI_SENSE_UNIT_ATTENTION,
        SCSI_ASC_MEDIUM_CHANGED,
        0
        );
      break;

    case EFI_NO_MEDIA:
      CommandFailed (
        Command,
        SCSI_SENSE_NOT_READY,
        SCSI_ASC_MEDIUM_NOT_PRESENT,
        0
        );
      break;

    case EFI_WRITE_PROTECTED:
      CommandFailed (
        Command,
        SCSI_SENSE_DATA_PROTECT,
        SCSI_ASC_WRITE_PROTECTED,
        0
        );
      break;

    case EFI_NOT_READY:
      CommandFailed (
        Command,
        SCSI_SENSE_NOT_READY,
        SCSI_ASC_LOGICAL_UNIT_NOT_READY,
        0
        );
      break;

    default:
      CommandFailed (
        Command,
        DefaultSenseKey,
        DefaultAdditionalSenseCode,
        DefaultAdditionalSenseQualifier
        );
      break;
  }
}

STATIC
VOID
CommandPhaseError (
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  Command->Csw.Status = USB_MSC_CSW_PHASE_ERROR;
}

STATIC
BOOLEAN
MediaReady (
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  if ((Command->Lun->BlockIo->Media == NULL) ||
      !Command->Lun->BlockIo->Media->MediaPresent)
  {
    CommandFailed (
      Command,
      SCSI_SENSE_NOT_READY,
      SCSI_ASC_MEDIUM_NOT_PRESENT,
      0
      );
    return FALSE;
  }

  return TRUE;
}

STATIC
BOOLEAN
PrepareDataPhase (
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN SCSI_DATA_DIRECTION       Direction,
  IN UINT64                    ExpectedLength
  )
{
  UINT32  HostLength;

  HostLength = Command->Cbw->DataTransferLength;

  if (ExpectedLength == 0) {
    // BOT cases 4 and 9: the command has no data phase, but the host declared
    // one.  FinishDataPhase() terminates that pipe and the CSW must not pass.
    if ((HostLength != 0) &&
        (Command->Csw.Status == USB_MSC_CSW_PASSED))
    {
      Command->Csw.Status = USB_MSC_CSW_FAILED;
    }

    return TRUE;
  }

  if ((ExpectedLength > MAX_UINT32) ||
      (HostLength == 0) ||
      (Command->HostDirection != Direction))
  {
    CommandPhaseError (Command);
    return FALSE;
  }

  if (HostLength < ExpectedLength) {
    // BOT cases 7 and 13 transfer only the amount the host declared and then
    // report a phase error.  Callers cap their transfer at DataResidue.
    CommandPhaseError (Command);
  }

  // BOT cases 5 and 11 permit the host transfer length to exceed the device
  // intent in either direction.  The residue is reported in the CSW and the
  // remaining host data phase is terminated by FinishDataPhase().
  return TRUE;
}

STATIC
EFI_STATUS
SendData (
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN CONST VOID                *Buffer,
  IN UINTN                     Length
  )
{
  EFI_STATUS  Status;
  UINTN       ToSend;
  UINTN       Transferred;

  ToSend = MIN ((UINTN)Command->Csw.DataResidue, Length);
  if (ToSend == 0) {
    return EFI_SUCCESS;
  }

  Status = Dwc3BulkTransfer (
             Device,
             TRUE,
             (VOID *)Buffer,
             ToSend,
             &Transferred
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Command->BytesTransferred += (UINT32)Transferred;
  Command->Csw.DataResidue   -= (UINT32)Transferred;
  if (Transferred != ToSend) {
    CommandPhaseError (Command);
    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SendResponse (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN CONST VOID                *Buffer,
  IN UINTN                     ResponseLength,
  IN UINTN                     AllocationLength
  )
{
  UINTN  ExpectedLength;

  ExpectedLength = MIN (ResponseLength, AllocationLength);
  if (!PrepareDataPhase (Command, ScsiDataIn, ExpectedLength)) {
    return EFI_SUCCESS;
  }

  return SendData (Device, Command, Buffer, ExpectedLength);
}

STATIC
EFI_STATUS
ScsiInquiry (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  UINT8   *Response;
  UINTN   Length;
  UINT8   PageCode;
  BOOLEAN EnableVitalProductData;

  Response = Context->IoBuffer;
  ZeroMem (Response, 96);

  EnableVitalProductData = (Command->Cbw->CommandBlock[1] & BIT0) != 0;
  PageCode               = Command->Cbw->CommandBlock[2];

  if (!EnableVitalProductData) {
    if (PageCode != 0) {
      CommandFailed (
        Command,
        SCSI_SENSE_ILLEGAL_REQUEST,
        SCSI_ASC_INVALID_FIELD,
        0
        );
      return EFI_SUCCESS;
    }

    Length      = 36;
    Response[0] = 0x00; // Direct-access block device
    Response[1] = Command->Lun->BlockIo->Media->RemovableMedia ? 0x80 : 0x00;
    Response[2] = 0x06; // SPC-4
    Response[3] = 0x02;
    Response[4] = 31;
    CopyMem (&Response[8], "ROCKCHIP", 8);
    CopyMem (&Response[16], "UEFI MASS STORAGE", 16);
    CopyMem (&Response[32], "1.00", 4);
  } else {
    switch (PageCode) {
      case 0x00:
        Length      = 7;
        Response[0] = 0x00;
        Response[1] = 0x00;
        Response[3] = 3;
        Response[4] = 0x00;
        Response[5] = 0x80;
        Response[6] = 0x83;
        break;

      case 0x80:
        Length      = 12;
        Response[0] = 0x00;
        Response[1] = 0x80;
        Response[3] = 8;
        AsciiSPrint (
          (CHAR8 *)&Response[4],
          9,
          "RK%02u%04x",
          Command->Cbw->Lun,
          Command->Lun->BlockIo->Media->MediaId & 0xffffU
          );
        break;

      case 0x83:
        Length       = 20;
        Response[0]  = 0x00;
        Response[1]  = 0x83;
        Response[3]  = 16;
        Response[4]  = 0x02; // ASCII identifier
        Response[5]  = 0x01; // T10 vendor ID
        Response[7]  = 12;
        CopyMem (&Response[8], "ROCKCHIP", 8);
        Response[16] = 'L';
        Response[17] = 'U';
        Response[18] = 'N';
        Response[19] = (UINT8)('0' + Command->Cbw->Lun);
        break;

      default:
        CommandFailed (
          Command,
          SCSI_SENSE_ILLEGAL_REQUEST,
          SCSI_ASC_INVALID_FIELD,
          0
          );
        return EFI_SUCCESS;
    }
  }

  return SendResponse (
           Context,
           Device,
           Command,
           Response,
           Length,
           Command->Cbw->CommandBlock[4]
           );
}

STATIC
EFI_STATUS
ScsiRequestSense (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  UINT8       *Response;
  EFI_STATUS  Status;

  Response = Context->IoBuffer;
  ZeroMem (Response, 18);
  Response[0]  = 0x70;
  Response[2]  = Command->Lun->SenseKey;
  Response[7]  = 10;
  Response[12] = Command->Lun->AdditionalSenseCode;
  Response[13] = Command->Lun->AdditionalSenseQualifier;

  Status = SendResponse (
             Context,
             Device,
             Command,
             Response,
             18,
             Command->Cbw->CommandBlock[4]
             );
  if (!EFI_ERROR (Status) &&
      (Command->Csw.Status == USB_MSC_CSW_PASSED))
  {
    ClearSense (Command->Lun);
  }

  return Status;
}

STATIC
EFI_STATUS
ScsiModeSense (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN BOOLEAN                   TenByte
  )
{
  UINT8  *Response;
  UINTN  HeaderLength;
  UINTN  ResponseLength;
  UINTN  AllocationLength;
  UINT8  PageCode;

  Response     = Context->IoBuffer;
  HeaderLength = TenByte ? 8 : 4;
  ZeroMem (Response, 32);

  if (Command->Lun->BlockIo->Media->ReadOnly) {
    Response[TenByte ? 3 : 2] = 0x80;
  }

  PageCode       = Command->Cbw->CommandBlock[2] & 0x3fU;
  ResponseLength = HeaderLength;
  if ((PageCode == 0x08) || (PageCode == 0x3f)) {
    // Caching mode page.  WCE remains clear because WriteBlocks followed by
    // FlushBlocks is presented as write-through storage.
    Response[HeaderLength]     = 0x08;
    Response[HeaderLength + 1] = 0x0a;
    ResponseLength            += 12;
  } else if (PageCode != 0) {
    CommandFailed (
      Command,
      SCSI_SENSE_ILLEGAL_REQUEST,
      SCSI_ASC_INVALID_FIELD,
      0
      );
    return EFI_SUCCESS;
  }

  if (TenByte) {
    WriteBigEndian16 (Response, (UINT16)(ResponseLength - 2));
    AllocationLength = ReadBigEndian16 (&Command->Cbw->CommandBlock[7]);
  } else {
    Response[0]       = (UINT8)(ResponseLength - 1);
    AllocationLength = Command->Cbw->CommandBlock[4];
  }

  return SendResponse (
           Context,
           Device,
           Command,
           Response,
           ResponseLength,
           AllocationLength
           );
}

STATIC
EFI_STATUS
ScsiReadCapacity10 (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  UINT8   *Response;
  UINT64  LastBlock;

  if (!PrepareDataPhase (Command, ScsiDataIn, 8)) {
    return EFI_SUCCESS;
  }

  if (!MediaReady (Command)) {
    return EFI_SUCCESS;
  }

  Response  = Context->IoBuffer;
  LastBlock = Command->Lun->BlockIo->Media->LastBlock;
  if (((Command->Cbw->CommandBlock[8] & BIT0) != 0) &&
      (ReadBigEndian32 (&Command->Cbw->CommandBlock[2]) > LastBlock))
  {
    CommandFailed (
      Command,
      SCSI_SENSE_ILLEGAL_REQUEST,
      SCSI_ASC_LBA_OUT_OF_RANGE,
      0
      );
    return EFI_SUCCESS;
  }

  WriteBigEndian32 (
    Response,
    (LastBlock > MAX_UINT32) ? MAX_UINT32 : (UINT32)LastBlock
    );
  WriteBigEndian32 (Response + 4, Command->Lun->BlockIo->Media->BlockSize);

  return SendResponse (Context, Device, Command, Response, 8, 8);
}

STATIC
EFI_STATUS
ScsiReadCapacity16 (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  UINT8  *Response;
  UINTN  AllocationLength;

  AllocationLength = ReadBigEndian32 (&Command->Cbw->CommandBlock[10]);
  if (!PrepareDataPhase (
         Command,
         ScsiDataIn,
         MIN ((UINTN)32, AllocationLength)
         ))
  {
    return EFI_SUCCESS;
  }

  if (!MediaReady (Command)) {
    return EFI_SUCCESS;
  }

  if (((Command->Cbw->CommandBlock[14] & BIT0) != 0) &&
      (ReadBigEndian64 (&Command->Cbw->CommandBlock[2]) >
       Command->Lun->BlockIo->Media->LastBlock))
  {
    CommandFailed (
      Command,
      SCSI_SENSE_ILLEGAL_REQUEST,
      SCSI_ASC_LBA_OUT_OF_RANGE,
      0
      );
    return EFI_SUCCESS;
  }

  Response = Context->IoBuffer;
  ZeroMem (Response, 32);
  WriteBigEndian64 (Response, Command->Lun->BlockIo->Media->LastBlock);
  WriteBigEndian32 (Response + 8, Command->Lun->BlockIo->Media->BlockSize);

  return SendResponse (
           Context,
           Device,
           Command,
           Response,
           32,
           AllocationLength
           );
}

STATIC
EFI_STATUS
ScsiReadFormatCapacities (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  UINT8   *Response;
  UINT64  BlockCount;
  UINTN   AllocationLength;

  AllocationLength = ReadBigEndian16 (&Command->Cbw->CommandBlock[7]);
  if (!PrepareDataPhase (
         Command,
         ScsiDataIn,
         MIN ((UINTN)12, AllocationLength)
         ))
  {
    return EFI_SUCCESS;
  }

  if (!MediaReady (Command)) {
    return EFI_SUCCESS;
  }

  Response   = Context->IoBuffer;
  BlockCount = Command->Lun->BlockIo->Media->LastBlock + 1;
  ZeroMem (Response, 12);
  Response[3] = 8;
  WriteBigEndian32 (
    Response + 4,
    (BlockCount > MAX_UINT32) ? MAX_UINT32 : (UINT32)BlockCount
    );
  Response[8] = 0x02; // Formatted media, current capacity
  WriteBigEndian24 (Response + 9, Command->Lun->BlockIo->Media->BlockSize);

  return SendResponse (
           Context,
           Device,
           Command,
           Response,
           12,
           AllocationLength
           );
}

STATIC
EFI_STATUS
ScsiReportLuns (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  UINT8  *Response;
  UINTN  ResponseLength;
  UINTN  AllocationLength;
  UINTN  Index;

  ResponseLength = 8 + (Context->LunCount * 8);
  Response       = Context->IoBuffer;
  ZeroMem (Response, ResponseLength);
  WriteBigEndian32 (Response, (UINT32)(Context->LunCount * 8));

  for (Index = 0; Index < Context->LunCount; Index++) {
    Response[8 + (Index * 8) + 1] = (UINT8)Index;
  }

  AllocationLength = ReadBigEndian32 (&Command->Cbw->CommandBlock[6]);
  return SendResponse (
           Context,
           Device,
           Command,
           Response,
           ResponseLength,
           AllocationLength
           );
}

STATIC
BOOLEAN
ValidateBlockRange (
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN UINT64                    Lba,
  IN UINT32                    BlockCount,
  OUT UINT64                   *ByteCount
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;

  if (!MediaReady (Command)) {
    return FALSE;
  }

  Media = Command->Lun->BlockIo->Media;
  if ((BlockCount != 0) &&
      ((Lba > Media->LastBlock) ||
       ((UINT64)(BlockCount - 1) > (Media->LastBlock - Lba))))
  {
    CommandFailed (
      Command,
      SCSI_SENSE_ILLEGAL_REQUEST,
      SCSI_ASC_LBA_OUT_OF_RANGE,
      0
      );
    return FALSE;
  }

  *ByteCount = MultU64x32 (BlockCount, Media->BlockSize);
  if (*ByteCount > MAX_UINT32) {
    CommandPhaseError (Command);
    return FALSE;
  }

  return TRUE;
}

STATIC
EFI_STATUS
ScsiReadBlocks (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN UINT64                    Lba,
  IN UINT32                    BlockCount
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  UINT64              ByteCount;
  UINT64              Remaining;
  UINTN               BufferCapacity;
  UINTN               IoChunk;
  UINTN               TransferChunk;
  UINTN               Transferred;
  EFI_STATUS          Status;

  Media = Command->Lun->BlockIo->Media;
  if (Media == NULL) {
    MediaReady (Command);
    return EFI_SUCCESS;
  }

  ByteCount = MultU64x32 (BlockCount, Media->BlockSize);
  if (!PrepareDataPhase (Command, ScsiDataIn, ByteCount)) {
    return EFI_SUCCESS;
  }

  if (!ValidateBlockRange (Command, Lba, BlockCount, &ByteCount)) {
    return EFI_SUCCESS;
  }

  Remaining = MIN (ByteCount, Command->Csw.DataResidue);
  BufferCapacity = Context->IoBufferSize -
                   (Context->IoBufferSize % Media->BlockSize);
  if ((Remaining != 0) && (BufferCapacity == 0)) {
    CommandPhaseError (Command);
    return EFI_SUCCESS;
  }

  while (Remaining != 0) {
    TransferChunk = (UINTN)MIN (Remaining, BufferCapacity);
    IoChunk       = (UINTN)DivU64x32 (
                            TransferChunk + Media->BlockSize - 1,
                            Media->BlockSize
                            ) * Media->BlockSize;

    Status = Command->Lun->BlockIo->ReadBlocks (
                                      Command->Lun->BlockIo,
                                      Media->MediaId,
                                      Lba,
                                      IoChunk,
                                      Context->IoBuffer
                                      );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MassStorage: ReadBlocks failed: %r\n", Status));
      CommandFailedFromBlockIo (
        Command,
        Status,
        SCSI_SENSE_MEDIUM_ERROR,
        SCSI_ASC_UNRECOVERED_READ,
        0
        );
      return EFI_SUCCESS;
    }

    Status = Dwc3BulkTransfer (
               Device,
               TRUE,
               Context->IoBuffer,
               TransferChunk,
               &Transferred
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Command->BytesTransferred += (UINT32)Transferred;
    Command->Csw.DataResidue   -= (UINT32)Transferred;
    if (Transferred != TransferChunk) {
      CommandPhaseError (Command);
      return EFI_SUCCESS;
    }

    Remaining -= TransferChunk;
    Lba       += IoChunk / Media->BlockSize;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ScsiWriteBlocks (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN UINT64                    Lba,
  IN UINT32                    BlockCount
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  UINT64              ByteCount;
  UINT64              Remaining;
  UINTN               Chunk;
  UINTN               Transferred;
  EFI_STATUS          Status;

  Media = Command->Lun->BlockIo->Media;
  if (Media == NULL) {
    MediaReady (Command);
    return EFI_SUCCESS;
  }

  ByteCount = MultU64x32 (BlockCount, Media->BlockSize);
  if (!PrepareDataPhase (Command, ScsiDataOut, ByteCount)) {
    return EFI_SUCCESS;
  }

  if (!ValidateBlockRange (Command, Lba, BlockCount, &ByteCount)) {
    return EFI_SUCCESS;
  }

  if (Media->ReadOnly) {
    CommandFailed (
      Command,
      SCSI_SENSE_DATA_PROTECT,
      SCSI_ASC_WRITE_PROTECTED,
      0
      );
    return EFI_SUCCESS;
  }

  Remaining = MIN (ByteCount, Command->Csw.DataResidue);
  while (Remaining != 0) {
    Chunk = (UINTN)MIN (Remaining, Context->IoBufferSize);
    if (Chunk >= Media->BlockSize) {
      Chunk -= Chunk % Media->BlockSize;
    }

    Status = Dwc3BulkTransfer (
               Device,
               FALSE,
               Context->IoBuffer,
               Chunk,
               &Transferred
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Command->BytesTransferred += (UINT32)Transferred;
    if (Transferred != Chunk) {
      CommandPhaseError (Command);
      return EFI_SUCCESS;
    }

    Remaining -= Transferred;
    if ((Transferred % Media->BlockSize) != 0) {
      // A short host intent may end inside a logical block.  Consume those
      // bytes for BOT synchronization but never issue a partial Block I/O
      // write; the phase-error residue reports them as unprocessed.
      CommandPhaseError (Command);
      return EFI_SUCCESS;
    }

    Status = Command->Lun->BlockIo->WriteBlocks (
                                      Command->Lun->BlockIo,
                                      Media->MediaId,
                                      Lba,
                                      Chunk,
                                      Context->IoBuffer
                                      );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MassStorage: WriteBlocks failed: %r\n", Status));
      CommandFailedFromBlockIo (
        Command,
        Status,
        SCSI_SENSE_MEDIUM_ERROR,
        SCSI_ASC_WRITE_ERROR,
        2
        );
      return EFI_SUCCESS;
    }

    Command->Csw.DataResidue -= (UINT32)Transferred;
    Lba                      += Chunk / Media->BlockSize;
  }

  // Advertise write-through caching and make that promise true.
  Status = Command->Lun->BlockIo->FlushBlocks (Command->Lun->BlockIo);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MassStorage: FlushBlocks failed: %r\n", Status));
    CommandFailedFromBlockIo (
      Command,
      Status,
      SCSI_SENSE_MEDIUM_ERROR,
      SCSI_ASC_WRITE_ERROR,
      2
      );
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ScsiSynchronizeCache (
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  EFI_STATUS  Status;

  if (!PrepareDataPhase (Command, ScsiDataNone, 0)) {
    return EFI_SUCCESS;
  }

  Status = Command->Lun->BlockIo->FlushBlocks (Command->Lun->BlockIo);
  if (EFI_ERROR (Status)) {
    CommandFailedFromBlockIo (
      Command,
      Status,
      SCSI_SENSE_MEDIUM_ERROR,
      SCSI_ASC_WRITE_ERROR,
      2
      );
  }

  return EFI_SUCCESS;
}

STATIC
UINT8
ExpectedCdbLength (
  IN UINT8  Opcode
  )
{
  switch (Opcode) {
    case SCSI_TEST_UNIT_READY:
    case SCSI_REQUEST_SENSE:
    case SCSI_READ_6:
    case SCSI_WRITE_6:
    case SCSI_INQUIRY:
    case SCSI_MODE_SENSE_6:
    case SCSI_START_STOP_UNIT:
    case SCSI_PREVENT_ALLOW_MEDIUM:
      return 6;

    case SCSI_READ_FORMAT_CAPACITIES:
    case SCSI_READ_CAPACITY_10:
    case SCSI_READ_10:
    case SCSI_WRITE_10:
    case SCSI_VERIFY_10:
    case SCSI_SYNCHRONIZE_CACHE_10:
    case SCSI_MODE_SENSE_10:
      return 10;

    case SCSI_REPORT_LUNS:
    case SCSI_READ_12:
    case SCSI_WRITE_12:
      return 12;

    case SCSI_READ_16:
    case SCSI_WRITE_16:
    case SCSI_VERIFY_16:
    case SCSI_SYNCHRONIZE_CACHE_16:
    case SCSI_SERVICE_ACTION_IN_16:
      return 16;

    default:
      return 0;
  }
}

STATIC
BOOLEAN
ValidateCdbFields (
  IN OUT MASS_STORAGE_COMMAND  *Command,
  IN UINT8                     Opcode
  )
{
  UINT8    *Cdb;
  BOOLEAN  Valid;

  Cdb   = Command->Cbw->CommandBlock;
  Valid = TRUE;

  switch (Opcode) {
    case SCSI_TEST_UNIT_READY:
      Valid = ((Cdb[1] & 0x1fU) == 0) && IsZeroBuffer (&Cdb[2], 3);
      break;

    case SCSI_REQUEST_SENSE:
      // Only the fixed-format sense response is implemented (DESC must be 0).
      Valid = ((Cdb[1] & 0x1fU) == 0) && IsZeroBuffer (&Cdb[2], 2);
      break;

    case SCSI_INQUIRY:
      Valid = ((Cdb[1] & 0x1eU) == 0) && (Cdb[3] == 0);
      break;

    case SCSI_MODE_SENSE_6:
      // DBD is harmless because this target returns no block descriptor.
      Valid = ((Cdb[1] & 0x17U) == 0) &&
              ((Cdb[2] & 0xc0U) == 0) &&
              (Cdb[3] == 0);
      break;

    case SCSI_START_STOP_UNIT:
      Valid = ((Cdb[1] & 0x1eU) == 0) &&
              IsZeroBuffer (&Cdb[2], 2) &&
              ((Cdb[4] & 0xfcU) == 0);
      break;

    case SCSI_PREVENT_ALLOW_MEDIUM:
      Valid = ((Cdb[1] & 0x1fU) == 0) &&
              IsZeroBuffer (&Cdb[2], 2) &&
              ((Cdb[4] & 0xfcU) == 0);
      break;

    case SCSI_READ_FORMAT_CAPACITIES:
      Valid = (Cdb[1] == 0) && IsZeroBuffer (&Cdb[2], 5);
      break;

    case SCSI_READ_CAPACITY_10:
      Valid = (Cdb[1] == 0) &&
              IsZeroBuffer (&Cdb[6], 2) &&
              ((Cdb[8] & 0xfeU) == 0) &&
              (((Cdb[8] & BIT0) != 0) || IsZeroBuffer (&Cdb[2], 4));
      break;

    case SCSI_READ_10:
    case SCSI_WRITE_10:
      Valid = ((Cdb[1] & ~0x18U) == 0) && ((Cdb[6] & 0xe0U) == 0);
      break;

    case SCSI_VERIFY_10:
      Valid = ((Cdb[1] & ~BIT4) == 0) && ((Cdb[6] & 0xe0U) == 0);
      break;

    case SCSI_SYNCHRONIZE_CACHE_10:
      Valid = ((Cdb[1] & ~0x06U) == 0) && ((Cdb[6] & 0xe0U) == 0);
      break;

    case SCSI_MODE_SENSE_10:
      Valid = ((Cdb[1] & ~0x18U) == 0) &&
              ((Cdb[2] & 0xc0U) == 0) &&
              (Cdb[3] == 0) &&
              IsZeroBuffer (&Cdb[4], 3);
      break;

    case SCSI_REPORT_LUNS:
      Valid = (Cdb[1] == 0) &&
              (Cdb[2] == 0) &&
              IsZeroBuffer (&Cdb[3], 3) &&
              (Cdb[10] == 0);
      break;

    case SCSI_READ_12:
    case SCSI_WRITE_12:
      Valid = ((Cdb[1] & ~0x18U) == 0) && ((Cdb[10] & 0xe0U) == 0);
      break;

    case SCSI_READ_16:
    case SCSI_WRITE_16:
      Valid = ((Cdb[1] & ~0x18U) == 0) && ((Cdb[14] & 0xe0U) == 0);
      break;

    case SCSI_VERIFY_16:
      Valid = ((Cdb[1] & ~BIT4) == 0) && ((Cdb[14] & 0xe0U) == 0);
      break;

    case SCSI_SYNCHRONIZE_CACHE_16:
      Valid = ((Cdb[1] & ~0x06U) == 0) && ((Cdb[14] & 0xe0U) == 0);
      break;

    case SCSI_SERVICE_ACTION_IN_16:
      Valid = ((Cdb[1] & 0xe0U) == 0) &&
              ((Cdb[14] & 0xfeU) == 0) &&
              (((Cdb[14] & BIT0) != 0) || IsZeroBuffer (&Cdb[2], 8));
      break;

    default:
      break;
  }

  if (!Valid) {
    CommandFailed (
      Command,
      SCSI_SENSE_ILLEGAL_REQUEST,
      SCSI_ASC_INVALID_FIELD,
      0
      );
  }

  return Valid;
}

STATIC
EFI_STATUS
ExecuteScsiCommand (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN OUT MASS_STORAGE_COMMAND  *Command
  )
{
  UINT8   Opcode;
  UINT64  Lba;
  UINT64  ByteCount;
  UINT32  BlockCount;
  UINT8   *Response;
  UINT8   CdbLength;
  EFI_STATUS  Status;

  Opcode = Command->Cbw->CommandBlock[0];
  DEBUG ((
    DEBUG_VERBOSE,
    "MassStorage: LUN %u SCSI opcode 0x%02x, host length %u\n",
    Command->Cbw->Lun,
    Opcode,
    Command->Cbw->DataTransferLength
    ));

  CdbLength = ExpectedCdbLength (Opcode);
  if ((CdbLength != 0) && (Command->Cbw->CommandLength != CdbLength)) {
    CommandFailed (
      Command,
      SCSI_SENSE_ILLEGAL_REQUEST,
      SCSI_ASC_INVALID_FIELD,
      0
      );
    return EFI_SUCCESS;
  }

  if (!ValidateCdbFields (Command, Opcode)) {
    return EFI_SUCCESS;
  }

  DetectMediaChange (
    Command->Lun,
    (Opcode == SCSI_TEST_UNIT_READY) ||
    (Opcode == SCSI_READ_FORMAT_CAPACITIES) ||
    (Opcode == SCSI_READ_CAPACITY_10) ||
    (Opcode == SCSI_SERVICE_ACTION_IN_16)
    );

  // SPC requires a pending Unit Attention to fail commands until it is
  // reported by REQUEST SENSE.  Discovery commands remain usable while the
  // condition is pending and do not consume it.
  if ((Command->Lun->SenseKey == SCSI_SENSE_UNIT_ATTENTION) &&
      (Opcode != SCSI_REQUEST_SENSE) &&
      (Opcode != SCSI_INQUIRY) &&
      (Opcode != SCSI_REPORT_LUNS))
  {
    Command->Csw.Status = USB_MSC_CSW_FAILED;
    return EFI_SUCCESS;
  }

  switch (Opcode) {
    case SCSI_TEST_UNIT_READY:
      if (!PrepareDataPhase (Command, ScsiDataNone, 0)) {
        return EFI_SUCCESS;
      }

      MediaReady (Command);
      return EFI_SUCCESS;

    case SCSI_REQUEST_SENSE:
      return ScsiRequestSense (Context, Device, Command);

    case SCSI_INQUIRY:
      return ScsiInquiry (Context, Device, Command);

    case SCSI_MODE_SENSE_6:
      return ScsiModeSense (Context, Device, Command, FALSE);

    case SCSI_MODE_SENSE_10:
      return ScsiModeSense (Context, Device, Command, TRUE);

    case SCSI_START_STOP_UNIT:
      if (!PrepareDataPhase (Command, ScsiDataNone, 0)) {
        return EFI_SUCCESS;
      }

      if (((Command->Cbw->CommandBlock[4] & BIT1) != 0) ||
          ((Command->Cbw->CommandBlock[4] & BIT0) == 0))
      {
        Status = Command->Lun->BlockIo->FlushBlocks (Command->Lun->BlockIo);
        if (EFI_ERROR (Status)) {
          CommandFailedFromBlockIo (
            Command,
            Status,
            SCSI_SENSE_MEDIUM_ERROR,
            SCSI_ASC_WRITE_ERROR,
            2
            );
        }
      }

      return EFI_SUCCESS;

    case SCSI_PREVENT_ALLOW_MEDIUM:
      PrepareDataPhase (Command, ScsiDataNone, 0);
      return EFI_SUCCESS;

    case SCSI_READ_FORMAT_CAPACITIES:
      return ScsiReadFormatCapacities (Context, Device, Command);

    case SCSI_READ_CAPACITY_10:
      return ScsiReadCapacity10 (Context, Device, Command);

    case SCSI_SERVICE_ACTION_IN_16:
      if ((Command->Cbw->CommandBlock[1] & 0x1fU) != SCSI_SAI_READ_CAPACITY_16) {
        CommandFailed (
          Command,
          SCSI_SENSE_ILLEGAL_REQUEST,
          SCSI_ASC_INVALID_FIELD,
          0
          );
        return EFI_SUCCESS;
      }

      return ScsiReadCapacity16 (Context, Device, Command);

    case SCSI_REPORT_LUNS:
      return ScsiReportLuns (Context, Device, Command);

    case SCSI_READ_6:
      Lba = ((UINT32)(Command->Cbw->CommandBlock[1] & 0x1fU) << 16) |
            ((UINT32)Command->Cbw->CommandBlock[2] << 8) |
            Command->Cbw->CommandBlock[3];
      BlockCount = Command->Cbw->CommandBlock[4];
      if (BlockCount == 0) {
        BlockCount = 256;
      }

      return ScsiReadBlocks (Context, Device, Command, Lba, BlockCount);

    case SCSI_WRITE_6:
      Lba = ((UINT32)(Command->Cbw->CommandBlock[1] & 0x1fU) << 16) |
            ((UINT32)Command->Cbw->CommandBlock[2] << 8) |
            Command->Cbw->CommandBlock[3];
      BlockCount = Command->Cbw->CommandBlock[4];
      if (BlockCount == 0) {
        BlockCount = 256;
      }

      return ScsiWriteBlocks (Context, Device, Command, Lba, BlockCount);

    case SCSI_READ_10:
      Lba        = ReadBigEndian32 (&Command->Cbw->CommandBlock[2]);
      BlockCount = ReadBigEndian16 (&Command->Cbw->CommandBlock[7]);
      return ScsiReadBlocks (Context, Device, Command, Lba, BlockCount);

    case SCSI_WRITE_10:
      Lba        = ReadBigEndian32 (&Command->Cbw->CommandBlock[2]);
      BlockCount = ReadBigEndian16 (&Command->Cbw->CommandBlock[7]);
      return ScsiWriteBlocks (Context, Device, Command, Lba, BlockCount);

    case SCSI_READ_12:
      Lba        = ReadBigEndian32 (&Command->Cbw->CommandBlock[2]);
      BlockCount = ReadBigEndian32 (&Command->Cbw->CommandBlock[6]);
      return ScsiReadBlocks (Context, Device, Command, Lba, BlockCount);

    case SCSI_WRITE_12:
      Lba        = ReadBigEndian32 (&Command->Cbw->CommandBlock[2]);
      BlockCount = ReadBigEndian32 (&Command->Cbw->CommandBlock[6]);
      return ScsiWriteBlocks (Context, Device, Command, Lba, BlockCount);

    case SCSI_READ_16:
      Lba        = ReadBigEndian64 (&Command->Cbw->CommandBlock[2]);
      BlockCount = ReadBigEndian32 (&Command->Cbw->CommandBlock[10]);
      return ScsiReadBlocks (Context, Device, Command, Lba, BlockCount);

    case SCSI_WRITE_16:
      Lba        = ReadBigEndian64 (&Command->Cbw->CommandBlock[2]);
      BlockCount = ReadBigEndian32 (&Command->Cbw->CommandBlock[10]);
      return ScsiWriteBlocks (Context, Device, Command, Lba, BlockCount);

    case SCSI_VERIFY_10:
      if ((Command->Cbw->CommandBlock[1] & BIT1) != 0) {
        CommandFailed (
          Command,
          SCSI_SENSE_ILLEGAL_REQUEST,
          SCSI_ASC_INVALID_FIELD,
          0
          );
      } else {
        Lba        = ReadBigEndian32 (&Command->Cbw->CommandBlock[2]);
        BlockCount = ReadBigEndian16 (&Command->Cbw->CommandBlock[7]);
        if (ValidateBlockRange (Command, Lba, BlockCount, &ByteCount)) {
          PrepareDataPhase (Command, ScsiDataNone, 0);
        }
      }

      return EFI_SUCCESS;

    case SCSI_VERIFY_16:
      if ((Command->Cbw->CommandBlock[1] & BIT1) != 0) {
        CommandFailed (
          Command,
          SCSI_SENSE_ILLEGAL_REQUEST,
          SCSI_ASC_INVALID_FIELD,
          0
          );
      } else {
        Lba        = ReadBigEndian64 (&Command->Cbw->CommandBlock[2]);
        BlockCount = ReadBigEndian32 (&Command->Cbw->CommandBlock[10]);
        if (ValidateBlockRange (Command, Lba, BlockCount, &ByteCount)) {
          PrepareDataPhase (Command, ScsiDataNone, 0);
        }
      }

      return EFI_SUCCESS;

    case SCSI_SYNCHRONIZE_CACHE_10:
    case SCSI_SYNCHRONIZE_CACHE_16:
      return ScsiSynchronizeCache (Command);

    default:
      Response = Context->IoBuffer;
      Response[0] = Opcode;
      DEBUG ((DEBUG_WARN, "MassStorage: unsupported SCSI opcode 0x%02x\n", Response[0]));
      CommandFailed (
        Command,
        SCSI_SENSE_ILLEGAL_REQUEST,
        SCSI_ASC_INVALID_OPCODE,
        0
        );
      return EFI_SUCCESS;
  }
}

STATIC
BOOLEAN
ValidateCbw (
  IN MASS_STORAGE_CONTEXT  *Context,
  IN USB_MSC_CBW           *Cbw,
  IN UINTN                 Received
  )
{
  if ((Received != sizeof (*Cbw)) ||
      (Cbw->Signature != USB_MSC_CBW_SIGNATURE) ||
      ((Cbw->Flags & ~USB_MSC_CBW_FLAG_IN) != 0) ||
      ((Cbw->Lun & 0xf0U) != 0) ||
      ((Cbw->CommandLength == 0) || (Cbw->CommandLength > 16)))
  {
    return FALSE;
  }

  return Cbw->Lun < Context->LunCount;
}

STATIC
EFI_STATUS
StallDataEndpointAndWait (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN MASS_STORAGE_COMMAND      *Command,
  IN BOOLEAN                   DirectionIn
  )
{
  EFI_STATUS  Status;

  Status = Dwc3BulkSetStall (Device, DirectionIn, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Dwc3BulkWaitForClearStall (Device, DirectionIn);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Context->ResetGeneration != Command->ResetGeneration) {
    return EFI_ABORTED;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
FinishDataPhase (
  IN MASS_STORAGE_CONTEXT      *Context,
  IN DWC3_DEVICE               *Device,
  IN MASS_STORAGE_COMMAND      *Command
  )
{
  if ((Command->Cbw->DataTransferLength == 0) ||
      (Command->BytesTransferred >= Command->Cbw->DataTransferLength))
  {
    return EFI_SUCCESS;
  }

  // BOT cases 4, 5, 9 and 11, as well as a command failure with an unfinished
  // data phase, require the host's declared data pipe to be terminated before
  // the CSW can be sent.  Waiting for ClearFeature also guarantees that a CSW
  // cannot be consumed as trailing IN data.
  return StallDataEndpointAndWait (
           Context,
           Device,
           Command,
           Command->HostDirection == ScsiDataIn
           );
}

EFI_STATUS
MassStorageCreate (
  OUT MASS_STORAGE_CONTEXT  **Context
  )
{
  MASS_STORAGE_CONTEXT  *NewContext;
  EFI_STATUS            Status;
  UINTN                 Alignment;
  UINTN                 Index;
  UINT32                IoAlign;

  if (Context == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Context   = NULL;
  NewContext = AllocateZeroPool (sizeof (*NewContext));
  if (NewContext == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  NewContext->Signature = MASS_STORAGE_SIGNATURE;
  NewContext->IoBufferSize = MASS_STORAGE_IO_BUFFER_SIZE;
  Status = EnumeratePhysicalDisks (NewContext);
  if (EFI_ERROR (Status)) {
    FreePool (NewContext);
    return Status;
  }

  Alignment = EFI_PAGE_SIZE;
  for (Index = 0; Index < NewContext->LunCount; Index++) {
    IoAlign = NewContext->Luns[Index].BlockIo->Media->IoAlign;
    NewContext->IoBufferSize = MAX (
                                 NewContext->IoBufferSize,
                                 (UINTN)NewContext->Luns[Index].BlockIo->Media->BlockSize
                                 );
    if (IoAlign <= 1) {
      continue;
    }

    if (!IS_POW2 (IoAlign)) {
      DEBUG ((
        DEBUG_ERROR,
        "MassStorage: LUN %u reports invalid IoAlign %u\n",
        Index,
        IoAlign
        ));
      FreePool (NewContext);
      return EFI_UNSUPPORTED;
    }

    Alignment = MAX (Alignment, (UINTN)IoAlign);
  }

  NewContext->IoBufferPages = EFI_SIZE_TO_PAGES (NewContext->IoBufferSize);
  NewContext->IoBufferSize  = EFI_PAGES_TO_SIZE (NewContext->IoBufferPages);
  Status = DmaAllocateAlignedBuffer (
             EfiBootServicesData,
             NewContext->IoBufferPages,
             Alignment,
             &NewContext->IoBuffer
             );
  if (EFI_ERROR (Status)) {
    FreePool (NewContext);
    return Status;
  }

  ZeroMem (NewContext->IoBuffer, NewContext->IoBufferSize);
  *Context = NewContext;
  return EFI_SUCCESS;
}

VOID
MassStorageDestroy (
  IN MASS_STORAGE_CONTEXT  *Context
  )
{
  if ((Context == NULL) || (Context->Signature != MASS_STORAGE_SIGNATURE)) {
    return;
  }

  MassStorageFlushAll (Context);
  if (Context->IoBuffer != NULL) {
    DmaFreeBuffer (Context->IoBufferPages, Context->IoBuffer);
  }

  Context->Signature = 0;
  FreePool (Context);
}

VOID
MassStoragePrintLuns (
  IN MASS_STORAGE_CONTEXT  *Context
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  UINT64              CapacityMiB;
  UINTN               Index;

  if ((Context == NULL) || (Context->Signature != MASS_STORAGE_SIGNATURE)) {
    return;
  }

  for (Index = 0; Index < Context->LunCount; Index++) {
    Media       = Context->Luns[Index].BlockIo->Media;
    CapacityMiB = DivU64x32 (
                    MultU64x32 (Media->LastBlock + 1, Media->BlockSize),
                    SIZE_1MB
                    );
    Print (
      L"  LUN %u: %s, %Lu MiB, block %u%s%s\n",
      Index,
      Context->Luns[Index].TypeName,
      CapacityMiB,
      Media->BlockSize,
      Media->ReadOnly ? L", read-only" : L"",
      Media->RemovableMedia ? L", removable" : L""
      );
  }
}

UINT8
MassStorageGetMaxLun (
  IN MASS_STORAGE_CONTEXT  *Context
  )
{
  if ((Context == NULL) ||
      (Context->Signature != MASS_STORAGE_SIGNATURE) ||
      (Context->LunCount == 0))
  {
    return 0;
  }

  return (UINT8)(Context->LunCount - 1);
}

VOID
MassStorageFlushAll (
  IN MASS_STORAGE_CONTEXT  *Context
  )
{
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINTN                  Index;

  if ((Context == NULL) || (Context->Signature != MASS_STORAGE_SIGNATURE)) {
    return;
  }

  for (Index = 0; Index < Context->LunCount; Index++) {
    BlockIo = Context->Luns[Index].BlockIo;
    if ((BlockIo == NULL) || (BlockIo->FlushBlocks == NULL)) {
      continue;
    }

    BlockIo->FlushBlocks (BlockIo);
  }
}

VOID
EFIAPI
MassStorageReset (
  IN VOID  *Context
  )
{
  MASS_STORAGE_CONTEXT  *MassStorage;
  UINTN                 Index;

  MassStorage = Context;
  if ((MassStorage == NULL) ||
      (MassStorage->Signature != MASS_STORAGE_SIGNATURE))
  {
    return;
  }

  // A disconnect or reset can interrupt a multi-chunk write before its normal
  // end-of-command flush.  Commit every completed block before recovery.
  MassStorageFlushAll (MassStorage);
  MassStorage->NeedsResetRecovery = FALSE;
  MassStorage->ResetGeneration++;
  for (Index = 0; Index < MassStorage->LunCount; Index++) {
    SetSense (
      &MassStorage->Luns[Index],
      SCSI_SENSE_UNIT_ATTENTION,
      SCSI_ASC_POWER_ON_RESET,
      0
      );
  }
}

EFI_STATUS
MassStorageProcessCommand (
  IN MASS_STORAGE_CONTEXT  *Context,
  IN DWC3_DEVICE           *Device
  )
{
  USB_MSC_CBW          *Cbw;
  MASS_STORAGE_COMMAND Command;
  UINTN                Received;
  UINTN                Sent;
  EFI_STATUS           Status;

  if ((Context == NULL) ||
      (Context->Signature != MASS_STORAGE_SIGNATURE) ||
      (Device == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (!Dwc3DeviceIsConfigured (Device)) {
    Status = Dwc3DevicePoll (Device);
    return ((Status == EFI_SUCCESS) || (Status == EFI_NOT_READY)) ?
           EFI_NOT_READY : Status;
  }

  if (Context->NeedsResetRecovery) {
    Status = Dwc3DevicePoll (Device);
    return ((Status == EFI_SUCCESS) || (Status == EFI_NOT_READY)) ?
           EFI_NOT_READY : Status;
  }

  ZeroMem (Context->CbwBuffer, sizeof (Context->CbwBuffer));
  Status = Dwc3BulkTransfer (
             Device,
             FALSE,
             Context->CbwBuffer,
             sizeof (Context->CbwBuffer),
             &Received
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cbw = (USB_MSC_CBW *)Context->CbwBuffer;
  if (!ValidateCbw (Context, Cbw, Received)) {
    DEBUG ((
      DEBUG_ERROR,
      "MassStorage: invalid CBW (length %u, signature %08x)\n",
      Received,
      Cbw->Signature
      ));
    Context->NeedsResetRecovery = TRUE;
    Dwc3BulkSetStall (Device, FALSE, TRUE);
    Dwc3BulkSetStall (Device, TRUE, TRUE);
    return EFI_PROTOCOL_ERROR;
  }

  ZeroMem (&Command, sizeof (Command));
  Command.Cbw             = Cbw;
  Command.Lun             = &Context->Luns[Cbw->Lun];
  Command.HostDirection   = ((Cbw->Flags & USB_MSC_CBW_FLAG_IN) != 0) ?
                            ScsiDataIn : ScsiDataOut;
  Command.Csw.Signature   = USB_MSC_CSW_SIGNATURE;
  Command.Csw.Tag         = Cbw->Tag;
  Command.Csw.DataResidue = Cbw->DataTransferLength;
  Command.Csw.Status      = USB_MSC_CSW_PASSED;
  Command.ResetGeneration = Context->ResetGeneration;

  Status = ExecuteScsiCommand (Context, Device, &Command);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FinishDataPhase (Context, Device, &Command);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Dwc3BulkTransfer (
             Device,
             TRUE,
             &Command.Csw,
             sizeof (Command.Csw),
             &Sent
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Sent != sizeof (Command.Csw)) {
    return EFI_DEVICE_ERROR;
  }

  if (Command.Csw.Status == USB_MSC_CSW_PHASE_ERROR) {
    Context->NeedsResetRecovery = TRUE;
  }

  return EFI_SUCCESS;
}
