/* Copy this to secrets.h and fill it in. secrets.h is never committed;
 * tools/setup.sh copies this file for you on first run.
 *
 * You can leave all of it empty. WiFi can be typed on the badge itself
 * (Settings -> WiFi), which is the intended way — a badge that needs a phone
 * or a PC to be set up is not much of a badge. These are only a head start
 * for a board you flash often.
 *
 * 🚨 Leave a field EMPTY rather than putting a placeholder in it. Anything
 * you write here is copied onto the badge at boot, so a line like
 *   #define BADGE_WIFI_SSID "your-ssid-here"
 * makes the badge chase a network called "your-ssid-here" AND overwrites the
 * credentials you typed on the device. Empty means "leave what is there".
 */
#pragma once

/* Up to three networks. Order does not matter: the badge scans first and
 * joins whichever of these is actually in range with the strongest signal. */
#define BADGE_WIFI_SSID  ""
#define BADGE_WIFI_PASS  ""
#define BADGE_WIFI_SSID2 ""
#define BADGE_WIFI_PASS2 ""
#define BADGE_WIFI_SSID3 ""
#define BADGE_WIFI_PASS3 ""
