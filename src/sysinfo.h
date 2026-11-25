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
// The computer, for the debug report: OS and version, CPU architecture (and whether this build
// runs translated: Rosetta on a Mac, x64 emulation on Windows on Arm), CPU, cores, memory.
// Windows and macOS; no libobs.
#pragma once

struct fdr_sysinfo {
	char os[32];          // "Windows 11", "Windows 10", "macOS"
	char os_version[64];  // "10.0.26100.2314 (24H2)", "15.1 (24B83)"
	char arch[64];        // "x86_64", "arm64", "x86_64 (Rosetta on Apple Silicon)", "x86_64 (emulated on ARM64)"
	char cpu[96];         // the CPU's brand string, or ""
	int cores_physical, cores_logical;
	int ram_gb;           // rounded
};

void fdr_sysinfo_get(struct fdr_sysinfo *out);
