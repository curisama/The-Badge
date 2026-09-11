/* TinyUSB configuration — this badge uses only MSC (USB storage).
 *
 * 🚨 This file is **found and read by the tinyusb component itself** (from
 * `#include "tusb_config.h"` inside tusb_option.h). But that component's
 * CMakeLists knows nothing about our paths, so the top-level CMakeLists.txt
 * pushes this directory into that component's include path. Just creating the
 * file is not enough.
 *
 * 🚨 CFG_TUSB_MCU is not defined here — the component already supplies it as a
 * compile option (-DCFG_TUSB_MCU=OPT_MCU_ESP32S3). Defining it again warns
 * about redefinition. */
#pragma once

#define CFG_TUSB_OS               OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_DEBUG            0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE    64

/* One class in use, everything else off. Left on, the linker blows up over a
 * missing callback for that class, or unused buffers eat RAM. */
#define CFG_TUD_MSC               1
#define CFG_TUD_CDC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_AUDIO             0
#define CFG_TUD_VIDEO             0
#define CFG_TUD_VENDOR            0
#define CFG_TUD_DFU               0
#define CFG_TUD_DFU_RUNTIME       0
#define CFG_TUD_ECM_RNDIS         0
#define CFG_TUD_NCM               0
#define CFG_TUD_BTH               0

/* One sector (512 B) at a time. Our volume is built in sectors, so it fits exactly. */
#define CFG_TUD_MSC_EP_BUFSIZE    512
