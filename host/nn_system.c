/* The small system services the game asks for: nn::time (the host's clock and time zone), nn::oe (the application's
   environment: language, mode, focus), nn::account (one local user), nn::vi (the display and its vsync event) and
   nn::pl (system shared fonts: a TrueType font of the host's for every type) and
   nn::ngc (the profanity filter: nothing masked).

   Struct layouts are the SDK's (the game reads them):
     PosixTime {s64}; CalendarTime {s16 year; s8 month, day, hour, minute, second; pad}: 8 bytes, returned in x0;
     CalendarAdditionalInfo {s32 dayOfWeek, dayOfYear; TimeZone {char name[8]; bool isDst; s32 utcOffsetSeconds}};
     SteadyClockTimePoint {s64 seconds; Uuid source}; LocationName char[0x24]; Uid {u64, u64};
     UserHandle {u64 [3]} (ours: the uid, then a magic); Nickname char[33]; LanguageCode char[8], returned in x0.
   HOS_TIME_OFFSET=<seconds>: added to the clock (the game's time travel, without changing the PC's clock). */
#include "hos.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t clock_offset(void) {
    static int64_t off = -1;
    static int done;
    if (!done) {
        const char *s = getenv("HOS_TIME_OFFSET");
        off = s ? strtoll(s, NULL, 10) : 0;
        done = 1;
    }
    return off;
}
static int64_t now_posix(void) { return (int64_t)time(NULL) + clock_offset(); }

/* ---- nn::time ------------------------------------------------------------------------------------------------- */
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
_Static_assert(sizeof(CalendarTime) == 8, "CalendarTime");

