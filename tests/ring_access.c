/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors */

/* Sequentially consistent loads and stores of a ring's latest and reading fields, for the fake
 * launcher (protocol.md "Frame ring"), which Python cannot do on shared memory by itself. */

#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#define EXPORT __declspec(dllexport)

EXPORT uint32_t ring_load(volatile uint32_t* at) { return (uint32_t)InterlockedCompareExchange((volatile LONG*)at, 0, 0); }
EXPORT void ring_store(volatile uint32_t* at, uint32_t value) { InterlockedExchange((volatile LONG*)at, (LONG)value); }
#else
#define EXPORT __attribute__((visibility("default")))

EXPORT uint32_t ring_load(uint32_t* at) { return __atomic_load_n(at, __ATOMIC_SEQ_CST); }
EXPORT void ring_store(uint32_t* at, uint32_t value) { __atomic_store_n(at, value, __ATOMIC_SEQ_CST); }
#endif
