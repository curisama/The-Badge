/* TinyUSB 설정 — 이 배지는 MSC(USB 저장장치) 하나만 쓴다.
 *
 * 🚨 이 파일은 **tinyusb 컴포넌트가 스스로 찾아 읽는다**(tusb_option.h 안에서
 * `#include "tusb_config.h"`). 그런데 그 컴포넌트의 CMakeLists 는 우리 쪽
 * 경로를 모른다. 그래서 최상위 CMakeLists.txt 에서 이 디렉터리를 그 컴포넌트의
 * 포함 경로에 끼워 넣는다. 파일만 만들어 두면 안 잡힌다.
 *
 * 🚨 CFG_TUSB_MCU 는 여기서 정의하지 않는다 — 컴포넌트가 컴파일 옵션으로
 * 이미 준다(-DCFG_TUSB_MCU=OPT_MCU_ESP32S3). 겹치면 재정의 경고가 난다. */
#pragma once

#define CFG_TUSB_OS               OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_DEBUG            0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE    64

/* 쓰는 것 하나, 나머지는 전부 끈다. 안 끄면 그 클래스의 콜백이 없다고
 * 링크에서 터지거나, 쓰지도 않는 버퍼가 램을 먹는다. */
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

/* 한 섹터(512B)를 한 번에 넘긴다. 우리 판이 섹터 단위로 지어지니 딱 맞다. */
#define CFG_TUD_MSC_EP_BUFSIZE    512