static CalendarTime calendar_of(const struct tm *t) {
    CalendarTime c = {(int16_t)(t->tm_year + 1900), (int8_t)(t->tm_mon + 1), (int8_t)t->tm_mday, (int8_t)t->tm_hour,
                      (int8_t)t->tm_min, (int8_t)t->tm_sec, 0};
    return c;
}
static struct tm tm_of(const CalendarTime *c) {
    struct tm t = {0};
    t.tm_year = c->year - 1900, t.tm_mon = c->month - 1, t.tm_mday = c->day;
    t.tm_hour = c->hour, t.tm_min = c->minute, t.tm_sec = c->second;
    t.tm_isdst = -1;
    return t;
}
static uint64_t ret_calendar(CalendarTime c) {
    uint64_t v;
    memcpy(&v, &c, 8);
    return v;
}
/* (calendar, additional info) of a POSIX time in local time (local: the device's zone) or UTC */
static void to_calendar(int64_t posix, int local, uint64_t cal, uint64_t info) {
    const time_t tt = (time_t)posix;
    struct tm t;
    if (local) localtime_r(&tt, &t);
    else gmtime_r(&tt, &t);
    if (cal) {
        const CalendarTime c = calendar_of(&t);
        memcpy(GP(cal), &c, 8);
    }
    if (info) {
        uint8_t *p = GP(info);
        memset(p, 0, 0x18);
        const int32_t dow = t.tm_wday, doy = t.tm_yday, off = local ? (int32_t)t.tm_gmtoff : 0;
        memcpy(p, &dow, 4);
        memcpy(p + 4, &doy, 4);
        strncpy((char *)p + 8, local && t.tm_zone ? t.tm_zone : "UTC", 7);
        p[0x10] = local && t.tm_isdst > 0;
        memcpy(p + 0x14, &off, 4);
    }
}
HLE(_ZN2nn4time10InitializeEv) { RET(0); }
HLE(_ZN2nn4time13IsInitializedEv) { RET(1); }
HLE(_ZN2nn4time8FinalizeEv) { RET(0); }
static int valid_date(int y, int m, int d) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1) return 0;
    const int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d <= days[m - 1] + (m == 2 && leap);
}
HLE(_ZN2nn4time11IsValidDateEiii) { RET(valid_date((int)ARG(0), (int)ARG(1), (int)ARG(2))); }
HLE(_ZN2nn4time12GetDayOfWeekEiii) {
    struct tm t = {0};
    t.tm_year = (int)ARG(0) - 1900, t.tm_mon = (int)ARG(1) - 1, t.tm_mday = (int)ARG(2), t.tm_hour = 12;
    timegm(&t);
    RET(t.tm_wday);
}
static void current_time(Ctx *c) {
    if (ARG(0)) G64(ARG(0)) = (uint64_t)now_posix();
    RET(0);
}
HLE(_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE) { current_time(c); }
HLE(_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE) { current_time(c); }
HLE(_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE) { current_time(c); }
HLE(_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv) { RET(1); }
HLE(_ZN2nn4time25AdjustableUserSystemClock6AdjustEv) { RET(0); }
/* ToCalendarTime(CalendarTime*, CalendarAdditionalInfo*, PosixTime const&[, TimeZoneRule const&]): the device's zone */
HLE(_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE) {
    to_calendar((int64_t)G64(ARG(2)), 1, ARG(0), ARG(1));
    RET(0);
}
HLE(_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE) {
    to_calendar((int64_t)G64(ARG(2)), 1, ARG(0), ARG(1));
    RET(0);
}
HLE(_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE) {
    const time_t tt = (time_t)(int64_t)G64(ARG(0));
    struct tm t;
    gmtime_r(&tt, &t);
    RET(ret_calendar(calendar_of(&t)));
}
HLE(_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE) {
    CalendarTime cal;
    memcpy(&cal, GP(ARG(0)), 8);
    struct tm t = tm_of(&cal);
    RET((uint64_t)(int64_t)timegm(&t));
}
/* ToPosixTime(int* count, PosixTime* out, int capacity, CalendarTime const&, TimeZoneRule const&): local time */
HLE(_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE) {
    CalendarTime cal;
    memcpy(&cal, GP(ARG(3)), 8);
    struct tm t = tm_of(&cal);
    const int64_t p = (int64_t)mktime(&t);
    if (ARG(0)) G32(ARG(0)) = (int)ARG(2) > 0;
    if ((int)ARG(2) > 0 && ARG(1)) G64(ARG(1)) = (uint64_t)p;
    RET(0);
}
HLE(_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE) { RET(0); }
HLE(_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE) {
    memset(GP(ARG(0)), 0, 0x24);
    const char *tz = getenv("TZ");
    strncpy((char *)GP(ARG(0)), tz && *tz ? tz : "UTC", 0x23);
}
static int cal_cmp(uint64_t a, uint64_t b) {
    CalendarTime x, y;
    memcpy(&x, GP(a), 8);
    memcpy(&y, GP(b), 8);
    const int64_t kx = ((((int64_t)x.year * 12 + x.month) * 31 + x.day) * 24 + x.hour) * 3600 + x.minute * 60 + x.second;
    const int64_t ky = ((((int64_t)y.year * 12 + y.month) * 31 + y.day) * 24 + y.hour) * 3600 + y.minute * 60 + y.second;
    return kx < ky ? -1 : kx > ky;
}
HLE(_ZN2nn4timeleERKNS0_12CalendarTimeES3_) { RET(cal_cmp(ARG(0), ARG(1)) <= 0); }
HLE(_ZN2nn4timeltERKNS0_12CalendarTimeES3_) { RET(cal_cmp(ARG(0), ARG(1)) < 0); }
/* operator-(CalendarTime const&, CalendarTime const&) -> TimeSpan (nanoseconds) */
HLE(_ZN2nn4timemiERKNS0_12CalendarTimeES3_) {
    CalendarTime x, y;
    memcpy(&x, GP(ARG(0)), 8);
    memcpy(&y, GP(ARG(1)), 8);
    struct tm a = tm_of(&x), b = tm_of(&y);
    RET((uint64_t)(((int64_t)timegm(&a) - (int64_t)timegm(&b)) * 1000000000LL));
}
/* the steady clock: seconds since the program started, from one source (a fixed id) */
static const uint8_t steady_source[16] = {0x6f, 0x72, 0x61, 0x6e, 0x67, 0x65, 0x73, 0x2d,
                                          0x73, 0x74, 0x65, 0x61, 0x64, 0x79, 0x00, 0x01};
