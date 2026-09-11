/* Exporting recordings as a USB drive.
 *
 *   normally      : USB-C = the COM port (USB-Serial/JTAG). Flashing and logs as usual.
 *   "export"      : the badge comes back as a read-only USB drive.
 *   "done"/eject  : it returns to COM.
 *
 * 🚨 On the S3, USB-Serial/JTAG and USB-OTG are **separate peripherals sharing
 * the same two pins**. They cannot both run. That is why this is a mode. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── the fake FAT (usb_export.c) ─────────────────────────── */
void     usb_export_build(void);                  /* lays out the volume from the recordings there now */
bool     usb_export_read(uint32_t lba, uint8_t *out);
uint32_t usb_export_sectors(void);
uint32_t usb_export_sector_size(void);
int      usb_export_files(void);                  /* how many files are on the volume */

/* ── the mode (usb_msc.c) ─────────────────────────────────── */
bool usb_msc_start(void);      /* COM → drive. false on failure */
void usb_msc_stop(void);       /* drive → COM (see the 🚨 below) */
bool usb_msc_active(void);
bool usb_msc_mounted(void);    /* is a host actually attached and reading? */
bool usb_msc_ejected(void);    /* has the host ejected it? */
