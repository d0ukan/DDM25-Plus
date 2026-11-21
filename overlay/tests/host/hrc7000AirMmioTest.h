// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 tylerwatt12
// Test-only observation of intermediate modem writes; arrays otherwise retain only the last register value.
#ifndef HRC7000_AIR_MMIO_TEST_H
#define HRC7000_AIR_MMIO_TEST_H
#include "modem/hrc7000/hrc7000.h"
void hrc7000_air_test_wr(hrc7000_t *d, uint32_t off, uint32_t value);
#define hrc7000_wr hrc7000_air_test_wr
#endif