HLE(_ZN2nn4time38GetStandardSteadyClockCurrentTimePointEPNS0_20SteadyClockTimePointE) {
    G64(ARG(0)) = (uint64_t)(hos_now_ns() / 1000000000LL);
    memcpy(GP(ARG(0) + 8), steady_source, 16);
    RET(0);
}
HLE(_ZN2nn4time24GetElapsedSecondsBetweenEPlRKNS0_20SteadyClockTimePointES4_) {
    if (memcmp(GP(ARG(1) + 8), GP(ARG(2) + 8), 16)) { RET(HOS_RESULT(116, 201)); return; }   /* (other sources) */
    G64(ARG(0)) = G64(ARG(2)) - G64(ARG(1));
    RET(0);
}
/* ClockSnapshot: an opaque 0xD0-byte record; ours holds the POSIX time at +0 (Longshot's) */
HLE(_ZN2nn4time13ClockSnapshotC1Ev) { memset(GP(ARG(0)), 0, 0xD0); }
HLE(_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_) {
    memset(GP(ARG(0)), 0, 0xD0);
    G64(ARG(0)) = (uint64_t)now_posix();
    RET(0);
}
HLE(_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_) { RET(0); }

/* ---- nn::oe --------------------------------------------------------------------------------------------------- */
HLE(_ZN2nn2oe10InitializeEv) { (void)c; }
/* LanguageCode "en-US" (HOS_LANGUAGE=<code> another, e.g. ja, fr, de, es, it, nl, ru, zh-Hans, ko) */
HLE(_ZN2nn2oe18GetDesiredLanguageEv) {
    char code[8] = {0};
    const char *l = getenv("HOS_LANGUAGE");
    strncpy(code, l && *l ? l : "en-US", 8);
    uint64_t v;
    memcpy(&v, code, 8);
    RET(v);
}
HLE(_ZN2nn2oe13GetRegionCodeEv) { RET(1); }          /* RegionCode_Usa */
HLE(_ZN2nn2oe16GetOperationModeEv) { RET(1); }       /* OperationMode_Console: docked, 1080p */
HLE(_ZN2nn2oe18GetPerformanceModeEv) { RET(1); }     /* PerformanceMode_Boost (docked) */
HLE(_ZN2nn2oe20GetCurrentFocusStateEv) { RET(1); }   /* FocusState_InFocus */
HLE(_ZN2nn2oe25TryPopNotificationMessageEPj) { RET(0); }
HLE(_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_) {
    const float one = 1.0f;
    if (ARG(0)) memcpy(GP(ARG(0)), &one, 4);
    if (ARG(1)) memcpy(GP(ARG(1)), &one, 4);
}
HLE(_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE) {
    static const uint8_t id[16] = {0x6f, 0x72, 0x61, 0x6e, 0x67, 0x65, 0x73, 0x2d, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x00, 0x01};
    memcpy(GP(ARG(0)), id, 16);
}
HLE(_ZN2nn2oe25GetProgramTotalActiveTimeEv) { RET((uint64_t)hos_now_ns()); }
HLE(_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv) { RET(0); }

