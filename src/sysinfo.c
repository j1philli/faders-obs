/*
 * Faders OBS plugin
 * Copyright (C) 2026 Josh Phillips
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sysinfo.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

typedef LONG(WINAPI *rtl_get_version_t)(OSVERSIONINFOW *);
typedef BOOL(WINAPI *is_wow64_process2_t)(HANDLE, USHORT *, USHORT *);

#ifndef IMAGE_FILE_MACHINE_ARM64
#define IMAGE_FILE_MACHINE_ARM64 0xAA64
#endif

static void reg_string(HKEY root, const wchar_t *path, const wchar_t *name, char *out, size_t size)
{
	wchar_t buf[256];
	DWORD bytes = sizeof(buf);
	out[0] = 0;
	if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, NULL, buf, &bytes) == ERROR_SUCCESS)
		WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, (int)size, NULL, NULL);
}

static DWORD reg_dword(HKEY root, const wchar_t *path, const wchar_t *name)
{
	DWORD v = 0, bytes = sizeof(v);
	return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, NULL, &v, &bytes) == ERROR_SUCCESS ? v : 0;
}

void fdr_sysinfo_get(struct fdr_sysinfo *out)
{
	memset(out, 0, sizeof(*out));
	// GetVersionEx lies to unmanifested programs; RtlGetVersion doesn't.
	OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof(v)};
	rtl_get_version_t rtl = (rtl_get_version_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	if (rtl)
		rtl(&v);
	const wchar_t *cur = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	char display[32];
	reg_string(HKEY_LOCAL_MACHINE, cur, L"DisplayVersion", display, sizeof(display));
	DWORD ubr = reg_dword(HKEY_LOCAL_MACHINE, cur, L"UBR");
	snprintf(out->os, sizeof(out->os), "Windows %s", v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "11" : v.dwMajorVersion == 10 ? "10" : "");
	snprintf(out->os_version, sizeof(out->os_version), "%lu.%lu.%lu.%lu%s%s%s", (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber,
		 (unsigned long)ubr, *display ? " (" : "", display, *display ? ")" : "");

	SYSTEM_INFO si;
	GetNativeSystemInfo(&si);
	const char *native = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x86_64"
			     : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
			     : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86"
											 : "unknown";
	// An x64 build on Windows on Arm runs emulated, and GetNativeSystemInfo then says x64 too.
	USHORT process_machine = 0, native_machine = 0;
	is_wow64_process2_t wow = (is_wow64_process2_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
	bool emulated = wow && wow(GetCurrentProcess(), &process_machine, &native_machine) && native_machine == IMAGE_FILE_MACHINE_ARM64;
#if defined(_M_ARM64) || defined(__aarch64__)
	emulated = false;
#endif
	if (emulated)
		snprintf(out->arch, sizeof(out->arch), "x86_64 (emulated on ARM64)");
	else
		snprintf(out->arch, sizeof(out->arch), "%s", native);

	reg_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", out->cpu, sizeof(out->cpu));
	// Trailing spaces in the brand string.
	for (size_t n = strlen(out->cpu); n > 0 && out->cpu[n - 1] == ' '; n--)
		out->cpu[n - 1] = 0;

	out->cores_logical = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	DWORD len = 0;
	GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
	if (len) {
		char *buf = HeapAlloc(GetProcessHeap(), 0, len);
		if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
			for (DWORD off = 0; off < len;) {
				SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + off);
				out->cores_physical++;
				off += e->Size;
			}
		}
		if (buf)
			HeapFree(GetProcessHeap(), 0, buf);
	}
	MEMORYSTATUSEX mem = {.dwLength = sizeof(mem)};
	if (GlobalMemoryStatusEx(&mem))
		out->ram_gb = (int)((mem.ullTotalPhys + (512ULL << 20)) >> 30);
}

#elif defined(__APPLE__)
#include <stdint.h>
#include <sys/sysctl.h>
#include <sys/types.h>

static void sysctl_string(const char *name, char *out, size_t size)
{
	size_t len = size;
	out[0] = 0;
	if (sysctlbyname(name, out, &len, NULL, 0) != 0)
		out[0] = 0;
	out[size - 1] = 0;
}

static int64_t sysctl_int(const char *name)
{
	int64_t v64 = 0;
	size_t len = sizeof(v64);
	if (sysctlbyname(name, &v64, &len, NULL, 0) != 0)
		return 0;
	if (len == sizeof(int32_t)) {
		int32_t v32;
		memcpy(&v32, &v64, sizeof(v32));
		return v32;
	}
	return v64;
}

void fdr_sysinfo_get(struct fdr_sysinfo *out)
{
	memset(out, 0, sizeof(*out));
	snprintf(out->os, sizeof(out->os), "macOS");
	char product[32], build[32];
	sysctl_string("kern.osproductversion", product, sizeof(product));
	sysctl_string("kern.osversion", build, sizeof(build));
	snprintf(out->os_version, sizeof(out->os_version), "%s%s%s%s", product, *build ? " (" : "", build, *build ? ")" : "");
	// An Intel build on Apple Silicon runs under Rosetta; hw.machine then says x86_64.
	bool translated = sysctl_int("sysctl.proc_translated") == 1;
#if defined(__aarch64__) || defined(__arm64__)
	snprintf(out->arch, sizeof(out->arch), "arm64");
#else
	snprintf(out->arch, sizeof(out->arch), "x86_64%s", translated ? " (Rosetta on Apple Silicon)" : "");
#endif
	(void)translated;
	sysctl_string("machdep.cpu.brand_string", out->cpu, sizeof(out->cpu));
	out->cores_physical = (int)sysctl_int("hw.physicalcpu");
	out->cores_logical = (int)sysctl_int("hw.logicalcpu");
	out->ram_gb = (int)((sysctl_int("hw.memsize") + (512LL << 20)) >> 30);
}

#else
#include <unistd.h>

void fdr_sysinfo_get(struct fdr_sysinfo *out)
{
	memset(out, 0, sizeof(*out));
	snprintf(out->os, sizeof(out->os), "unknown");
	snprintf(out->os_version, sizeof(out->os_version), "unknown");
	snprintf(out->arch, sizeof(out->arch), "unknown");
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	out->cores_logical = out->cores_physical = n > 0 ? (int)n : 0;
}
#endif
