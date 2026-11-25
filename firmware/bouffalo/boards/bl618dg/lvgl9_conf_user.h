// SPDX-License-Identifier: Apache-2.0
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
// The display task owns the LVGL instance. The SDK port handles DMA/scanout completion.
#define LV_USE_OS LV_OS_NONE

#endif