/* ---- nn::account: one local user, with no network service account ------------------------------------------- */
static const uint64_t user_uid[2] = {0x6f72616e67657300ULL, 0x0000000000000001ULL};
#define HANDLE_MAGIC 0x55534552484e444cULL   /* "LDNHRESU" */
static void put_user_handle(uint64_t h) {
    G64(h) = user_uid[0], G64(h + 8) = user_uid[1], G64(h + 16) = HANDLE_MAGIC;
}
#define ACCOUNT_RESULT(desc) HOS_RESULT(124, desc)
HLE(_ZN2nn7account10InitializeEv) { (void)c; }
static void list_users(Ctx *c) {
    const int n = (int)ARG(2) > 0 ? 1 : 0;
    if (n && ARG(1)) G64(ARG(1)) = user_uid[0], G64(ARG(1) + 8) = user_uid[1];
    if (ARG(0)) G32(ARG(0)) = (uint32_t)n;
    RET(0);
}
HLE(_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi) { list_users(c); }
HLE(_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi) { list_users(c); }
HLE(_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE) {
    *(uint8_t *)GP(ARG(0)) = G64(ARG(1)) == user_uid[0] && G64(ARG(1) + 8) == user_uid[1];
    RET(0);
}
HLE(_ZN2nn7account20PromoteQualificationERKNS0_3UidE) { RET(0); }
HLE(_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE) {
    put_user_handle(ARG(0));
    RET(1);
}
HLE(_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE) {
    put_user_handle(ARG(0));
    RET(0);
}
HLE(_ZN2nn7account9CloseUserERKNS0_10UserHandleE) { (void)c; }
HLE(_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE) {
    G64(ARG(0)) = G64(ARG(1)), G64(ARG(0) + 8) = G64(ARG(1) + 8);
    RET(0);
}
/* Nickname: char[33] (HOS_USER_NAME=<name>, else "Player") */
HLE(_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE) {
    const char *name = getenv("HOS_USER_NAME");
    memset(GP(ARG(0)), 0, 33);
    strncpy((char *)GP(ARG(0)), name && *name ? name : "Player", 32);
    RET(0);
}
HLE(_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE) {   /* (no image: an empty one, and the account's "not found") */
    if (ARG(0)) G64(ARG(0)) = 0;
    RET(ACCOUNT_RESULT(15));
}
HLE(_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE) {
    *(uint8_t *)GP(ARG(0)) = 0;
    RET(0);
}
HLE(_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE) { RET(ACCOUNT_RESULT(280)); }
HLE(_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE) {
    RET(ACCOUNT_RESULT(280));
}
HLE(_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE) {
    if (ARG(0)) G64(ARG(0)) = 0;
    RET(ACCOUNT_RESULT(280));
}
HLE(_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE) { RET(0); }
/* AsyncContext: done at once, with the network account's failure */
HLE(_ZN2nn7account12AsyncContextC1Ev) { memset(GP(ARG(0)), 0, 0x10); }
HLE(_ZN2nn7account12AsyncContextD1Ev) { (void)c; }
HLE(_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE) { RET(0); }
HLE(_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE) {
    hos_kernel_event_signal(hos_system_event_init(c, ARG(1), 0));
    RET(0);
}
HLE(_ZN2nn7account12AsyncContext7HasDoneEPb) {
    *(uint8_t *)GP(ARG(1)) = 1;
    RET(0);
}
HLE(_ZN2nn7account12AsyncContext9GetResultEv) { RET(ACCOUNT_RESULT(280)); }
HLE(_ZN2nn7account12AsyncContext6CancelEv) { RET(0); }

