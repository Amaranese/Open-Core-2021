/** @file
  Copyright (C) 2020, vit9696. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#ifndef OC_CPU_INTERNALS_H
#define OC_CPU_INTERNALS_H

//
// Tolerance within which we consider two frequency values to be roughly
// equivalent.
//
#define OC_CPU_FREQUENCY_TOLERANCE 50000000ULL // 50 Mhz

/**
  Returns microcode revision for Intel CPUs.

  @retval  microcode revision.
**/
UINT32
EFIAPI
AsmReadIntelMicrocodeRevision (
  VOID
  );

/**
  Detect Apple CPU major type.

  @param[in] BrandString   CPU Brand String from CPUID.

  @retval Apple CPU major type.
**/
UINT8
InternalDetectAppleMajorType (
  IN  CONST CHAR8  *BrandString
  );

/**
  Detect Apple CPU type.

  @param[in] Model           CPU model from CPUID.
  @param[in] Stepping        CPU stepping from CPUID.
  @param[in] AppleMajorType  Apple CPU major type.

  @retval Apple CPU type.
**/
UINT16
InternalDetectAppleProcessorType (
  IN UINT8  Model,
  IN UINT8  Stepping,
  IN UINT8  AppleMajorType
  );

/**
  Obtain ACPI PM timer address for this BSP.

  @param[out]  Type   Address source type, optional.

  @retval ACPI PM timer address or 0.
**/
UINTN
InternalGetPmTimerAddr (
  OUT CONST CHAR8 **Type  OPTIONAL
  );

/**
  Calculate the TSC frequency via PM timer

  @param[in] Recalculate  Do not re-use previously cached information.

  @retval  The calculated TSC frequency.
**/
UINT64
InternalCalculateTSCFromPMTimer (
  IN BOOLEAN  Recalculate
  );

/**
  Calculate the ART frequency and derieve the CPU frequency for Intel CPUs

  @param[out] CPUFrequency  The derieved CPU frequency.
  @param[in]  Recalculate   Do not re-use previously cached information.

  @retval  The calculated ART frequency.
**/
UINT64
InternalCalculateARTFrequencyIntel (
  OUT UINT64   *CPUFrequency,
  IN  BOOLEAN  Recalculate
  );

/**
  Calculate the CPU frequency via VMT for hypervisors

  @param[out] FSBFrequency     FSB frequency, optional.
  @param[out] UnderHypervisor  Hypervisor status, optional.

  @retval  CPU frequency or 0.
**/
UINT64
InternalCalculateVMTFrequency (
  OUT UINT64   *FSBFrequency     OPTIONAL,
  OUT BOOLEAN  *UnderHypervisor  OPTIONAL
  );

#endif // OC_CPU_INTERNALS_H
