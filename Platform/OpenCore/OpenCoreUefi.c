/** @file
  OpenCore driver.

Copyright (c) 2019, vit9696. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <OpenCore.h>

#include <Guid/OcVariable.h>
#include <Guid/GlobalVariable.h>

#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAfterBootCompatLib.h>
#include <Library/OcAppleBootPolicyLib.h>
#include <Library/OcAppleEventLib.h>
#include <Library/OcAppleImageConversionLib.h>
#include <Library/OcAudioLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcInputLib.h>
#include <Library/OcApfsLib.h>
#include <Library/OcAppleKeyMapLib.h>
#include <Library/OcAppleUserInterfaceThemeLib.h>
#include <Library/OcConsoleLib.h>
#include <Library/OcCpuLib.h>
#include <Library/OcDataHubLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/OcDevicePropertyLib.h>
#include <Library/OcDriverConnectionLib.h>
#include <Library/OcFirmwareVolumeLib.h>
#include <Library/OcHashServicesLib.h>
#include <Library/OcMiscLib.h>
#include <Library/OcRtcLib.h>
#include <Library/OcSmcLib.h>
#include <Library/OcOSInfoLib.h>
#include <Library/OcUnicodeCollationEngGenericLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

#define OC_EXIT_BOOT_SERVICES_HANDLER_MAX 5

STATIC EFI_EVENT_NOTIFY mOcExitBootServicesHandlers[OC_EXIT_BOOT_SERVICES_HANDLER_MAX+1];
STATIC VOID             *mOcExitBootServicesContexts[OC_EXIT_BOOT_SERVICES_HANDLER_MAX];
STATIC UINTN            mOcExitBootServicesIndex;

VOID
OcScheduleExitBootServices (
  IN EFI_EVENT_NOTIFY   Handler,
  IN VOID               *Context
  )
{
  if (mOcExitBootServicesIndex + 1 == OC_EXIT_BOOT_SERVICES_HANDLER_MAX) {
    ASSERT (FALSE);
    return;
  }

  mOcExitBootServicesHandlers[mOcExitBootServicesIndex] = Handler;
  mOcExitBootServicesContexts[mOcExitBootServicesIndex] = Context;
  ++mOcExitBootServicesIndex;
}

STATIC
VOID
OcLoadDrivers (
  IN  OC_STORAGE_CONTEXT  *Storage,
  IN  OC_GLOBAL_CONFIG    *Config,
  OUT EFI_HANDLE          **DriversToConnect  OPTIONAL
  )
{
  EFI_STATUS  Status;
  VOID        *Driver;
  UINT32      DriverSize;
  UINT32      Index;
  CHAR16      DriverPath[OC_STORAGE_SAFE_PATH_MAX];
  EFI_HANDLE  ImageHandle;
  EFI_HANDLE  *DriversToConnectIterator;
  VOID        *DriverBinding;
  BOOLEAN     SkipDriver;

  DriversToConnectIterator = NULL;
  if (DriversToConnect != NULL) {
    *DriversToConnect = NULL;
  }

  DEBUG ((DEBUG_INFO, "OC: Got %u drivers\n", Config->Uefi.Drivers.Count));

  for (Index = 0; Index < Config->Uefi.Drivers.Count; ++Index) {
    SkipDriver = OC_BLOB_GET (Config->Uefi.Drivers.Values[Index])[0] == '#';

    DEBUG ((
      DEBUG_INFO,
      "OC: Driver %a at %u is %a\n",
      OC_BLOB_GET (Config->Uefi.Drivers.Values[Index]),
      Index,
      SkipDriver ? "skipped!" : "being loaded..."
      ));

    //
    // Skip drivers marked as comments.
    //
    if (SkipDriver) {
      continue;
    }

    Status = OcUnicodeSafeSPrint (
      DriverPath,
      sizeof (DriverPath),
      OPEN_CORE_UEFI_DRIVER_PATH "%a",
      OC_BLOB_GET (Config->Uefi.Drivers.Values[Index])
      );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "OC: Driver %s%a doex not fit path!\n",
        OPEN_CORE_UEFI_DRIVER_PATH,
        OC_BLOB_GET (Config->Uefi.Drivers.Values[Index])
        ));
      continue;
    }

    Driver = OcStorageReadFileUnicode (Storage, DriverPath, &DriverSize);
    if (Driver == NULL) {
      DEBUG ((
        DEBUG_ERROR,
        "OC: Driver %a at %u cannot be found!\n",
        OC_BLOB_GET (Config->Uefi.Drivers.Values[Index]),
        Index
        ));
      //
      // TODO: This should cause security violation if configured!
      //
      continue;
    }

    //
    // TODO: Use AppleLoadedImage!!
    //
    ImageHandle = NULL;
    Status = gBS->LoadImage (
      FALSE,
      gImageHandle,
      NULL,
      Driver,
      DriverSize,
      &ImageHandle
      );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "OC: Driver %a at %u cannot be loaded - %r!\n",
        OC_BLOB_GET (Config->Uefi.Drivers.Values[Index]),
        Index,
        Status
        ));
      FreePool (Driver);
      continue;
    }

    Status = gBS->StartImage (
      ImageHandle,
      NULL,
      NULL
      );

    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "OC: Driver %a at %u cannot be started - %r!\n",
        OC_BLOB_GET (Config->Uefi.Drivers.Values[Index]),
        Index,
        Status
        ));
      gBS->UnloadImage (ImageHandle);
    }

    if (!EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "OC: Driver %a at %u is successfully loaded!\n",
        OC_BLOB_GET (Config->Uefi.Drivers.Values[Index]),
        Index
        ));

      if (DriversToConnect != NULL) {
        Status = gBS->HandleProtocol (
          ImageHandle,
          &gEfiDriverBindingProtocolGuid,
          (VOID **) &DriverBinding
          );

        if (!EFI_ERROR (Status)) {
          if (*DriversToConnect == NULL) {
            //
            // Allocate enough entries for the drivers to connect.
            //
            *DriversToConnect = AllocatePool (
              (Config->Uefi.Drivers.Count + 1 - Index) * sizeof (**DriversToConnect)
              );

            if (*DriversToConnect != NULL) {
              DriversToConnectIterator = *DriversToConnect;
            } else {
              DEBUG ((DEBUG_ERROR, "OC: Failed to allocate memory for drivers to connect\n"));
              FreePool (Driver);
              return;
            }
          }

          *DriversToConnectIterator = ImageHandle;
          ++DriversToConnectIterator;

          DEBUG ((
            DEBUG_INFO,
            "OC: Driver %a at %u needs connection.\n",
            OC_BLOB_GET (Config->Uefi.Drivers.Values[Index]),
            Index
            ));
        }
      }
    }

    FreePool (Driver);
  }

  //
  // Driver connection list should be null-terminated.
  //
  if (DriversToConnectIterator != NULL) {
    *DriversToConnectIterator = NULL;
  }
}

STATIC
VOID
EFIAPI
OcExitBootServicesHandler (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS         Status;
  OC_GLOBAL_CONFIG   *Config;

  Config = (OC_GLOBAL_CONFIG *) Context;

  //
  // Printing from ExitBootServices is dangerous, as it may cause
  // memory reallocation, which can make ExitBootServices fail.
  // Only do that on error, which is not expected.
  //

  if (Config->Uefi.Quirks.ReleaseUsbOwnership) {
    Status = ReleaseUsbOwnership ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "OC: ReleaseUsbOwnership - %r\n", Status));
    }
  }

  //
  // FIXME: This is a very ugly hack for (at least) ASUS Z87-Pro.
  // This board results in still waiting for root devices due to firmware
  // performing some timer(?) actions in parallel to ExitBootServices.
  // Some day we should figure out what exactly happens there.
  // It is not the first time I face this, check AptioInputFix timer code:
  // https://github.com/acidanthera/AptioFixPkg/blob/e54c185/Platform/AptioInputFix/Timer/AIT.c#L72-L73
  // Roughly 5 seconds is good enough.
  //
  if (Config->Uefi.Quirks.ExitBootServicesDelay > 0) {
    gBS->Stall (Config->Uefi.Quirks.ExitBootServicesDelay);
  }
}

STATIC
VOID
OcReinstallProtocols (
  IN OC_GLOBAL_CONFIG    *Config
  )
{
  if (OcAudioInstallProtocols (Config->Uefi.ProtocolOverrides.AppleAudio) == NULL) {
    DEBUG ((DEBUG_INFO, "OC: Disabling audio in favour of firmware implementation\n"));
  }

  if (OcAppleBootPolicyInstallProtocol (Config->Uefi.ProtocolOverrides.AppleBootPolicy) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install boot policy protocol\n"));
  }

  if (OcDataHubInstallProtocol (Config->Uefi.ProtocolOverrides.DataHub) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install data hub protocol\n"));
  }

  if (OcDevicePathPropertyInstallProtocol (Config->Uefi.ProtocolOverrides.DeviceProperties) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install device properties protocol\n"));
  }

  if (OcAppleImageConversionInstallProtocol (Config->Uefi.ProtocolOverrides.AppleImageConversion) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install image conversion protocol\n"));
  }

  if (OcAppleDebugLogInstallProtocol (Config->Uefi.ProtocolOverrides.AppleDebugLog) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install debug log protocol\n"));
  }

  if (OcSmcIoInstallProtocol (Config->Uefi.ProtocolOverrides.AppleSmcIo, Config->Misc.Security.AuthRestart) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install smc i/o protocol\n"));
  }

  if (OcAppleUserInterfaceThemeInstallProtocol (Config->Uefi.ProtocolOverrides.AppleUserInterfaceTheme) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install user interface theme protocol\n"));
  }

  if (OcUnicodeCollationEngInstallProtocol (Config->Uefi.ProtocolOverrides.UnicodeCollation) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install unicode collation protocol\n"));
  }

  if (OcHashServicesInstallProtocol (Config->Uefi.ProtocolOverrides.HashServices) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install hash services protocol\n"));
  }

  if (OcAppleKeyMapInstallProtocols (Config->Uefi.ProtocolOverrides.AppleKeyMap) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install key map protocols\n"));
  }

  if (OcAppleEventInstallProtocol (Config->Uefi.ProtocolOverrides.AppleEvent) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install key event protocol\n"));
  }

  if (OcFirmwareVolumeInstallProtocol (Config->Uefi.ProtocolOverrides.FirmwareVolume) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install firmware volume protocol\n"));
  }

  if (OcOSInfoInstallProtocol (Config->Uefi.ProtocolOverrides.OSInfo) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install os info protocol\n"));
  }

  if (OcAppleRtcRamInstallProtocol (Config->Uefi.ProtocolOverrides.AppleRtcRam) == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install rtc ram protocol\n"));
  }
}

VOID
OcLoadBooterUefiSupport (
  IN OC_GLOBAL_CONFIG  *Config
  )
{
  OC_ABC_SETTINGS  AbcSettings;
  UINT32           Index;
  UINT32           NextIndex;

  ZeroMem (&AbcSettings, sizeof (AbcSettings));

  AbcSettings.AvoidRuntimeDefrag     = Config->Booter.Quirks.AvoidRuntimeDefrag;
  AbcSettings.DevirtualiseMmio       = Config->Booter.Quirks.DevirtualiseMmio;
  AbcSettings.DisableSingleUser      = Config->Booter.Quirks.DisableSingleUser;
  AbcSettings.DisableVariableWrite   = Config->Booter.Quirks.DisableVariableWrite;
  AbcSettings.ProtectSecureBoot      = Config->Booter.Quirks.ProtectSecureBoot;
  AbcSettings.DiscardHibernateMap    = Config->Booter.Quirks.DiscardHibernateMap;
  AbcSettings.EnableSafeModeSlide    = Config->Booter.Quirks.EnableSafeModeSlide;
  AbcSettings.EnableWriteUnprotector = Config->Booter.Quirks.EnableWriteUnprotector;
  AbcSettings.ForceExitBootServices  = Config->Booter.Quirks.ForceExitBootServices;
  AbcSettings.ProtectMemoryRegions   = Config->Booter.Quirks.ProtectMemoryRegions;
  AbcSettings.ProvideCustomSlide     = Config->Booter.Quirks.ProvideCustomSlide;
  AbcSettings.ProtectUefiServices    = Config->Booter.Quirks.ProtectUefiServices;
  AbcSettings.RebuildAppleMemoryMap  = Config->Booter.Quirks.RebuildAppleMemoryMap;
  AbcSettings.SetupVirtualMap        = Config->Booter.Quirks.SetupVirtualMap;
  AbcSettings.SignalAppleOS          = Config->Booter.Quirks.SignalAppleOS;
  AbcSettings.SyncRuntimePermissions = Config->Booter.Quirks.SyncRuntimePermissions;

  if (AbcSettings.DevirtualiseMmio && Config->Booter.MmioWhitelist.Count > 0) {
    AbcSettings.MmioWhitelist = AllocatePool (
      Config->Booter.MmioWhitelist.Count * sizeof (AbcSettings.MmioWhitelist[0])
      );

    if (AbcSettings.MmioWhitelist != NULL) {
      NextIndex = 0;
      for (Index = 0; Index < Config->Booter.MmioWhitelist.Count; ++Index) {
        if (Config->Booter.MmioWhitelist.Values[Index]->Enabled) {
          AbcSettings.MmioWhitelist[NextIndex] = Config->Booter.MmioWhitelist.Values[Index]->Address;
          ++NextIndex;
        }
      }
      AbcSettings.MmioWhitelistSize = NextIndex;
    } else {
      DEBUG ((
        DEBUG_ERROR,
        "OC: Failed to allocate %u slots for mmio addresses\n",
        (UINT32) Config->Booter.MmioWhitelist.Count
        ));
    }
  }

  AbcSettings.ExitBootServicesHandlers = mOcExitBootServicesHandlers;
  AbcSettings.ExitBootServicesHandlerContexts = mOcExitBootServicesContexts;

  OcAbcInitialize (&AbcSettings);
}

VOID
OcLoadUefiSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config,
  IN OC_CPU_INFO         *CpuInfo
  )
{
  EFI_STATUS            Status;
  EFI_HANDLE            *DriversToConnect;
  UINTN                 Index;
  UINT16                *BootOrder;
  UINTN                 BootOrderCount;
  BOOLEAN               BootOrderChanged;
  EFI_EVENT             Event;
  EFI_PHYSICAL_ADDRESS  ReservedAddress;

  OcReinstallProtocols (Config);

  OcLoadUefiInputSupport (Config);

  //
  // Setup Apple bootloader specific UEFI features.
  //
  OcLoadBooterUefiSupport (Config);

  if (Config->Uefi.Quirks.IgnoreInvalidFlexRatio) {
    OcCpuCorrectFlexRatio (CpuInfo);
  }

  DEBUG ((
    DEBUG_INFO,
    "OC: RBVR %d DDBR %d\n",
    Config->Uefi.Quirks.RequestBootVarRouting,
    Config->Uefi.Quirks.DeduplicateBootOrder
    ));

  //
  // Inform platform support whether we want Boot#### routing or not.
  //
  gRT->SetVariable (
    OC_BOOT_REDIRECT_VARIABLE_NAME,
    &gOcVendorVariableGuid,
    OPEN_CORE_INT_NVRAM_ATTR,
    sizeof (Config->Uefi.Quirks.RequestBootVarRouting),
    &Config->Uefi.Quirks.RequestBootVarRouting
    );

  if (Config->Uefi.Quirks.DeduplicateBootOrder) {
    BootOrder = OcGetBootOrder (
      &gEfiGlobalVariableGuid,
      FALSE,
      &BootOrderCount,
      &BootOrderChanged,
      NULL
      );

    if (BootOrder != NULL) {
      if (BootOrderChanged) {
        DEBUG ((DEBUG_INFO, "OC: Performed BootOrder deduplication\n"));
        gRT->SetVariable (
          EFI_BOOT_ORDER_VARIABLE_NAME,
          &gEfiGlobalVariableGuid,
          EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE,
          BootOrderCount * sizeof (UINT16),
          BootOrder
          );
      }

      FreePool (BootOrder);
    }
  }

  if (Config->Uefi.Quirks.UnblockFsConnect) {
    OcUnblockUnmountedPartitions ();
  }

  OcMiscUefiQuirksLoaded (Config);

  for (Index = 0; Index < Config->Uefi.ReservedMemory.Count; ++Index) {
    if (!Config->Uefi.ReservedMemory.Values[Index]->Enabled) {
      continue;
    }

    if ((Config->Uefi.ReservedMemory.Values[Index]->Address & (BASE_4KB - 1)) != 0
      || (Config->Uefi.ReservedMemory.Values[Index]->Size & (BASE_4KB - 1)) != 0) {
      Status = EFI_INVALID_PARAMETER;
    } else {
      ReservedAddress = Config->Uefi.ReservedMemory.Values[Index]->Address;
      Status = gBS->AllocatePages (
        AllocateAddress,
        EfiReservedMemoryType,
        (UINTN) EFI_SIZE_TO_PAGES (Config->Uefi.ReservedMemory.Values[Index]->Size),
        &ReservedAddress
        );
    }

    DEBUG ((
      DEBUG_INFO,
      "OC: Reserving region %Lx of %Lx size - %r\n",
      Config->Uefi.ReservedMemory.Values[Index]->Address,
      Config->Uefi.ReservedMemory.Values[Index]->Size,
      Status
      ));
  }

  if (Config->Uefi.ConnectDrivers) {
    OcLoadDrivers (Storage, Config, &DriversToConnect);
    DEBUG ((DEBUG_INFO, "OC: Connecting drivers...\n"));
    if (DriversToConnect != NULL) {
      OcRegisterDriversToHighestPriority (DriversToConnect);
      //
      // DriversToConnect is not freed as it is owned by OcRegisterDriversToHighestPriority.
      //
    }
    OcConnectDrivers ();
    DEBUG ((DEBUG_INFO, "OC: Connecting drivers done...\n"));
  } else {
    OcLoadDrivers (Storage, Config, NULL);
  }

  if (Config->Uefi.Apfs.EnableJumpstart) {
    OcApfsConfigure (
      Config->Uefi.Apfs.MinVersion,
      Config->Uefi.Apfs.MinDate,
      Config->Misc.Security.ScanPolicy,
      Config->Uefi.Apfs.HideVerbose
      );

    OcApfsConnectDevices (
      Config->Uefi.Apfs.JumpstartHotPlug
      );
  }

  OcLoadUefiOutputSupport (Config);

  OcLoadUefiAudioSupport (Storage, Config);

  gBS->CreateEvent (
    EVT_SIGNAL_EXIT_BOOT_SERVICES,
    TPL_CALLBACK,
    OcExitBootServicesHandler,
    Config,
    &Event
    );
}
