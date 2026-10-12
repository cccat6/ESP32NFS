// SPDX-License-Identifier: MIT
#pragma once
// Optional filesystem implementation: instantiated only by applications selecting this backend.
#include <SdFat.h>

#include "FsLib/FsLib.h"
#include "ExFatLib/ExFatFile.ipp"
#include "ExFatLib/ExFatFilePrint.ipp"
#include "ExFatLib/ExFatFileWrite.ipp"
#include "ExFatLib/ExFatName.ipp"
#include "ExFatLib/ExFatPartition.ipp"
#include "ExFatLib/ExFatVolume.ipp"
#include "FatLib/FatFile.ipp"
#include "FatLib/FatFileLFN.ipp"
#include "FatLib/FatFilePrint.ipp"
#include "FatLib/FatFileSFN.ipp"
#include "FatLib/FatName.ipp"
#include "FatLib/FatPartition.ipp"
#include "FatLib/FatVolume.ipp"
#include "FsLib/FsFile.ipp"
#include "FsLib/FsVolume.ipp"
#include "common/FsCache.ipp"
#include "common/FsName.ipp"
