#pragma once
#include "sys_defs.h"
void biz_initialize(SystemVersionInfo versionInfo, unsigned long long envFlag, unsigned long envIndex, unsigned long long cpuAffinityMask, const wchar_t* rootPath, DWORD rootPathCount);