/* ---- nn::vi: one display and layer (objects of no content), and the display's vsync at 60 Hz ------------------ */
static uint64_t vi_display, vi_layer, vi_window;
static uint32_t vsync_event;
static void *vsync_main(void *p) {
    (void)p;
    int64_t next = hos_now_ns();
    for (;;) {
        next += 16666667;
        const int64_t d = next - hos_now_ns();
        if (d > 0) {
            struct timespec ts = {(time_t)(d / 1000000000LL), (long)(d % 1000000000LL)};
            while (nanosleep(&ts, &ts)) {}
        } else if (d < -1000000000LL) {
            next = hos_now_ns();   /* (a stop in the debugger: no catching up) */
        }
        hos_kernel_event_signal(vsync_event);
    }
    return NULL;
}
HLE(_ZN2nn2vi10InitializeEv) { RET(0); }
HLE(_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE) {
    if (!vi_display) vi_display = hos_sys_alloc(0x1000, 0x1000);
    G64(ARG(0)) = vi_display;
    RET(0);
}
HLE(_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE) {
    if (!vi_layer) vi_layer = hos_sys_alloc(0x1000, 0x1000);
    G64(ARG(0)) = vi_layer;
    RET(0);
}
HLE(_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE) {
    if (!vi_window) vi_window = hos_sys_alloc(0x1000, 0x1000);
    G64(ARG(0)) = vi_window;
    RET(0);
}
HLE(_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE) {
    vsync_event = hos_system_event_init(c, ARG(0), 1);
    static pthread_t t;
    if (!t) pthread_create(&t, NULL, vsync_main, NULL);
    RET(0);
}

/* ---- nn::pl: system shared fonts -------------------------------------------------------------------------------
   The console's are system data we do not have. Every font type is one TrueType font of the host's instead
   (HOS_SHARED_FONT=<file.ttf>, else the first of a few common ones), handed out as the console's pl service hands out
   its fonts: plain TrueType data (the system decrypts the BFTTF files into its shared memory). The font has to be
   TrueType outlines (glyf), which the SDK's font engine reads; without one the game's glyph lookups fail. */
static uint64_t font_addr, font_size;
static void font_load(void) {
    static int done;
    if (done++) return;
    static const char *const candidates[] = {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "C:/Windows/Fonts/arial.ttf"};
    const char *env = getenv("HOS_SHARED_FONT");
    for (size_t k = env && *env ? 0 : 1; k <= sizeof candidates / sizeof *candidates; k++) {
        const char *path = k ? candidates[k - 1] : env;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (32L << 20)) {
            font_addr = hos_sys_alloc(((uint64_t)n + 0xFFF) & ~0xFFFULL, 0x1000);
            font_size = fread(GP(font_addr), 1, (size_t)n, f) == (size_t)n ? (uint64_t)n : 0;
        }
        fclose(f);
        if (font_size) {
            hos_log("pl: shared fonts: %s (%llu KB)", path, (unsigned long long)(font_size >> 10));
            return;
        }
    }
    font_addr = hos_sys_alloc(0x1000, 0x1000);
    hos_log("pl: no TrueType font found for the system's shared fonts (HOS_SHARED_FONT=<file.ttf>): text will fail");
}
HLE(_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE) { font_load(); }
HLE(_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE) { RET(1); }   /* SharedFontLoadState_Loaded */
HLE(_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE) {
    font_load();
    RET(font_addr);
}
HLE(_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE) {
    font_load();
    RET(font_size);
}
/* GetSharedFont(int* count, SharedFontData* out, int capacity, LanguageCode): the one font, as type Standard.
   SharedFontData: {SharedFontType type; const void* address; size_t size} */
HLE(_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE) {
    font_load();
    const int n = (int)ARG(2) > 0 && font_size ? 1 : 0;
    if (n && ARG(1)) G64(ARG(1)) = 0, G64(ARG(1) + 8) = font_addr, G64(ARG(1) + 16) = font_size;
    if (ARG(0)) G32(ARG(0)) = (uint32_t)n;
    RET(0);
}

/* ---- nn::ngc: the profanity filter (its service, ngct, is not emulated): nothing is masked ----------------------- */
HLE(_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb) { RET(0); }
HLE(_ZN2nn3ngc15ProfanityFilterD1Ev) { (void)c; }
HLE(_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE) { RET(0); }
/* MaskProfanityWordsInText(int* masked, char16_t* text, unsigned int) -> Result */
HLE(_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj) {
    if (ARG(1)) G32(ARG(1)) = 0;
    RET(0);
}
