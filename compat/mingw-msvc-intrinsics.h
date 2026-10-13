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
// MinGW-only shim: libobs's Windows headers (util/threading-windows.h) use MSVC intrinsics that
// GCC doesn't provide. Release builds use MSVC and never include this file.
#pragma once
#if defined(__MINGW32__) && !defined(_MSC_VER)
static inline int __iso_volatile_load32(const volatile int *p)
{
	return __atomic_load_n(p, __ATOMIC_RELAXED);
}
static inline char __iso_volatile_load8(const volatile char *p)
{
	return __atomic_load_n(p, __ATOMIC_RELAXED);
}
static inline char _InterlockedExchange8(volatile char *p, char v)
{
	return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
#endif
