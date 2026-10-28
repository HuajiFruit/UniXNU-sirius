// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * Weak fallbacks for the sirius legacy display integration.
 */
#include <stdbool.h>
#include <stdint.h>

__attribute__((weak)) int
boot_progress_pulse_anchor(uintptr_t base, uint64_t bytes, unsigned width,
                           unsigned height, unsigned row_bytes)
{
  (void)base;
  (void)bytes;
  (void)width;
  (void)height;
  (void)row_bytes;
  return 0;
}

__attribute__((weak)) int
boot_progress_prepare_scanout(void)
{
  return 1;
}

__attribute__((weak)) void
boot_progress_cancel_scanout(void)
{
}

__attribute__((weak)) void
boot_progress_stage(const char *text)
{
  (void)text;
}

__attribute__((weak)) volatile uint32_t native_ufs_dpu_state;

__attribute__((weak)) uint32_t
native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count)
{
  (void)slot;
  (void)pages;
  (void)count;
  return 0;
}
