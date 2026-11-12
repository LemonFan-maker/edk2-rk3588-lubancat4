/** @file
  Minimal polling-mode Synopsys DWC3 USB device controller interface.

  This interface is intentionally small.  It only exposes the operations that
  are required by the USB Mass Storage mode application: endpoint zero,
  Bulk-OUT endpoint 1 and Bulk-IN endpoint 1.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef DWC3_DEVICE_H_
#define DWC3_DEVICE_H_

#include <Uefi.h>

typedef struct _DWC3_DEVICE DWC3_DEVICE;

typedef
VOID
(EFIAPI *DWC3_MASS_STORAGE_RESET)(
  IN VOID  *Context
  );

typedef
BOOLEAN
(EFIAPI *DWC3_CANCEL_CHECK)(
  IN VOID  *Context
  );

/**
  Create and start a USB 2.0 Mass Storage device on a DWC3 controller.

  @param[in]  ControllerBase  Base address of the complete DWC3 register range.
  @param[in]  MaxLun          Highest valid Mass Storage LUN number.
  @param[in]  Reset           Callback for a Bulk-Only Mass Storage Reset.
  @param[in]  ResetContext    Context passed to Reset.
  @param[in]  Cancel          Optional callback used by blocking transfers.
  @param[in]  CancelContext   Context passed to Cancel.
  @param[out] Device          Newly allocated controller context.

  @retval EFI_SUCCESS            Controller started.
  @retval EFI_INVALID_PARAMETER  Device is NULL or MaxLun is invalid.
  @retval EFI_NOT_FOUND          The MMIO range is not a DWC3 controller.
  @retval EFI_OUT_OF_RESOURCES   DMA memory could not be allocated.
  @retval EFI_DEVICE_ERROR       Controller initialization failed.
**/
EFI_STATUS
Dwc3DeviceStart (
  IN  EFI_PHYSICAL_ADDRESS       ControllerBase,
  IN  UINT8                      MaxLun,
  IN  DWC3_MASS_STORAGE_RESET    Reset,
  IN  VOID                       *ResetContext,
  IN  DWC3_CANCEL_CHECK          Cancel OPTIONAL,
  IN  VOID                       *CancelContext OPTIONAL,
  OUT DWC3_DEVICE                **Device
  );

/**
  Stop the device controller and release its DMA resources.
**/
VOID
Dwc3DeviceStop (
  IN DWC3_DEVICE  *Device
  );

/**
  Poll and process all currently pending USB events.

  This must be called while waiting for the host to configure the device.
**/
EFI_STATUS
Dwc3DevicePoll (
  IN DWC3_DEVICE  *Device
  );

/**
  Return TRUE after the host selected configuration 1.
**/
BOOLEAN
Dwc3DeviceIsConfigured (
  IN DWC3_DEVICE  *Device
  );

/**
  Execute one synchronous transfer on Mass Storage endpoint 1.

  For OUT transfers, BufferSize is the available receive capacity and
  Transferred reports the number of bytes supplied by the host.  A short OUT
  packet therefore completes the operation successfully.

  @param[in]     Device       Controller context.
  @param[in]     DirectionIn  TRUE for device-to-host, FALSE for host-to-device.
  @param[in,out] Buffer       Transfer buffer.
  @param[in]     BufferSize   Transfer length or OUT receive capacity.
  @param[out]    Transferred  Completed byte count.
**/
EFI_STATUS
Dwc3BulkTransfer (
  IN     DWC3_DEVICE  *Device,
  IN     BOOLEAN      DirectionIn,
  IN OUT VOID         *Buffer,
  IN     UINTN        BufferSize,
  OUT    UINTN        *Transferred
  );

/**
  Set or clear the halt condition on a Mass Storage bulk endpoint.
**/
EFI_STATUS
Dwc3BulkSetStall (
  IN DWC3_DEVICE  *Device,
  IN BOOLEAN      DirectionIn,
  IN BOOLEAN      Stall
  );

/**
  Process control events until the host clears a bulk endpoint halt.

  This is used by Bulk-Only Transport error handling before sending the CSW.
**/
EFI_STATUS
Dwc3BulkWaitForClearStall (
  IN DWC3_DEVICE  *Device,
  IN BOOLEAN      DirectionIn
  );

/**
  Abort both bulk endpoints and clear their halt/data-toggle state.
**/
VOID
Dwc3BulkReset (
  IN DWC3_DEVICE  *Device
  );

#endif // DWC3_DEVICE_H_
