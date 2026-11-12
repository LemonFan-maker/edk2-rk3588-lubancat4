/** @file
  Minimal Synopsys DWC3 USB device-mode implementation for RK3588.

  The implementation is deliberately polling-only and USB 2.0-only.  Keeping
  SuperSpeed disabled avoids depending on a Type-C orientation/alternate-mode
  mux while firmware owns the port.  Endpoint zero and one pair of bulk
  endpoints are sufficient for USB Mass Storage Bulk-Only Transport.

  Copyright (c) 2026, OrionisLi <cyan2254650260@gmail.com>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <IndustryStandard/Usb.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DmaLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "Dwc3Device.h"

#define DWC3_DEVICE_SIGNATURE  SIGNATURE_32 ('D', 'W', 'C', 'D')

#define DWC3_REG(Base, Offset)  ((UINTN)(Base) + (Offset))

// DWC3 global registers.  ControllerBase points at the xHCI capability range;
// the DWC3 global register window starts at offset 0xc100.
#define DWC3_GSBUSCFG0       0xc100
#define DWC3_GSBUSCFG1       0xc104
#define DWC3_GCTL            0xc110
#define DWC3_GUCTL1          0xc11c
#define DWC3_GSNPSID         0xc120
#define DWC3_GHWPARAMS1      0xc144
#define DWC3_GUSB2PHYCFG0    0xc200
#define DWC3_GUSB3PIPECTL0   0xc2c0
#define DWC3_GEVNTADRLO0     0xc400
#define DWC3_GEVNTADRHI0     0xc404
#define DWC3_GEVNTSIZ0       0xc408
#define DWC3_GEVNTCOUNT0     0xc40c
#define DWC3_GFLADJ          0xc630
#define DWC3_DCFG            0xc700
#define DWC3_DCTL            0xc704
#define DWC3_DEVTEN          0xc708
#define DWC3_DSTS            0xc70c
#define DWC3_DALEPENA        0xc720
#define DWC3_DEPCMDPAR2(Ep)  (0xc800 + ((Ep) * 0x10))
#define DWC3_DEPCMDPAR1(Ep)  (0xc804 + ((Ep) * 0x10))
#define DWC3_DEPCMDPAR0(Ep)  (0xc808 + ((Ep) * 0x10))
#define DWC3_DEPCMD(Ep)      (0xc80c + ((Ep) * 0x10))

#define DWC3_GSNPSID_MASK  0xffff0000U
#define DWC3_SYNOPSYS_ID   0x55330000U
#define DWC3_RELEASE_MASK  0x0000ffffU
#define DWC3_RELEASE_290A  0x290aU

#define DWC3_GCTL_PRTCAPDIR_MASK    (3U << 12)
#define DWC3_GCTL_PRTCAP_DEVICE     (2U << 12)
#define DWC3_GCTL_CORESOFTRESET     BIT11
#define DWC3_GCTL_SCALEDOWN_MASK    (3U << 4)
#define DWC3_GCTL_DISSCRAMBLE       BIT3
#define DWC3_GCTL_DSBLCLKGTNG       BIT0
#define DWC3_GUCTL1_TX_IPGAP_LINECHECK_DIS    BIT28
#define DWC3_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK BIT26
#define DWC3_GUCTL1_PARKMODE_DISABLE_SS       BIT17
#define DWC3_GUCTL1_PARKMODE_DISABLE_HS       BIT16
#define DWC3_GHWPARAMS1_EN_PWROPT   (3U << 24)
#define DWC3_GHWPARAMS1_EN_PWROPT_CLK  (1U << 24)

#define DWC3_GUSB2PHYCFG_PHYSOFTRST         BIT31
#define DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS  BIT30
#define DWC3_GUSB2PHYCFG_USBTRDTIM_MASK     (0xfU << 10)
#define DWC3_GUSB2PHYCFG_USBTRDTIM(Value)   (((Value) & 0xfU) << 10)
#define DWC3_GUSB2PHYCFG_SUSPHY             BIT6
#define DWC3_GUSB2PHYCFG_PHYIF              BIT3
#define DWC3_GUSB2PHYCFG_ENBLSLPM           BIT8
#define DWC3_GUSB3PIPECTL_PHYSOFTRST        BIT31
#define DWC3_GUSB3PIPECTL_SUSPHY            BIT17
#define DWC3_GUSB3PIPECTL_DEPOCHANGE        BIT18

#define DWC3_GFLADJ_30MHZ_REG_SEL  BIT7
#define DWC3_GFLADJ_30MHZ_DEFAULT  0x20U

#define DWC3_GSBUSCFG0_BURST_MASK   0xffU
#define DWC3_GSBUSCFG0_BURST_ENABLE 0x0fU
#define DWC3_GSBUSCFG1_BURST_LIMIT  0x0f00U

#define DWC3_DCFG_DEVADDR_MASK       (0x7fU << 3)
#define DWC3_DCFG_DEVADDR(Address)   (((Address) & 0x7fU) << 3)
#define DWC3_DCFG_SPEED_MASK         0x7U
#define DWC3_DCFG_SPEED_HS           0U

#define DWC3_DCTL_RUN_STOP   BIT31
#define DWC3_DCTL_CSFTRST    BIT30
#define DWC3_DSTS_DEVCTRLHLT BIT22
#define DWC3_DSTS_CONNECTSPD_MASK  0x7U
#define DWC3_DSTS_CONNECTSPD_HS    0U

#define DWC3_DEVTEN_DISCONNEVTEN  BIT0
#define DWC3_DEVTEN_USBRSTEVTEN   BIT1
#define DWC3_DEVTEN_CONNECTDONEEN BIT2
#define DWC3_DEVTEN_ULSTCNGEN     BIT3
#define DWC3_DEVTEN_ERRTICERREN   BIT9
#define DWC3_DEVTEN_EVNTOVERFLOWEN BIT11

#define DWC3_EVENT_BUFFER_SIZE  1024U
#define DWC3_EVENT_COUNT_MASK   0xfffcU
#define DWC3_COMMON_BUFFER_SIZE EFI_PAGE_SIZE

#define DWC3_DEVICE_EVENT_DISCONNECT       0U
#define DWC3_DEVICE_EVENT_RESET            1U
#define DWC3_DEVICE_EVENT_CONNECT_DONE     2U
#define DWC3_DEVICE_EVENT_LINK_STATUS      3U
#define DWC3_DEVICE_EVENT_ERRATIC_ERROR    9U
#define DWC3_DEVICE_EVENT_BUFFER_OVERFLOW  11U

#define DWC3_DEPEVT_XFERCOMPLETE    1U
#define DWC3_DEPEVT_XFERINPROGRESS  2U
#define DWC3_DEPEVT_XFERNOTREADY    3U
#define DWC3_DEPEVT_EPCMDCMPLT      7U
#define DWC3_DEPEVT_STATUS_BUSERR    BIT0

#define DWC3_DEPCMD_CMDTYPE_MASK   0x0fU
#define DWC3_DEPCMD_SETEPCONFIG    1U
#define DWC3_DEPCMD_SETTRANSFRESOURCE 2U
#define DWC3_DEPCMD_GETEPSTATE     3U
#define DWC3_DEPCMD_SETSTALL       4U
#define DWC3_DEPCMD_CLEARSTALL     5U
#define DWC3_DEPCMD_STARTTRANSFER  6U
#define DWC3_DEPCMD_UPDATETRANSFER 7U
#define DWC3_DEPCMD_ENDTRANSFER    8U
#define DWC3_DEPCMD_DEPSTARTCFG    9U
#define DWC3_DEPCMD_CMDIOC         BIT8
#define DWC3_DEPCMD_HIPRI_FORCERM  BIT11
#define DWC3_DEPCMD_CMDACT         BIT10
#define DWC3_DEPCMD_STATUS_MASK    (0xfU << 12)
#define DWC3_DEPCMD_PARAM(Value)   (((Value) & 0x7fU) << 16)
#define DWC3_DEPCMD_GET_RSC_IDX(Value)  (((Value) >> 16) & 0x7fU)

#define DWC3_DEPCFG_EP_TYPE(Type)          (((Type) & 3U) << 1)
#define DWC3_DEPCFG_MAX_PACKET_SIZE(Size)  (((Size) & 0x7ffU) << 3)
#define DWC3_DEPCFG_FIFO_NUMBER(Number)    (((Number) & 0x1fU) << 17)
#define DWC3_DEPCFG_ACTION_INIT            (0U << 30)
#define DWC3_DEPCFG_ACTION_MODIFY          (2U << 30)
#define DWC3_DEPCFG_XFER_COMPLETE_EN       BIT8
#define DWC3_DEPCFG_XFER_IN_PROGRESS_EN    BIT9
#define DWC3_DEPCFG_XFER_NOT_READY_EN      BIT10
#define DWC3_DEPCFG_BINTERVAL_M1(Value)    (((Value) & 0xffU) << 16)
#define DWC3_DEPCFG_EP_NUMBER(Number)      (((Number) & 0x1fU) << 25)
#define DWC3_DEPXFERCFG_NUM_XFER_RES(Number) ((Number) & 0xffffU)

#define DWC3_ENDPOINT_TYPE_CONTROL  0U
#define DWC3_ENDPOINT_TYPE_BULK     2U

#define DWC3_TRB_SIZE_MASK          0x00ffffffU
#define DWC3_TRB_CTRL_HWO           BIT0
#define DWC3_TRB_CTRL_LST           BIT1
#define DWC3_TRB_CTRL_CHN           BIT2
#define DWC3_TRB_CTRL_CSP           BIT3
#define DWC3_TRB_CTRL_TRBCTL(Type)  (((Type) & 0x3fU) << 4)
#define DWC3_TRB_CTRL_ISP_IMI       BIT10
#define DWC3_TRB_CTRL_IOC           BIT11

#define DWC3_TRBCTL_NORMAL          1U
#define DWC3_TRBCTL_CONTROL_SETUP   2U
#define DWC3_TRBCTL_CONTROL_STATUS2 3U
#define DWC3_TRBCTL_CONTROL_STATUS3 4U
#define DWC3_TRBCTL_CONTROL_DATA    5U

#define DWC3_PHYSICAL_EP(Number, DirectionIn)  \
  (((Number) << 1) | ((DirectionIn) ? 1U : 0U))
#define DWC3_EP0_OUT  DWC3_PHYSICAL_EP (0, FALSE)
#define DWC3_EP0_IN   DWC3_PHYSICAL_EP (0, TRUE)
#define DWC3_EP1_OUT  DWC3_PHYSICAL_EP (1, FALSE)
#define DWC3_EP1_IN   DWC3_PHYSICAL_EP (1, TRUE)
#define DWC3_EP_COUNT 4U

#define USB_MSC_GET_MAX_LUN  0xfeU
#define USB_MSC_RESET        0xffU

#define USB_REQUEST_TYPE_MASK  (3U << 5)

#define USB_DESC_TYPE_DEVICE_QUALIFIER  6U
#define USB_DESC_TYPE_OTHER_SPEED_CONFIG 7U

#pragma pack (1)
typedef struct {
  UINT32    BufferPtrLow;
  UINT32    BufferPtrHigh;
  UINT32    Size;
  UINT32    Control;
} DWC3_TRB;
#pragma pack ()

typedef enum {
  Dwc3TransferNone,
  Dwc3TransferSetup,
  Dwc3TransferControl,
  Dwc3TransferBulk
} DWC3_TRANSFER_KIND;

typedef struct {
  DWC3_TRB               *Trb;
  PHYSICAL_ADDRESS       TrbDeviceAddress;
  VOID                   *Mapping;
  UINTN                  Requested;
  UINTN                  CompletedBytes;
  EFI_STATUS             CompletionStatus;
  DWC3_TRANSFER_KIND     Kind;
  UINT8                  ResourceIndex;
  BOOLEAN                ResourceValid;
  BOOLEAN                Active;
  BOOLEAN                Complete;
  BOOLEAN                Stalled;
} DWC3_ENDPOINT;

struct _DWC3_DEVICE {
  UINT32                     Signature;
  EFI_PHYSICAL_ADDRESS       ControllerBase;
  VOID                       *CommonBuffer;
  PHYSICAL_ADDRESS           CommonDeviceAddress;
  VOID                       *CommonMapping;
  UINT8                      *EventBuffer;
  PHYSICAL_ADDRESS           EventDeviceAddress;
  UINTN                      EventOffset;
  DWC3_TRB                   *Trbs;
  PHYSICAL_ADDRESS           TrbsDeviceAddress;
  USB_DEVICE_REQUEST         *SetupPacket;
  PHYSICAL_ADDRESS           SetupDeviceAddress;
  UINT8                      *DummyBuffer;
  PHYSICAL_ADDRESS           DummyDeviceAddress;
  DWC3_ENDPOINT              Endpoints[DWC3_EP_COUNT];
  DWC3_MASS_STORAGE_RESET    Reset;
  VOID                       *ResetContext;
  DWC3_CANCEL_CHECK          Cancel;
  VOID                       *CancelContext;
  UINT8                      MaxLun;
  UINT16                     BulkMaxPacket;
  BOOLEAN                    Connected;
  BOOLEAN                    Configured;
  BOOLEAN                    BulkConfigured;
  BOOLEAN                    ControllerAccessible;
  BOOLEAN                    Started;
  BOOLEAN                    HandlingSetup;
  UINT8                      ConfigurationValue;
  UINT8                      ConfigDescriptor[32];
  UINT8                      OtherSpeedDescriptor[32];
};

STATIC CONST UINT8  mDeviceDescriptor[] = {
  18, USB_DESC_TYPE_DEVICE,
  0x00, 0x02,                       // USB 2.00
  0x00, 0x00, 0x00,                 // Class is specified by the interface
  64,                               // EP0 maximum packet size
  0x07, 0x22,                       // VID 0x2207 (Rockchip)
  0x10, 0x00,                       // PID 0x0010
  0x00, 0x01,                       // Device release 1.00
  1, 2, 3,                          // Manufacturer, product, serial strings
  1                                 // One configuration
};

STATIC CONST UINT8  mConfigDescriptorTemplate[] = {
  9, USB_DESC_TYPE_CONFIG, 32, 0, 1, 1, 0, 0x80, 50,
  9, USB_DESC_TYPE_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,
  7, USB_DESC_TYPE_ENDPOINT, 0x01, USB_ENDPOINT_BULK, 0x00, 0x02, 0,
  7, USB_DESC_TYPE_ENDPOINT, 0x81, USB_ENDPOINT_BULK, 0x00, 0x02, 0
};

STATIC CONST UINT8  mDeviceQualifierDescriptor[] = {
  10, USB_DESC_TYPE_DEVICE_QUALIFIER,
  0x00, 0x02, 0x00, 0x00, 0x00, 64, 1, 0
};

STATIC CONST UINT8  mStringLangId[] = {
  4, USB_DESC_TYPE_STRING, 0x09, 0x04
};

STATIC CONST UINT8  mStringManufacturer[] = {
  28, USB_DESC_TYPE_STRING,
  'R', 0, 'o', 0, 'c', 0, 'k', 0, 'c', 0, 'h', 0, 'i', 0, 'p', 0,
  ' ', 0, 'U', 0, 'E', 0, 'F', 0, 'I', 0
};

STATIC CONST UINT8  mStringProduct[] = {
  46, USB_DESC_TYPE_STRING,
  'L', 0, 'u', 0, 'b', 0, 'a', 0, 'n', 0, 'C', 0, 'a', 0, 't', 0,
  '4', 0, ' ', 0, 'M', 0, 'a', 0, 's', 0, 's', 0, ' ', 0, 'S', 0,
  't', 0, 'o', 0, 'r', 0, 'a', 0, 'g', 0, 'e', 0
};

STATIC CONST UINT8  mStringSerial[] = {
  14, USB_DESC_TYPE_STRING,
  'R', 0, 'K', 0, '3', 0, '5', 0, '8', 0, '8', 0
};

STATIC
EFI_STATUS
Dwc3ProcessEvents (
  IN DWC3_DEVICE  *Device
  );

STATIC
EFI_STATUS
Dwc3QueueSetup (
  IN DWC3_DEVICE  *Device
  );

STATIC
VOID
Dwc3AbortEndpoint (
  IN DWC3_DEVICE  *Device,
  IN UINT8        PhysicalEp,
  IN BOOLEAN      IssueCommand
  );

STATIC
UINT32
Dwc3Read32 (
  IN DWC3_DEVICE  *Device,
  IN UINTN        Offset
  )
{
  return MmioRead32 (DWC3_REG (Device->ControllerBase, Offset));
}

STATIC
VOID
Dwc3Write32 (
  IN DWC3_DEVICE  *Device,
  IN UINTN        Offset,
  IN UINT32       Value
  )
{
  MmioWrite32 (DWC3_REG (Device->ControllerBase, Offset), Value);
}

STATIC
EFI_STATUS
Dwc3WaitRegisterClear (
  IN DWC3_DEVICE  *Device,
  IN UINTN        Offset,
  IN UINT32       Mask,
  IN UINTN        TimeoutUs
  )
{
  while (TimeoutUs-- != 0) {
    if ((Dwc3Read32 (Device, Offset) & Mask) == 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
Dwc3WaitRegisterSet (
  IN DWC3_DEVICE  *Device,
  IN UINTN        Offset,
  IN UINT32       Mask,
  IN UINTN        TimeoutUs
  )
{
  while (TimeoutUs-- != 0) {
    if ((Dwc3Read32 (Device, Offset) & Mask) == Mask) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
Dwc3EndpointCommand (
  IN  DWC3_DEVICE  *Device,
  IN  UINT8        PhysicalEp,
  IN  UINT32       Command,
  IN  UINT32       Parameter0,
  IN  UINT32       Parameter1,
  IN  UINT32       Parameter2,
  OUT UINT8        *ResourceIndex OPTIONAL
  )
{
  UINT32      Result;
  EFI_STATUS  Status;

  if (PhysicalEp >= DWC3_EP_COUNT) {
    return EFI_INVALID_PARAMETER;
  }

  Dwc3Write32 (Device, DWC3_DEPCMDPAR2 (PhysicalEp), Parameter2);
  Dwc3Write32 (Device, DWC3_DEPCMDPAR1 (PhysicalEp), Parameter1);
  Dwc3Write32 (Device, DWC3_DEPCMDPAR0 (PhysicalEp), Parameter0);
  MemoryFence ();
  Dwc3Write32 (
    Device,
    DWC3_DEPCMD (PhysicalEp),
    Command | DWC3_DEPCMD_CMDACT
    );

  Status = Dwc3WaitRegisterClear (
             Device,
             DWC3_DEPCMD (PhysicalEp),
             DWC3_DEPCMD_CMDACT,
             10000
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "DWC3: endpoint %u command 0x%x timed out\n",
      PhysicalEp,
      Command
      ));
    return Status;
  }

  Result = Dwc3Read32 (Device, DWC3_DEPCMD (PhysicalEp));
  if ((Result & DWC3_DEPCMD_STATUS_MASK) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "DWC3: endpoint %u command 0x%x failed (result 0x%x)\n",
      PhysicalEp,
      Command,
      Result
      ));
    return EFI_DEVICE_ERROR;
  }

  if (ResourceIndex != NULL) {
    *ResourceIndex = (UINT8)DWC3_DEPCMD_GET_RSC_IDX (Result);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
Dwc3ConfigureEndpoint (
  IN DWC3_DEVICE  *Device,
  IN UINT8        PhysicalEp,
  IN UINT8        EndpointType,
  IN UINT16       MaxPacket,
  IN BOOLEAN      Modify
  )
{
  EFI_STATUS  Status;
  UINT32      Parameter0;
  UINT32      Parameter1;

  Parameter0 = DWC3_DEPCFG_EP_TYPE (EndpointType) |
               DWC3_DEPCFG_MAX_PACKET_SIZE (MaxPacket) |
               (Modify ? DWC3_DEPCFG_ACTION_MODIFY : DWC3_DEPCFG_ACTION_INIT);

  if ((PhysicalEp & 1U) != 0) {
    Parameter0 |= DWC3_DEPCFG_FIFO_NUMBER (PhysicalEp >> 1);
  }

  Parameter1 = DWC3_DEPCFG_XFER_COMPLETE_EN |
               DWC3_DEPCFG_XFER_NOT_READY_EN |
               DWC3_DEPCFG_EP_NUMBER (PhysicalEp);

  Status = Dwc3EndpointCommand (
             Device,
             PhysicalEp,
             DWC3_DEPCMD_SETEPCONFIG,
             Parameter0,
             Parameter1,
             0,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Dwc3EndpointCommand (
           Device,
           PhysicalEp,
           DWC3_DEPCMD_SETTRANSFRESOURCE,
           DWC3_DEPXFERCFG_NUM_XFER_RES (1),
           0,
           0,
           NULL
           );
}

STATIC
EFI_STATUS
Dwc3StartTransfer (
  IN DWC3_DEVICE           *Device,
  IN UINT8                 PhysicalEp,
  IN PHYSICAL_ADDRESS      DeviceAddress,
  IN VOID                  *Mapping OPTIONAL,
  IN UINTN                 Length,
  IN UINT8                 TrbControl,
  IN DWC3_TRANSFER_KIND    Kind
  )
{
  DWC3_ENDPOINT  *Endpoint;
  DWC3_TRB       *Trb;
  EFI_STATUS     Status;
  UINT8          ResourceIndex;

  if ((PhysicalEp >= DWC3_EP_COUNT) || (Length > DWC3_TRB_SIZE_MASK)) {
    return EFI_INVALID_PARAMETER;
  }

  Endpoint = &Device->Endpoints[PhysicalEp];
  if (Endpoint->Active) {
    return EFI_ALREADY_STARTED;
  }

  Trb = Endpoint->Trb;
  ZeroMem (Trb, sizeof (*Trb));
  Trb->BufferPtrLow  = (UINT32)DeviceAddress;
  Trb->BufferPtrHigh = (UINT32)(DeviceAddress >> 32);
  Trb->Size          = (UINT32)Length;
  Trb->Control       = DWC3_TRB_CTRL_HWO |
                       DWC3_TRB_CTRL_LST |
                       DWC3_TRB_CTRL_IOC |
                       DWC3_TRB_CTRL_TRBCTL (TrbControl);
  MemoryFence ();

  Endpoint->Mapping          = Mapping;
  Endpoint->Requested        = Length;
  Endpoint->CompletedBytes   = 0;
  Endpoint->CompletionStatus = EFI_NOT_READY;
  Endpoint->Kind             = Kind;
  Endpoint->ResourceValid    = FALSE;
  Endpoint->Active           = TRUE;
  Endpoint->Complete         = FALSE;

  Status = Dwc3EndpointCommand (
             Device,
             PhysicalEp,
             DWC3_DEPCMD_STARTTRANSFER,
             (UINT32)(Endpoint->TrbDeviceAddress >> 32),
             (UINT32)Endpoint->TrbDeviceAddress,
             0,
             &ResourceIndex
             );
  if (EFI_ERROR (Status)) {
    Endpoint->Active = FALSE;
    Endpoint->Kind   = Dwc3TransferNone;
    if (Mapping != NULL) {
      DmaUnmap (Mapping);
      Endpoint->Mapping = NULL;
    }

    return Status;
  }

  Endpoint->ResourceIndex = ResourceIndex;
  Endpoint->ResourceValid = TRUE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
Dwc3WaitTransfer (
  IN  DWC3_DEVICE  *Device,
  IN  UINT8        PhysicalEp,
  OUT UINTN        *Transferred OPTIONAL
  )
{
  DWC3_ENDPOINT  *Endpoint;
  EFI_STATUS     Status;

  Endpoint = &Device->Endpoints[PhysicalEp];

  while (Endpoint->Active && !Endpoint->Complete) {
    Status = Dwc3ProcessEvents (Device);
    if (EFI_ERROR (Status) && (Status != EFI_NOT_READY)) {
      Dwc3AbortEndpoint (Device, PhysicalEp, TRUE);
      return Status;
    }

    if ((Device->Cancel != NULL) && Device->Cancel (Device->CancelContext)) {
      Dwc3AbortEndpoint (Device, PhysicalEp, TRUE);
      return EFI_ABORTED;
    }

    MicroSecondDelay (50);
  }

  if (!Endpoint->Complete) {
    return EFI_ABORTED;
  }

  Status = Endpoint->CompletionStatus;
  if (Transferred != NULL) {
    *Transferred = Endpoint->CompletedBytes;
  }

  Endpoint->Complete = FALSE;
  Endpoint->Kind     = Dwc3TransferNone;
  return Status;
}

STATIC
VOID
Dwc3CompleteEndpoint (
  IN DWC3_DEVICE  *Device,
  IN UINT8        PhysicalEp,
  IN UINT32       EventStatus
  )
{
  DWC3_ENDPOINT  *Endpoint;
  UINT32         Remaining;

  Endpoint = &Device->Endpoints[PhysicalEp];
  if (!Endpoint->Active) {
    return;
  }

  MemoryFence ();
  Remaining = Endpoint->Trb->Size & DWC3_TRB_SIZE_MASK;
  if (Remaining > Endpoint->Requested) {
    Remaining = (UINT32)Endpoint->Requested;
  }

  Endpoint->CompletedBytes = Endpoint->Requested - Remaining;
  Endpoint->CompletionStatus = ((EventStatus & DWC3_DEPEVT_STATUS_BUSERR) != 0) ?
                               EFI_DEVICE_ERROR : EFI_SUCCESS;

  if (Endpoint->Mapping != NULL) {
    DmaUnmap (Endpoint->Mapping);
    Endpoint->Mapping = NULL;
  }

  Endpoint->ResourceValid = FALSE;
  Endpoint->Active        = FALSE;
  Endpoint->Complete      = TRUE;
}

STATIC
VOID
Dwc3AbortEndpoint (
  IN DWC3_DEVICE  *Device,
  IN UINT8        PhysicalEp,
  IN BOOLEAN      IssueCommand
  )
{
  DWC3_ENDPOINT  *Endpoint;

  Endpoint = &Device->Endpoints[PhysicalEp];
  if (!Endpoint->Active) {
    return;
  }

  if (IssueCommand && Endpoint->ResourceValid) {
    Dwc3EndpointCommand (
      Device,
      PhysicalEp,
      DWC3_DEPCMD_ENDTRANSFER |
      DWC3_DEPCMD_HIPRI_FORCERM |
      DWC3_DEPCMD_CMDIOC |
      DWC3_DEPCMD_PARAM (Endpoint->ResourceIndex),
      0,
      0,
      0,
      NULL
      );
  }

  if (Endpoint->Mapping != NULL) {
    DmaUnmap (Endpoint->Mapping);
    Endpoint->Mapping = NULL;
  }

  Endpoint->ResourceValid    = FALSE;
  Endpoint->Active           = FALSE;
  Endpoint->Complete         = TRUE;
  Endpoint->CompletedBytes   = 0;
  Endpoint->CompletionStatus = EFI_ABORTED;
}

STATIC
VOID
Dwc3AbortBulkTransfers (
  IN DWC3_DEVICE  *Device
  )
{
  Dwc3AbortEndpoint (Device, DWC3_EP1_OUT, TRUE);
  Dwc3AbortEndpoint (Device, DWC3_EP1_IN, TRUE);
}

STATIC
EFI_STATUS
Dwc3SetEndpointStall (
  IN DWC3_DEVICE  *Device,
  IN UINT8        PhysicalEp,
  IN BOOLEAN      Stall
  )
{
  EFI_STATUS  Status;

  Status = Dwc3EndpointCommand (
             Device,
             PhysicalEp,
             Stall ? DWC3_DEPCMD_SETSTALL : DWC3_DEPCMD_CLEARSTALL,
             0,
             0,
             0,
             NULL
             );
  if (!EFI_ERROR (Status)) {
    Device->Endpoints[PhysicalEp].Stalled = Stall;
  }

  return Status;
}

STATIC
BOOLEAN
Dwc3ResolveEndpoint (
  IN  DWC3_DEVICE  *Device,
  IN  UINT16       EndpointIndex,
  IN  BOOLEAN      RequireBulk,
  OUT UINT8        *PhysicalEp
  )
{
  UINT8  EndpointAddress;
  UINT8  EndpointNumber;

  EndpointAddress = (UINT8)EndpointIndex;
  EndpointNumber  = EndpointAddress & 0x0fU;

  // Bits 15:8 and 6:4 of wIndex are reserved for endpoint requests.
  if (((EndpointIndex & 0xff70U) != 0) ||
      (EndpointNumber > 1) ||
      (RequireBulk && (EndpointNumber != 1)) ||
      ((EndpointNumber == 1) && !Device->BulkConfigured))
  {
    return FALSE;
  }

  *PhysicalEp = DWC3_PHYSICAL_EP (
                  EndpointNumber,
                  (EndpointAddress & USB_ENDPOINT_DIR_IN) != 0
                  );
  return TRUE;
}

STATIC
EFI_STATUS
Dwc3MapAndTransfer (
  IN  DWC3_DEVICE  *Device,
  IN  UINT8        PhysicalEp,
  IN  VOID         *Buffer,
  IN  UINTN        Length,
  IN  UINT8        TrbControl,
  IN  DWC3_TRANSFER_KIND Kind,
  OUT UINTN        *Transferred OPTIONAL
  )
{
  DMA_MAP_OPERATION  Operation;
  PHYSICAL_ADDRESS   DeviceAddress;
  VOID               *Mapping;
  UINTN              MapLength;
  EFI_STATUS         Status;

  if ((Length != 0) && (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Length == 0) {
    Status = Dwc3StartTransfer (
               Device,
               PhysicalEp,
               Device->DummyDeviceAddress,
               NULL,
               0,
               TrbControl,
               Kind
               );
  } else {
    Operation = ((PhysicalEp & 1U) != 0) ?
                MapOperationBusMasterRead : MapOperationBusMasterWrite;
    MapLength = Length;
    Mapping   = NULL;
    Status    = DmaMap (
                  Operation,
                  Buffer,
                  &MapLength,
                  &DeviceAddress,
                  &Mapping
                  );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (MapLength != Length) {
      DmaUnmap (Mapping);
      return EFI_BAD_BUFFER_SIZE;
    }

    Status = Dwc3StartTransfer (
               Device,
               PhysicalEp,
               DeviceAddress,
               Mapping,
               Length,
               TrbControl,
               Kind
               );
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Dwc3WaitTransfer (Device, PhysicalEp, Transferred);
}

STATIC
EFI_STATUS
Dwc3ControlDataIn (
  IN DWC3_DEVICE  *Device,
  IN CONST VOID   *Buffer,
  IN UINTN        BufferSize,
  IN UINT16       RequestedSize
  )
{
  EFI_STATUS  Status;
  UINTN       Length;
  UINTN       Transferred;

  Length = MIN (BufferSize, RequestedSize);
  Status = Dwc3MapAndTransfer (
             Device,
             DWC3_EP0_IN,
             (VOID *)Buffer,
             Length,
             DWC3_TRBCTL_CONTROL_DATA,
             Dwc3TransferControl,
             &Transferred
             );
  if (EFI_ERROR (Status) || (Transferred != Length)) {
    return EFI_DEVICE_ERROR;
  }

  // A device-to-host control request has an OUT status phase.
  return Dwc3MapAndTransfer (
           Device,
           DWC3_EP0_OUT,
           NULL,
           0,
           DWC3_TRBCTL_CONTROL_STATUS3,
           Dwc3TransferControl,
           NULL
           );
}

STATIC
EFI_STATUS
Dwc3ControlStatusIn (
  IN DWC3_DEVICE  *Device
  )
{
  return Dwc3MapAndTransfer (
           Device,
           DWC3_EP0_IN,
           NULL,
           0,
           DWC3_TRBCTL_CONTROL_STATUS2,
           Dwc3TransferControl,
           NULL
           );
}

STATIC
VOID
Dwc3UpdateEndpointDescriptors (
  IN DWC3_DEVICE  *Device
  )
{
  UINT16  OtherMaxPacket;

  Device->ConfigDescriptor[22] = (UINT8)Device->BulkMaxPacket;
  Device->ConfigDescriptor[23] = (UINT8)(Device->BulkMaxPacket >> 8);
  Device->ConfigDescriptor[29] = (UINT8)Device->BulkMaxPacket;
  Device->ConfigDescriptor[30] = (UINT8)(Device->BulkMaxPacket >> 8);

  OtherMaxPacket = (Device->BulkMaxPacket == 512) ? 64 : 512;
  CopyMem (
    Device->OtherSpeedDescriptor,
    mConfigDescriptorTemplate,
    sizeof (Device->OtherSpeedDescriptor)
    );
  Device->OtherSpeedDescriptor[1]  = USB_DESC_TYPE_OTHER_SPEED_CONFIG;
  Device->OtherSpeedDescriptor[22] = (UINT8)OtherMaxPacket;
  Device->OtherSpeedDescriptor[23] = (UINT8)(OtherMaxPacket >> 8);
  Device->OtherSpeedDescriptor[29] = (UINT8)OtherMaxPacket;
  Device->OtherSpeedDescriptor[30] = (UINT8)(OtherMaxPacket >> 8);
}

STATIC
EFI_STATUS
Dwc3ConfigureBulkEndpoints (
  IN DWC3_DEVICE  *Device
  )
{
  EFI_STATUS  Status;
  UINT32      Enabled;

  if (Device->BulkConfigured) {
    Enabled = Dwc3Read32 (Device, DWC3_DALEPENA);
    Enabled |= BIT2 | BIT3;
    Dwc3Write32 (Device, DWC3_DALEPENA, Enabled);
    return EFI_SUCCESS;
  }

  // Start a new endpoint configuration at the first non-control endpoint.
  // The command itself is always issued to physical EP0 OUT.  XferRscIdx 2
  // tells DWC3 that the new configuration begins after the EP0 resources.
  Status = Dwc3EndpointCommand (
             Device,
             DWC3_EP0_OUT,
             DWC3_DEPCMD_DEPSTARTCFG | DWC3_DEPCMD_PARAM (2),
             0,
             0,
             0,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Dwc3ConfigureEndpoint (
             Device,
             DWC3_EP1_OUT,
             DWC3_ENDPOINT_TYPE_BULK,
             Device->BulkMaxPacket,
             FALSE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Dwc3ConfigureEndpoint (
             Device,
             DWC3_EP1_IN,
             DWC3_ENDPOINT_TYPE_BULK,
             Device->BulkMaxPacket,
             FALSE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Enabled = Dwc3Read32 (Device, DWC3_DALEPENA);
  Enabled |= BIT2 | BIT3;
  Dwc3Write32 (Device, DWC3_DALEPENA, Enabled);
  Device->BulkConfigured = TRUE;
  return EFI_SUCCESS;
}

STATIC
VOID
Dwc3DisableBulkEndpoints (
  IN DWC3_DEVICE  *Device,
  IN BOOLEAN      IssueCommand
  )
{
  UINT32  Enabled;

  Dwc3AbortEndpoint (Device, DWC3_EP1_OUT, IssueCommand);
  Dwc3AbortEndpoint (Device, DWC3_EP1_IN, IssueCommand);

  if (IssueCommand) {
    if (Device->Endpoints[DWC3_EP1_OUT].Stalled) {
      Dwc3SetEndpointStall (Device, DWC3_EP1_OUT, FALSE);
    }

    if (Device->Endpoints[DWC3_EP1_IN].Stalled) {
      Dwc3SetEndpointStall (Device, DWC3_EP1_IN, FALSE);
    }
  }

  Device->Endpoints[DWC3_EP1_OUT].Stalled = FALSE;
  Device->Endpoints[DWC3_EP1_IN].Stalled  = FALSE;

  Enabled = Dwc3Read32 (Device, DWC3_DALEPENA);
  Enabled &= ~(BIT2 | BIT3);
  Dwc3Write32 (Device, DWC3_DALEPENA, Enabled);
  Device->Configured        = FALSE;
  Device->ConfigurationValue = 0;
  Device->BulkConfigured    = FALSE;
}

STATIC
EFI_STATUS
Dwc3GetDescriptor (
  IN DWC3_DEVICE        *Device,
  IN USB_DEVICE_REQUEST *Request
  )
{
  CONST VOID  *Descriptor;
  UINTN       DescriptorSize;
  UINT8       DescriptorType;
  UINT8       DescriptorIndex;

  DescriptorType  = (UINT8)(Request->Value >> 8);
  DescriptorIndex = (UINT8)Request->Value;
  Descriptor      = NULL;
  DescriptorSize  = 0;

  if (Request->RequestType != USB_DEV_GET_DESCRIPTOR_REQ_TYPE) {
    return EFI_UNSUPPORTED;
  }

  switch (DescriptorType) {
    case USB_DESC_TYPE_DEVICE:
      if ((DescriptorIndex != 0) || (Request->Index != 0)) {
        return EFI_UNSUPPORTED;
      }

      Descriptor     = mDeviceDescriptor;
      DescriptorSize = sizeof (mDeviceDescriptor);
      break;

    case USB_DESC_TYPE_CONFIG:
      if ((DescriptorIndex != 0) || (Request->Index != 0)) {
        return EFI_UNSUPPORTED;
      }

      Descriptor     = Device->ConfigDescriptor;
      DescriptorSize = sizeof (Device->ConfigDescriptor);
      break;

    case USB_DESC_TYPE_DEVICE_QUALIFIER:
      if ((DescriptorIndex != 0) || (Request->Index != 0)) {
        return EFI_UNSUPPORTED;
      }

      Descriptor     = mDeviceQualifierDescriptor;
      DescriptorSize = sizeof (mDeviceQualifierDescriptor);
      break;

    case USB_DESC_TYPE_OTHER_SPEED_CONFIG:
      if ((DescriptorIndex != 0) || (Request->Index != 0)) {
        return EFI_UNSUPPORTED;
      }

      Descriptor     = Device->OtherSpeedDescriptor;
      DescriptorSize = sizeof (Device->OtherSpeedDescriptor);
      break;

    case USB_DESC_TYPE_STRING:
      if (((DescriptorIndex == 0) && (Request->Index != 0)) ||
          ((DescriptorIndex != 0) &&
           (Request->Index != 0) &&
           (Request->Index != 0x0409)))
      {
        return EFI_UNSUPPORTED;
      }

      switch (DescriptorIndex) {
        case 0:
          Descriptor     = mStringLangId;
          DescriptorSize = sizeof (mStringLangId);
          break;
        case 1:
          Descriptor     = mStringManufacturer;
          DescriptorSize = sizeof (mStringManufacturer);
          break;
        case 2:
          Descriptor     = mStringProduct;
          DescriptorSize = sizeof (mStringProduct);
          break;
        case 3:
          Descriptor     = mStringSerial;
          DescriptorSize = sizeof (mStringSerial);
          break;
        default:
          return EFI_UNSUPPORTED;
      }

      break;

    default:
      return EFI_UNSUPPORTED;
  }

  return Dwc3ControlDataIn (
           Device,
           Descriptor,
           DescriptorSize,
           Request->Length
           );
}

STATIC
EFI_STATUS
Dwc3HandleStandardRequest (
  IN DWC3_DEVICE         *Device,
  IN USB_DEVICE_REQUEST  *Request
  )
{
  UINT8       ByteResponse;
  UINT16      WordResponse;
  UINT8       PhysicalEp;
  EFI_STATUS  Status;
  BOOLEAN     ResetTransport;

  switch (Request->Request) {
    case USB_REQ_GET_STATUS:
      if ((Request->Value != 0) || (Request->Length != sizeof (WordResponse))) {
        return EFI_UNSUPPORTED;
      }

      WordResponse = 0;
      switch (Request->RequestType) {
        case USB_DEV_GET_STATUS_REQ_TYPE_D:
          if (Request->Index != 0) {
            return EFI_UNSUPPORTED;
          }

          break;

        case USB_DEV_GET_STATUS_REQ_TYPE_I:
          if ((Request->Index != 0) || !Device->Configured) {
            return EFI_UNSUPPORTED;
          }

          break;

        case USB_DEV_GET_STATUS_REQ_TYPE_E:
          if (!Dwc3ResolveEndpoint (
                 Device,
                 Request->Index,
                 FALSE,
                 &PhysicalEp
                 ))
          {
            return EFI_UNSUPPORTED;
          }

          if (Device->Endpoints[PhysicalEp].Stalled) {
            WordResponse = BIT0;
          }

          break;

        default:
          return EFI_UNSUPPORTED;
      }

      return Dwc3ControlDataIn (
               Device,
               &WordResponse,
               sizeof (WordResponse),
               Request->Length
               );

    case USB_REQ_CLEAR_FEATURE:
      if ((Request->RequestType == USB_DEV_CLEAR_FEATURE_REQ_TYPE_E) &&
          (Request->Value == USB_FEATURE_ENDPOINT_HALT) &&
          (Request->Length == 0) &&
          Dwc3ResolveEndpoint (
            Device,
            Request->Index,
            TRUE,
            &PhysicalEp
            ))
      {
        Status = Dwc3SetEndpointStall (Device, PhysicalEp, FALSE);
        if (EFI_ERROR (Status)) {
          return Status;
        }

        return Dwc3ControlStatusIn (Device);
      }

      return EFI_UNSUPPORTED;

    case USB_REQ_SET_FEATURE:
      if ((Request->RequestType == USB_DEV_SET_FEATURE_REQ_TYPE_E) &&
          (Request->Value == USB_FEATURE_ENDPOINT_HALT) &&
          (Request->Length == 0) &&
          Dwc3ResolveEndpoint (
            Device,
            Request->Index,
            TRUE,
            &PhysicalEp
            ))
      {
        Status = Dwc3SetEndpointStall (Device, PhysicalEp, TRUE);
        if (EFI_ERROR (Status)) {
          return Status;
        }

        return Dwc3ControlStatusIn (Device);
      }

      return EFI_UNSUPPORTED;

    case USB_REQ_SET_ADDRESS:
      if ((Request->RequestType != USB_DEV_SET_ADDRESS_REQ_TYPE) ||
          (Request->Value > 127) ||
          (Request->Index != 0) ||
          (Request->Length != 0))
      {
        return EFI_UNSUPPORTED;
      }

      Dwc3Write32 (
        Device,
        DWC3_DCFG,
        (Dwc3Read32 (Device, DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK) |
        DWC3_DCFG_DEVADDR (Request->Value)
        );
      return Dwc3ControlStatusIn (Device);

    case USB_REQ_GET_DESCRIPTOR:
      return Dwc3GetDescriptor (Device, Request);

    case USB_REQ_GET_CONFIG:
      if ((Request->RequestType != USB_DEV_GET_CONFIGURATION_REQ_TYPE) ||
          (Request->Value != 0) ||
          (Request->Index != 0) ||
          (Request->Length != sizeof (ByteResponse)))
      {
        return EFI_UNSUPPORTED;
      }

      ByteResponse = Device->ConfigurationValue;
      return Dwc3ControlDataIn (
               Device,
               &ByteResponse,
               sizeof (ByteResponse),
               Request->Length
               );

    case USB_REQ_SET_CONFIG:
      if ((Request->RequestType != USB_DEV_SET_CONFIGURATION_REQ_TYPE) ||
          ((Request->Value != 0) && (Request->Value != 1)) ||
          (Request->Index != 0) ||
          (Request->Length != 0))
      {
        return EFI_UNSUPPORTED;
      }

      ResetTransport = Device->Configured || Device->BulkConfigured;
      if (ResetTransport && (Device->Reset != NULL)) {
        Device->Reset (Device->ResetContext);
      }

      if (Request->Value == 0) {
        Dwc3DisableBulkEndpoints (Device, TRUE);
      } else {
        // SetConfiguration resets endpoint halt and data-toggle state, even
        // when the host selects the currently active configuration again.
        if (Device->BulkConfigured) {
          Dwc3DisableBulkEndpoints (Device, TRUE);
        }

        Status = Dwc3ConfigureBulkEndpoints (Device);
        if (EFI_ERROR (Status)) {
          return Status;
        }

        Device->ConfigurationValue = 1;
        Device->Configured         = TRUE;
      }

      return Dwc3ControlStatusIn (Device);

    case USB_REQ_GET_INTERFACE:
      if ((Request->RequestType != USB_DEV_GET_INTERFACE_REQ_TYPE) ||
          (Request->Value != 0) ||
          (Request->Index != 0) ||
          (Request->Length != sizeof (ByteResponse)) ||
          !Device->Configured)
      {
        return EFI_UNSUPPORTED;
      }

      ByteResponse = 0;
      return Dwc3ControlDataIn (
               Device,
               &ByteResponse,
               sizeof (ByteResponse),
               Request->Length
               );

    case USB_REQ_SET_INTERFACE:
      if ((Request->RequestType != USB_DEV_SET_INTERFACE_REQ_TYPE) ||
          (Request->Value != 0) ||
          (Request->Index != 0) ||
          (Request->Length != 0) ||
          !Device->Configured)
      {
        return EFI_UNSUPPORTED;
      }

      // The sole alternate setting has the same endpoints, but selecting it
      // still resets their data toggles and halt state.
      Dwc3AbortBulkTransfers (Device);
      if (Device->Reset != NULL) {
        Device->Reset (Device->ResetContext);
      }

      Status = Dwc3SetEndpointStall (Device, DWC3_EP1_OUT, FALSE);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      Status = Dwc3SetEndpointStall (Device, DWC3_EP1_IN, FALSE);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      return Dwc3ControlStatusIn (Device);

    default:
      return EFI_UNSUPPORTED;
  }
}

STATIC
EFI_STATUS
Dwc3HandleClassRequest (
  IN DWC3_DEVICE         *Device,
  IN USB_DEVICE_REQUEST  *Request
  )
{
  if (!Device->Configured) {
    return EFI_UNSUPPORTED;
  }

  switch (Request->Request) {
    case USB_MSC_GET_MAX_LUN:
      if ((Request->RequestType != 0xa1) ||
          (Request->Value != 0) ||
          (Request->Index != 0) ||
          (Request->Length != 1))
      {
        return EFI_UNSUPPORTED;
      }

      return Dwc3ControlDataIn (
               Device,
               &Device->MaxLun,
               sizeof (Device->MaxLun),
               Request->Length
               );

    case USB_MSC_RESET:
      if ((Request->RequestType != 0x21) ||
          (Request->Value != 0) ||
          (Request->Index != 0) ||
          (Request->Length != 0))
      {
        return EFI_UNSUPPORTED;
      }

      // BOT reset aborts active transfers but deliberately preserves endpoint
      // halt and data-toggle state.  The host resets both toggles with the two
      // mandatory CLEAR_FEATURE(ENDPOINT_HALT) requests that follow.
      Dwc3AbortBulkTransfers (Device);
      if (Device->Reset != NULL) {
        Device->Reset (Device->ResetContext);
      }

      return Dwc3ControlStatusIn (Device);

    default:
      return EFI_UNSUPPORTED;
  }
}

STATIC
EFI_STATUS
Dwc3HandleSetupPacket (
  IN DWC3_DEVICE  *Device
  )
{
  USB_DEVICE_REQUEST  Request;
  EFI_STATUS          Status;

  CopyMem (&Request, Device->SetupPacket, sizeof (Request));
  DEBUG ((
    DEBUG_VERBOSE,
    "DWC3 SETUP: type=%02x request=%02x value=%04x index=%04x length=%u\n",
    Request.RequestType,
    Request.Request,
    Request.Value,
    Request.Index,
    Request.Length
    ));

  Device->HandlingSetup = TRUE;
  if ((Request.RequestType & USB_REQUEST_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
    Status = Dwc3HandleStandardRequest (Device, &Request);
  } else if ((Request.RequestType & USB_REQUEST_TYPE_MASK) == USB_REQ_TYPE_CLASS) {
    Status = Dwc3HandleClassRequest (Device, &Request);
  } else {
    Status = EFI_UNSUPPORTED;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "DWC3: unsupported/failed setup request, status %r\n",
      Status
      ));
    // DWC3 represents a control-pipe stall through physical EP0 OUT.
    Dwc3SetEndpointStall (Device, DWC3_EP0_OUT, TRUE);
  }

  Device->HandlingSetup = FALSE;
  return Dwc3QueueSetup (Device);
}

STATIC
EFI_STATUS
Dwc3QueueSetup (
  IN DWC3_DEVICE  *Device
  )
{
  DWC3_ENDPOINT  *Endpoint;

  Endpoint = &Device->Endpoints[DWC3_EP0_OUT];
  if (Endpoint->Active) {
    Dwc3AbortEndpoint (Device, DWC3_EP0_OUT, TRUE);
  }

  ZeroMem (Device->SetupPacket, sizeof (*Device->SetupPacket));
  return Dwc3StartTransfer (
           Device,
           DWC3_EP0_OUT,
           Device->SetupDeviceAddress,
           NULL,
           sizeof (*Device->SetupPacket),
           DWC3_TRBCTL_CONTROL_SETUP,
           Dwc3TransferSetup
           );
}

STATIC
VOID
Dwc3HandleBusReset (
  IN DWC3_DEVICE  *Device
  )
{
  UINT32  Config;
  UINTN   Index;

  DEBUG ((DEBUG_INFO, "DWC3: USB bus reset\n"));

  Dwc3AbortEndpoint (Device, DWC3_EP0_OUT, FALSE);
  Dwc3AbortEndpoint (Device, DWC3_EP0_IN, FALSE);
  Dwc3AbortEndpoint (Device, DWC3_EP1_OUT, FALSE);
  Dwc3AbortEndpoint (Device, DWC3_EP1_IN, FALSE);

  for (Index = 0; Index < DWC3_EP_COUNT; Index++) {
    // USB reset clears every endpoint halt and data toggle in hardware.
    Device->Endpoints[Index].Stalled = FALSE;
  }

  Config = Dwc3Read32 (Device, DWC3_DCFG);
  Config &= ~DWC3_DCFG_DEVADDR_MASK;
  Dwc3Write32 (Device, DWC3_DCFG, Config);
  Dwc3Write32 (Device, DWC3_DALEPENA, BIT0 | BIT1);

  Device->Connected          = TRUE;
  Device->Configured         = FALSE;
  Device->BulkConfigured     = FALSE;
  Device->ConfigurationValue = 0;

  if (Device->Reset != NULL) {
    Device->Reset (Device->ResetContext);
  }

  // If reset interrupted a synchronous EP0 data/status transfer, do not
  // overwrite that endpoint's aborted completion with a new SETUP transfer.
  // Dwc3HandleSetupPacket() will queue SETUP from its common exit path after
  // the interrupted waiter has observed EFI_ABORTED.  An idle control pipe
  // still has to be armed here immediately.
  if (!Device->HandlingSetup) {
    Dwc3QueueSetup (Device);
  }
}

STATIC
VOID
Dwc3HandleConnectDone (
  IN DWC3_DEVICE  *Device
  )
{
  UINT32  Speed;

  Speed = Dwc3Read32 (Device, DWC3_DSTS) & DWC3_DSTS_CONNECTSPD_MASK;
  Device->BulkMaxPacket = (Speed == DWC3_DSTS_CONNECTSPD_HS) ? 512 : 64;
  Dwc3UpdateEndpointDescriptors (Device);
  Device->Connected = TRUE;

  DEBUG ((
    DEBUG_INFO,
    "DWC3: USB connection complete (%a speed)\n",
    (Device->BulkMaxPacket == 512) ? "high" : "full"
    ));
}

STATIC
EFI_STATUS
Dwc3ProcessEndpointEvent (
  IN DWC3_DEVICE  *Device,
  IN UINT32       Event
  )
{
  UINT8           PhysicalEp;
  UINT8           EventType;
  UINT8           EventStatus;
  DWC3_ENDPOINT   *Endpoint;
  DWC3_TRANSFER_KIND Kind;

  PhysicalEp = (UINT8)((Event >> 1) & 0x1fU);
  EventType   = (UINT8)((Event >> 6) & 0x0fU);
  EventStatus = (UINT8)((Event >> 12) & 0x0fU);

  if (PhysicalEp >= DWC3_EP_COUNT) {
    return EFI_SUCCESS;
  }

  switch (EventType) {
    case DWC3_DEPEVT_XFERCOMPLETE:
      Endpoint = &Device->Endpoints[PhysicalEp];
      Kind     = Endpoint->Kind;
      Dwc3CompleteEndpoint (Device, PhysicalEp, EventStatus);

      if ((PhysicalEp == DWC3_EP0_OUT) &&
          (Kind == Dwc3TransferSetup) &&
          (Endpoint->CompletedBytes == sizeof (USB_DEVICE_REQUEST)))
      {
        // A new SETUP packet automatically recovers the default control pipe
        // from a protocol stall in hardware.
        Device->Endpoints[DWC3_EP0_OUT].Stalled = FALSE;
        Device->Endpoints[DWC3_EP0_IN].Stalled  = FALSE;
        Endpoint->Complete = FALSE;
        Endpoint->Kind     = Dwc3TransferNone;
        return Dwc3HandleSetupPacket (Device);
      }

      break;

    case DWC3_DEPEVT_XFERINPROGRESS:
    case DWC3_DEPEVT_XFERNOTREADY:
    case DWC3_DEPEVT_EPCMDCMPLT:
      break;

    default:
      DEBUG ((
        DEBUG_VERBOSE,
        "DWC3: endpoint %u event %u status %u\n",
        PhysicalEp,
        EventType,
        EventStatus
        ));
      break;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
Dwc3ProcessDeviceEvent (
  IN DWC3_DEVICE  *Device,
  IN UINT32       Event
  )
{
  UINT8  EventType;

  EventType = (UINT8)((Event >> 8) & 0x0fU);
  switch (EventType) {
    case DWC3_DEVICE_EVENT_DISCONNECT:
      DEBUG ((DEBUG_INFO, "DWC3: USB disconnected\n"));
      Device->Connected = FALSE;
      // A disconnect does not guarantee a completion event for an active
      // control data/status TRB.  Abort EP0 explicitly so a nested control
      // waiter cannot remain blocked until the next cable connection.
      Dwc3AbortEndpoint (Device, DWC3_EP0_OUT, FALSE);
      Dwc3AbortEndpoint (Device, DWC3_EP0_IN, FALSE);
      Dwc3DisableBulkEndpoints (Device, FALSE);
      if (Device->Reset != NULL) {
        Device->Reset (Device->ResetContext);
      }

      break;

    case DWC3_DEVICE_EVENT_RESET:
      Dwc3HandleBusReset (Device);
      break;

    case DWC3_DEVICE_EVENT_CONNECT_DONE:
      Dwc3HandleConnectDone (Device);
      break;

    case DWC3_DEVICE_EVENT_LINK_STATUS:
      break;

    case DWC3_DEVICE_EVENT_ERRATIC_ERROR:
      DEBUG ((DEBUG_ERROR, "DWC3: erratic USB bus error\n"));
      return EFI_DEVICE_ERROR;

    case DWC3_DEVICE_EVENT_BUFFER_OVERFLOW:
      DEBUG ((DEBUG_ERROR, "DWC3: event buffer overflow\n"));
      return EFI_DEVICE_ERROR;

    default:
      DEBUG ((DEBUG_VERBOSE, "DWC3: device event %u\n", EventType));
      break;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
Dwc3ProcessEvents (
  IN DWC3_DEVICE  *Device
  )
{
  UINT32      EventCount;
  UINT32      Event;
  EFI_STATUS  Status;
  EFI_STATUS  OverallStatus;
  BOOLEAN     Processed;

  OverallStatus = EFI_SUCCESS;
  Processed     = FALSE;
  while (TRUE) {
    // A SETUP event can synchronously process later events while completing
    // its data and status phases.  Re-read the hardware count for every event
    // instead of carrying a stale local count across that nested processing.
    EventCount = Dwc3Read32 (Device, DWC3_GEVNTCOUNT0) &
                 DWC3_EVENT_COUNT_MASK;
    if (EventCount < sizeof (UINT32)) {
      break;
    }

    MemoryFence ();
    CopyMem (
      &Event,
      Device->EventBuffer + Device->EventOffset,
      sizeof (Event)
      );

    Device->EventOffset += sizeof (Event);
    if (Device->EventOffset >= DWC3_EVENT_BUFFER_SIZE) {
      Device->EventOffset = 0;
    }

    Dwc3Write32 (Device, DWC3_GEVNTCOUNT0, sizeof (Event));
    Processed = TRUE;

    if ((Event & BIT0) == 0) {
      Status = Dwc3ProcessEndpointEvent (Device, Event);
    } else {
      Status = Dwc3ProcessDeviceEvent (Device, Event);
    }

    if (EFI_ERROR (Status)) {
      OverallStatus = Status;
    }
  }

  return Processed ? OverallStatus : EFI_NOT_READY;
}

STATIC
EFI_STATUS
Dwc3InitializeCommonBuffer (
  IN DWC3_DEVICE  *Device
  )
{
  EFI_STATUS  Status;
  UINTN       MapSize;
  UINTN       Index;
  UINTN       Offset;

  Status = DmaAllocateAlignedBuffer (
             EfiBootServicesData,
             EFI_SIZE_TO_PAGES (DWC3_COMMON_BUFFER_SIZE),
             64,
             &Device->CommonBuffer
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MapSize = DWC3_COMMON_BUFFER_SIZE;
  Status  = DmaMap (
              MapOperationBusMasterCommonBuffer,
              Device->CommonBuffer,
              &MapSize,
              &Device->CommonDeviceAddress,
              &Device->CommonMapping
              );
  if (EFI_ERROR (Status) || (MapSize != DWC3_COMMON_BUFFER_SIZE)) {
    if (!EFI_ERROR (Status)) {
      DmaUnmap (Device->CommonMapping);
      Status = EFI_BAD_BUFFER_SIZE;
    }

    DmaFreeBuffer (
      EFI_SIZE_TO_PAGES (DWC3_COMMON_BUFFER_SIZE),
      Device->CommonBuffer
      );
    Device->CommonBuffer = NULL;
    return Status;
  }

  ZeroMem (Device->CommonBuffer, DWC3_COMMON_BUFFER_SIZE);

  Offset                     = 0;
  Device->EventBuffer        = (UINT8 *)Device->CommonBuffer + Offset;
  Device->EventDeviceAddress = Device->CommonDeviceAddress + Offset;
  Offset                    += DWC3_EVENT_BUFFER_SIZE;

  Offset = ALIGN_VALUE (Offset, 16);
  Device->Trbs              = (DWC3_TRB *)((UINT8 *)Device->CommonBuffer + Offset);
  Device->TrbsDeviceAddress = Device->CommonDeviceAddress + Offset;
  Offset                   += sizeof (DWC3_TRB) * DWC3_EP_COUNT;

  Offset = ALIGN_VALUE (Offset, 8);
  Device->SetupPacket        = (USB_DEVICE_REQUEST *)((UINT8 *)Device->CommonBuffer + Offset);
  Device->SetupDeviceAddress = Device->CommonDeviceAddress + Offset;
  Offset                    += sizeof (USB_DEVICE_REQUEST);

  Offset = ALIGN_VALUE (Offset, 8);
  Device->DummyBuffer        = (UINT8 *)Device->CommonBuffer + Offset;
  Device->DummyDeviceAddress = Device->CommonDeviceAddress + Offset;

  for (Index = 0; Index < DWC3_EP_COUNT; Index++) {
    Device->Endpoints[Index].Trb = &Device->Trbs[Index];
    Device->Endpoints[Index].TrbDeviceAddress =
      Device->TrbsDeviceAddress + (Index * sizeof (DWC3_TRB));
  }

  return EFI_SUCCESS;
}

STATIC
VOID
Dwc3FreeCommonBuffer (
  IN DWC3_DEVICE  *Device
  )
{
  if (Device->CommonMapping != NULL) {
    DmaUnmap (Device->CommonMapping);
    Device->CommonMapping = NULL;
  }

  if (Device->CommonBuffer != NULL) {
    DmaFreeBuffer (
      EFI_SIZE_TO_PAGES (DWC3_COMMON_BUFFER_SIZE),
      Device->CommonBuffer
      );
    Device->CommonBuffer = NULL;
  }
}

STATIC
VOID
Dwc3CoreSoftReset (
  IN DWC3_DEVICE  *Device
  )
{
  MmioOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_GCTL),
    DWC3_GCTL_CORESOFTRESET
    );
  MmioOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_GUSB3PIPECTL0),
    DWC3_GUSB3PIPECTL_PHYSOFTRST
    );
  MmioOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_GUSB2PHYCFG0),
    DWC3_GUSB2PHYCFG_PHYSOFTRST
    );
  MicroSecondDelay (100);

  MmioAnd32 (
    DWC3_REG (Device->ControllerBase, DWC3_GUSB3PIPECTL0),
    ~DWC3_GUSB3PIPECTL_PHYSOFTRST
    );
  MmioAnd32 (
    DWC3_REG (Device->ControllerBase, DWC3_GUSB2PHYCFG0),
    ~DWC3_GUSB2PHYCFG_PHYSOFTRST
    );
  MicroSecondDelay (100);

  MmioAnd32 (
    DWC3_REG (Device->ControllerBase, DWC3_GCTL),
    ~DWC3_GCTL_CORESOFTRESET
    );
}

STATIC
EFI_STATUS
Dwc3InitializeController (
  IN DWC3_DEVICE  *Device
  )
{
  UINT32      Register;
  UINT32      Revision;
  EFI_STATUS  Status;

  Revision = Dwc3Read32 (Device, DWC3_GSNPSID);
  if ((Revision & DWC3_GSNPSID_MASK) != DWC3_SYNOPSYS_ID) {
    DEBUG ((DEBUG_ERROR, "DWC3: invalid core ID 0x%08x\n", Revision));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "DWC3: core ID 0x%08x at 0x%lx\n", Revision, Device->ControllerBase));
  Device->ControllerAccessible = TRUE;

  // Stop the xHCI/device state machine before changing the port capability.
  MmioAnd32 (
    DWC3_REG (Device->ControllerBase, DWC3_DCTL),
    ~DWC3_DCTL_RUN_STOP
    );
  MicroSecondDelay (1000);

  Dwc3CoreSoftReset (Device);

  Register  = Dwc3Read32 (Device, DWC3_GCTL);
  Register &= ~(DWC3_GCTL_PRTCAPDIR_MASK |
                DWC3_GCTL_SCALEDOWN_MASK |
                DWC3_GCTL_DISSCRAMBLE);
  Register |= DWC3_GCTL_PRTCAP_DEVICE;
  if ((Dwc3Read32 (Device, DWC3_GHWPARAMS1) &
       DWC3_GHWPARAMS1_EN_PWROPT) == DWC3_GHWPARAMS1_EN_PWROPT_CLK)
  {
    Register &= ~DWC3_GCTL_DSBLCLKGTNG;
  }

  Dwc3Write32 (Device, DWC3_GCTL, Register);

  // Preserve the RK3588 AXI and PHY quirks used by the platform's normal
  // xHCI initialization path when the same dual-role core becomes a device.
  MmioAndThenOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_GSBUSCFG0),
    ~DWC3_GSBUSCFG0_BURST_MASK,
    DWC3_GSBUSCFG0_BURST_ENABLE
    );
  MmioOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_GSBUSCFG1),
    DWC3_GSBUSCFG1_BURST_LIMIT
    );
  MmioOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_GFLADJ),
    DWC3_GFLADJ_30MHZ_REG_SEL | DWC3_GFLADJ_30MHZ_DEFAULT
    );
  MmioAnd32 (
    DWC3_REG (Device->ControllerBase, DWC3_GUSB3PIPECTL0),
    ~DWC3_GUSB3PIPECTL_DEPOCHANGE
    );
  MmioOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_GUCTL1),
    DWC3_GUCTL1_TX_IPGAP_LINECHECK_DIS |
    DWC3_GUCTL1_PARKMODE_DISABLE_SS |
    DWC3_GUCTL1_PARKMODE_DISABLE_HS
    );
  if ((Revision & DWC3_RELEASE_MASK) >= DWC3_RELEASE_290A) {
    MmioOr32 (
      DWC3_REG (Device->ControllerBase, DWC3_GUCTL1),
      DWC3_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK
      );
  }

  // RK3588 uses a 16-bit UTMI+ interface and the same DWC3 quirks as the host
  // initialization path in UsbHcdInitDxe.
  MmioAndThenOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_GUSB2PHYCFG0),
    ~(DWC3_GUSB2PHYCFG_USBTRDTIM_MASK |
      DWC3_GUSB2PHYCFG_ENBLSLPM |
      DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS |
      DWC3_GUSB2PHYCFG_SUSPHY),
    DWC3_GUSB2PHYCFG_USBTRDTIM (5) | DWC3_GUSB2PHYCFG_PHYIF
    );
  MmioAnd32 (
    DWC3_REG (Device->ControllerBase, DWC3_GUSB3PIPECTL0),
    ~DWC3_GUSB3PIPECTL_SUSPHY
    );

  // Reset the device side of the core after selecting peripheral mode.
  MmioOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_DCTL),
    DWC3_DCTL_CSFTRST
    );
  Status = Dwc3WaitRegisterClear (
             Device,
             DWC3_DCTL,
             DWC3_DCTL_CSFTRST,
             100000
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DWC3: device soft reset timed out\n"));
    return Status;
  }

  Register = Dwc3Read32 (Device, DWC3_GEVNTCOUNT0) &
             DWC3_EVENT_COUNT_MASK;
  if (Register != 0) {
    Dwc3Write32 (Device, DWC3_GEVNTCOUNT0, Register);
  }

  Dwc3Write32 (
    Device,
    DWC3_GEVNTADRLO0,
    (UINT32)Device->EventDeviceAddress
    );
  Dwc3Write32 (
    Device,
    DWC3_GEVNTADRHI0,
    (UINT32)(Device->EventDeviceAddress >> 32)
    );
  Dwc3Write32 (
    Device,
    DWC3_GEVNTSIZ0,
    BIT31 | DWC3_EVENT_BUFFER_SIZE
    );

  Register  = Dwc3Read32 (Device, DWC3_DCFG);
  Register &= ~(DWC3_DCFG_SPEED_MASK | DWC3_DCFG_DEVADDR_MASK);
  Register |= DWC3_DCFG_SPEED_HS;
  Dwc3Write32 (Device, DWC3_DCFG, Register);

  Dwc3Write32 (
    Device,
    DWC3_DEVTEN,
    DWC3_DEVTEN_DISCONNEVTEN |
    DWC3_DEVTEN_USBRSTEVTEN |
    DWC3_DEVTEN_CONNECTDONEEN |
    DWC3_DEVTEN_ULSTCNGEN |
    DWC3_DEVTEN_ERRTICERREN |
    DWC3_DEVTEN_EVNTOVERFLOWEN
    );

  // Endpoint zero starts a new configuration and owns one transfer resource
  // in each direction.
  Status = Dwc3EndpointCommand (
             Device,
             DWC3_EP0_OUT,
             DWC3_DEPCMD_DEPSTARTCFG,
             0,
             0,
             0,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Dwc3ConfigureEndpoint (
             Device,
             DWC3_EP0_OUT,
             DWC3_ENDPOINT_TYPE_CONTROL,
             64,
             FALSE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Dwc3ConfigureEndpoint (
             Device,
             DWC3_EP0_IN,
             DWC3_ENDPOINT_TYPE_CONTROL,
             64,
             FALSE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Dwc3Write32 (Device, DWC3_DALEPENA, BIT0 | BIT1);
  Status = Dwc3QueueSetup (Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioOr32 (
    DWC3_REG (Device->ControllerBase, DWC3_DCTL),
    DWC3_DCTL_RUN_STOP
    );
  Device->Started = TRUE;
  Status = Dwc3WaitRegisterClear (
             Device,
             DWC3_DSTS,
             DWC3_DSTS_DEVCTRLHLT,
             100000
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DWC3: device controller failed to run\n"));
    MmioAnd32 (
      DWC3_REG (Device->ControllerBase, DWC3_DCTL),
      ~DWC3_DCTL_RUN_STOP
      );
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
Dwc3DeviceStart (
  IN  EFI_PHYSICAL_ADDRESS       ControllerBase,
  IN  UINT8                      MaxLun,
  IN  DWC3_MASS_STORAGE_RESET    Reset,
  IN  VOID                       *ResetContext,
  IN  DWC3_CANCEL_CHECK          Cancel OPTIONAL,
  IN  VOID                       *CancelContext OPTIONAL,
  OUT DWC3_DEVICE                **Device
  )
{
  DWC3_DEVICE  *NewDevice;
  EFI_STATUS   Status;

  if ((Device == NULL) || (MaxLun > 15)) {
    return EFI_INVALID_PARAMETER;
  }

  *Device   = NULL;
  NewDevice = AllocateZeroPool (sizeof (*NewDevice));
  if (NewDevice == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  NewDevice->Signature       = DWC3_DEVICE_SIGNATURE;
  NewDevice->ControllerBase  = ControllerBase;
  NewDevice->MaxLun          = MaxLun;
  NewDevice->Reset           = Reset;
  NewDevice->ResetContext    = ResetContext;
  NewDevice->Cancel          = Cancel;
  NewDevice->CancelContext   = CancelContext;
  NewDevice->BulkMaxPacket   = 512;

  CopyMem (
    NewDevice->ConfigDescriptor,
    mConfigDescriptorTemplate,
    sizeof (NewDevice->ConfigDescriptor)
    );
  Dwc3UpdateEndpointDescriptors (NewDevice);

  Status = Dwc3InitializeCommonBuffer (NewDevice);
  if (EFI_ERROR (Status)) {
    FreePool (NewDevice);
    return Status;
  }

  Status = Dwc3InitializeController (NewDevice);
  if (EFI_ERROR (Status)) {
    Dwc3DeviceStop (NewDevice);
    return Status;
  }

  *Device = NewDevice;
  return EFI_SUCCESS;
}

VOID
Dwc3DeviceStop (
  IN DWC3_DEVICE  *Device
  )
{
  UINTN  Index;

  if ((Device == NULL) || (Device->Signature != DWC3_DEVICE_SIGNATURE)) {
    return;
  }

  if (Device->ControllerAccessible) {
    for (Index = 0; Index < DWC3_EP_COUNT; Index++) {
      Dwc3AbortEndpoint (Device, (UINT8)Index, TRUE);
    }

    if (Device->Started) {
      MmioAnd32 (
        DWC3_REG (Device->ControllerBase, DWC3_DCTL),
        ~DWC3_DCTL_RUN_STOP
        );
      if (EFI_ERROR (Dwc3WaitRegisterSet (
                       Device,
                       DWC3_DSTS,
                       DWC3_DSTS_DEVCTRLHLT,
                       100000
                       )))
      {
        DEBUG ((DEBUG_WARN, "DWC3: device controller failed to halt\n"));
      }

      Device->Started = FALSE;
    }

    Dwc3Write32 (Device, DWC3_DEVTEN, 0);
    Dwc3Write32 (Device, DWC3_GEVNTSIZ0, BIT31);
    Device->ControllerAccessible = FALSE;
  }

  Dwc3FreeCommonBuffer (Device);
  Device->Signature = 0;
  FreePool (Device);
}

EFI_STATUS
Dwc3DevicePoll (
  IN DWC3_DEVICE  *Device
  )
{
  if ((Device == NULL) || (Device->Signature != DWC3_DEVICE_SIGNATURE)) {
    return EFI_INVALID_PARAMETER;
  }

  return Dwc3ProcessEvents (Device);
}

BOOLEAN
Dwc3DeviceIsConfigured (
  IN DWC3_DEVICE  *Device
  )
{
  return (Device != NULL) &&
         (Device->Signature == DWC3_DEVICE_SIGNATURE) &&
         Device->Configured;
}

EFI_STATUS
Dwc3BulkTransfer (
  IN     DWC3_DEVICE  *Device,
  IN     BOOLEAN      DirectionIn,
  IN OUT VOID         *Buffer,
  IN     UINTN        BufferSize,
  OUT    UINTN        *Transferred
  )
{
  UINT8  PhysicalEp;

  if ((Device == NULL) ||
      (Device->Signature != DWC3_DEVICE_SIGNATURE) ||
      (Transferred == NULL) ||
      ((Buffer == NULL) && (BufferSize != 0)))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (!Device->Configured) {
    return EFI_NOT_READY;
  }

  PhysicalEp = DirectionIn ? DWC3_EP1_IN : DWC3_EP1_OUT;
  return Dwc3MapAndTransfer (
           Device,
           PhysicalEp,
           Buffer,
           BufferSize,
           DWC3_TRBCTL_NORMAL,
           Dwc3TransferBulk,
           Transferred
           );
}

EFI_STATUS
Dwc3BulkSetStall (
  IN DWC3_DEVICE  *Device,
  IN BOOLEAN      DirectionIn,
  IN BOOLEAN      Stall
  )
{
  if ((Device == NULL) || (Device->Signature != DWC3_DEVICE_SIGNATURE)) {
    return EFI_INVALID_PARAMETER;
  }

  return Dwc3SetEndpointStall (
           Device,
           DirectionIn ? DWC3_EP1_IN : DWC3_EP1_OUT,
           Stall
           );
}

EFI_STATUS
Dwc3BulkWaitForClearStall (
  IN DWC3_DEVICE  *Device,
  IN BOOLEAN      DirectionIn
  )
{
  DWC3_ENDPOINT  *Endpoint;
  EFI_STATUS     Status;

  if ((Device == NULL) || (Device->Signature != DWC3_DEVICE_SIGNATURE)) {
    return EFI_INVALID_PARAMETER;
  }

  Endpoint = &Device->Endpoints[DirectionIn ? DWC3_EP1_IN : DWC3_EP1_OUT];
  while (Endpoint->Stalled) {
    Status = Dwc3ProcessEvents (Device);
    if (EFI_ERROR (Status) && (Status != EFI_NOT_READY)) {
      return Status;
    }

    if ((Device->Cancel != NULL) && Device->Cancel (Device->CancelContext)) {
      return EFI_ABORTED;
    }

    MicroSecondDelay (50);
  }

  return EFI_SUCCESS;
}

VOID
Dwc3BulkReset (
  IN DWC3_DEVICE  *Device
  )
{
  if ((Device == NULL) || (Device->Signature != DWC3_DEVICE_SIGNATURE)) {
    return;
  }

  Dwc3AbortBulkTransfers (Device);
  Dwc3SetEndpointStall (Device, DWC3_EP1_OUT, FALSE);
  Dwc3SetEndpointStall (Device, DWC3_EP1_IN, FALSE);
}
