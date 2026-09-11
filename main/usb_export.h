/* 녹음을 USB 드라이브로 내보내기.
 *
 *   평소          : USB-C = COM 포트(USB-Serial/JTAG). 굽기·로그 그대로.
 *   "내보내기"    : 배지가 읽기 전용 USB 드라이브로 다시 잡힌다.
 *   "끝"/안전제거 : COM 으로 되돌아온다.
 *
 * 🚨 S3 에서 USB-Serial/JTAG 과 USB-OTG 는 **같은 두 핀을 나눠 쓰는 별개
 * 주변장치**다. 동시에 못 쓴다. 그래서 이 기능은 "모드" 다. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── 가짜 FAT (usb_export.c) ──────────────────────────────── */
void     usb_export_build(void);                  /* 지금 있는 녹음으로 판을 짠다 */
bool     usb_export_read(uint32_t lba, uint8_t *out);
uint32_t usb_export_sectors(void);
uint32_t usb_export_sector_size(void);
int      usb_export_files(void);                  /* 판에 올라간 파일 수 */

/* ── 모드 (usb_msc.c) ─────────────────────────────────────── */
bool usb_msc_start(void);      /* COM → 드라이브. 실패하면 false */
void usb_msc_stop(void);       /* 드라이브 → COM (아래 🚨 참고) */
bool usb_msc_active(void);
bool usb_msc_mounted(void);    /* 호스트가 실제로 붙어 읽고 있나 */
bool usb_msc_ejected(void);    /* 호스트가 "안전하게 제거" 했나 */
