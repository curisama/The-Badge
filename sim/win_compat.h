/* 윈도우(MinGW)에서 시뮬을 지을 때만 끼워 넣는 다리. sim/build.sh 가 -include 한다.
 *
 * 🚨 여기 있는 것들은 전부 POSIX 인데 UCRT 에 없다. 없는 줄 모르고 지으면
 *    534개 다 컴파일한 뒤 링크 직전에 죽는다 — 37분을 버렸다(0909, 회사 PC). */
#pragma once
#include <time.h>

/* 🚨 localtime_s 는 이름만 비슷하고 인자 순서가 거꾸로다. 성공하면 0 을 준다. */
static inline struct tm *badge_localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : NULL;
}
#define localtime_r badge_localtime_r

static inline struct tm *badge_gmtime_r(const time_t *t, struct tm *out)
{
    return gmtime_s(out, t) == 0 ? out : NULL;
}
#define gmtime_r badge_gmtime_r
