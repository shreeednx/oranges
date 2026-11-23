/* The controls: which keys and gamepad input make each Switch button and stick direction (Npad No.1, a Pro
   Controller). The settings menu's Controls tab changes them (hos_menu.c); hos_window.c reads the keyboard and the
   gamepad through them (hos_controls_eval). HOS_INPUT_SCRIPT and HOS_CONTROL don't go through them: they name the
   Switch's buttons.
   The defaults are the fixed map from before the menu, a gamepad's buttons by position as on a Nintendo pad (the
   right face button is A, the bottom one B, the top one X, the left one Y):
       left stick W A S D        right stick I J K L       D-pad arrow keys
       A Space / Enter           B Left Shift / Backspace  X E          Y F
       L Q   R R   ZL Z   ZR C   + Escape   - Tab          stick clicks X (L), N (R)
   A gamepad's sticks are its sticks, whatever is bound (a key or a gamepad button bound to a stick direction pushes
   that stick all the way).
   Built alone by tests/test_host_options.py, without SDL (key names are then numbers). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hos_menu.h"
#include "hos_options.h"
#ifdef HOS_HAVE_SDL3
#include <SDL3/SDL.h>
_Static_assert(SDL_GAMEPAD_BUTTON_COUNT == HOS_PAD_BUTTONS, "the gamepad inputs' numbering (hos_menu.h)");
#endif

void hos_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* (HOS_BTN_* of hos.h, by bit number, in HOS_CTL_* order) */
static const struct { const char *id, *name; int bit; } controls[HOS_CTL_N] = {
    {"A", "A", 0},   {"B", "B", 1},   {"X", "X", 2},   {"Y", "Y", 3},
    {"L", "L", 6},   {"R", "R", 7},   {"ZL", "ZL", 8}, {"ZR", "ZR", 9},
    {"PLUS", "+ (Plus)", 10}, {"MINUS", "- (Minus)", 11},
    {"LS", "Left stick click", 4}, {"RS", "Right stick click", 5},
    {"UP", "D-pad up", 13}, {"DOWN", "D-pad down", 15}, {"LEFT", "D-pad left", 12}, {"RIGHT", "D-pad right", 14},
    {"LSTICK_UP", "Left stick up", 17}, {"LSTICK_DOWN", "Left stick down", 19},
    {"LSTICK_LEFT", "Left stick left", 16}, {"LSTICK_RIGHT", "Left stick right", 18},
    {"RSTICK_UP", "Right stick up", 21}, {"RSTICK_DOWN", "Right stick down", 23},
    {"RSTICK_LEFT", "Right stick left", 20}, {"RSTICK_RIGHT", "Right stick right", 22},
};

/* SDL scancodes (USB HID usage ids) */
enum { K_A = 4, K_C = 6, K_D = 7, K_E = 8, K_F = 9, K_I = 12, K_J = 13, K_K = 14, K_L = 15, K_N = 17, K_Q = 20,
       K_R = 21, K_S = 22, K_W = 26, K_X = 27, K_Z = 29, K_RETURN = 40, K_ESCAPE = 41, K_BACKSPACE = 42, K_TAB = 43,
       K_SPACE = 44, K_RIGHT = 79, K_LEFT = 80, K_DOWN = 81, K_UP = 82, K_LSHIFT = 225 };
/* SDL_GamepadButton */
enum { P_SOUTH, P_EAST, P_WEST, P_NORTH, P_BACK, P_GUIDE, P_START, P_LSTICK, P_RSTICK, P_LSHOULDER, P_RSHOULDER,
       P_DUP, P_DDOWN, P_DLEFT, P_DRIGHT };
static const HosBinding defaults[HOS_CTL_N] = {
    [HOS_CTL_A] = {{K_SPACE, K_RETURN}, P_EAST},      [HOS_CTL_B] = {{K_LSHIFT, K_BACKSPACE}, P_SOUTH},
    [HOS_CTL_X] = {{K_E, 0}, P_NORTH},                [HOS_CTL_Y] = {{K_F, 0}, P_WEST},
    [HOS_CTL_L] = {{K_Q, 0}, P_LSHOULDER},            [HOS_CTL_R] = {{K_R, 0}, P_RSHOULDER},
    [HOS_CTL_ZL] = {{K_Z, 0}, HOS_PAD_LT},            [HOS_CTL_ZR] = {{K_C, 0}, HOS_PAD_RT},
    [HOS_CTL_PLUS] = {{K_ESCAPE, 0}, P_START},        [HOS_CTL_MINUS] = {{K_TAB, 0}, P_BACK},
    [HOS_CTL_LS] = {{K_X, 0}, P_LSTICK},              [HOS_CTL_RS] = {{K_N, 0}, P_RSTICK},
    [HOS_CTL_UP] = {{K_UP, 0}, P_DUP},                [HOS_CTL_DOWN] = {{K_DOWN, 0}, P_DDOWN},
    [HOS_CTL_LEFT] = {{K_LEFT, 0}, P_DLEFT},          [HOS_CTL_RIGHT] = {{K_RIGHT, 0}, P_DRIGHT},
    [HOS_CTL_LSTICK_UP] = {{K_W, 0}, -1},             [HOS_CTL_LSTICK_DOWN] = {{K_S, 0}, -1},
    [HOS_CTL_LSTICK_LEFT] = {{K_A, 0}, -1},           [HOS_CTL_LSTICK_RIGHT] = {{K_D, 0}, -1},
    [HOS_CTL_RSTICK_UP] = {{K_I, 0}, -1},             [HOS_CTL_RSTICK_DOWN] = {{K_K, 0}, -1},
    [HOS_CTL_RSTICK_LEFT] = {{K_J, 0}, -1},           [HOS_CTL_RSTICK_RIGHT] = {{K_L, 0}, -1},
};
/* the gamepad inputs' names: in the options file, and shown */
static const struct { const char *id, *name; } pad_names[HOS_PAD_N] = {
    {"south", "Bottom button"}, {"east", "Right button"}, {"west", "Left button"}, {"north", "Top button"},
    {"back", "Back / Select"}, {"guide", "Home / Guide"}, {"start", "Start"}, {"leftstick", "Left stick click"},
    {"rightstick", "Right stick click"}, {"leftshoulder", "LB / L1"}, {"rightshoulder", "RB / R1"},
    {"dpup", "D-pad up"}, {"dpdown", "D-pad down"}, {"dpleft", "D-pad left"}, {"dpright", "D-pad right"},
    {"misc1", "Share / Capture"}, {"paddle1", "Right paddle 1"}, {"paddle2", "Left paddle 1"},
    {"paddle3", "Right paddle 2"}, {"paddle4", "Left paddle 2"}, {"touchpad", "Touchpad"}, {"misc2", "Misc 2"},
    {"misc3", "Misc 3"}, {"misc4", "Misc 4"}, {"misc5", "Misc 5"}, {"misc6", "Misc 6"},
    {"lefttrigger", "LT / L2"}, {"righttrigger", "RT / R2"},
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static HosBinding table[HOS_CTL_N];
static int loaded;

const char *hos_control_name(int ctl) { return ctl >= 0 && ctl < HOS_CTL_N ? controls[ctl].name : ""; }
const char *hos_control_id(int ctl) { return ctl >= 0 && ctl < HOS_CTL_N ? controls[ctl].id : ""; }
uint64_t hos_control_bit(int ctl) { return ctl >= 0 && ctl < HOS_CTL_N ? 1ull << controls[ctl].bit : 0; }
HosBinding hos_binding_default(int ctl) {
    const HosBinding none = {{0, 0}, -1};
    return ctl >= 0 && ctl < HOS_CTL_N ? defaults[ctl] : none;
}

const char *hos_key_name(int sc, char *buf, size_t size) {
    if (!size) return buf;
    buf[0] = 0;
    if (sc <= 0) return buf;
#ifdef HOS_HAVE_SDL3
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    if (n && *n) {
        snprintf(buf, size, "%s", n);
        return buf;
    }
#endif
    snprintf(buf, size, "#%d", sc);
    return buf;
}
static int key_from_name(const char *s) {
    while (*s == ' ') s++;
    if (!*s) return 0;
    if (*s == '#') return atoi(s + 1);
#ifdef HOS_HAVE_SDL3
    const SDL_Scancode sc = SDL_GetScancodeFromName(s);
    if (sc != SDL_SCANCODE_UNKNOWN) return (int)sc;
#endif
    hos_log("options: \"%s\" is not a key", s);
    return 0;
}
const char *hos_pad_name(int input) { return input >= 0 && input < HOS_PAD_N ? pad_names[input].name : ""; }
static int pad_from_name(const char *s) {
    while (*s == ' ') s++;
    if (!*s) return -1;
    for (int i = 0; i < HOS_PAD_N; i++)
        if (!strcmp(s, pad_names[i].id)) return i;
    hos_log("options: \"%s\" is not a gamepad input", s);
    return -1;
}

static void load_locked(void) {
    if (loaded) return;
    loaded = 1;
    memcpy(table, defaults, sizeof table);
    char key[64], v[256];
    for (int c = 0; c < HOS_CTL_N; c++) {
        snprintf(key, sizeof key, "key_%s", controls[c].id);
        if (hos_option(key, v, sizeof v)) {
            char *comma = strchr(v, ',');
            if (comma) *comma = 0;
            table[c].key[0] = key_from_name(v);
            table[c].key[1] = comma ? key_from_name(comma + 1) : 0;
        }
        snprintf(key, sizeof key, "pad_%s", controls[c].id);
        if (hos_option(key, v, sizeof v)) table[c].pad = pad_from_name(v);
    }
}
void hos_controls_load(void) {
    pthread_mutex_lock(&lock);
    load_locked();
    pthread_mutex_unlock(&lock);
}

HosBinding hos_binding(int ctl) {
    pthread_mutex_lock(&lock);
    load_locked();
    const HosBinding b = ctl >= 0 && ctl < HOS_CTL_N ? table[ctl] : hos_binding_default(-1);
    pthread_mutex_unlock(&lock);
    return b;
}
int hos_control_pad(int ctl) { return hos_binding(ctl).pad; }

/* the control's lines: none when it is the default */
static void save_locked(int c) {
    char key[64], v[256], a[64], b[64];
    snprintf(key, sizeof key, "key_%s", controls[c].id);
    const HosBinding *t = &table[c], *d = &defaults[c];
    if (t->key[0] == d->key[0] && t->key[1] == d->key[1]) {
        hos_option_unset(key);
    } else {
        hos_key_name(t->key[0], a, sizeof a), hos_key_name(t->key[1], b, sizeof b);
        snprintf(v, sizeof v, "%s%s%s", a, t->key[1] ? "," : "", b);
        hos_option_set(key, v);
    }
    snprintf(key, sizeof key, "pad_%s", controls[c].id);
    if (t->pad == d->pad) hos_option_unset(key);
    else hos_option_set(key, t->pad >= 0 ? pad_names[t->pad].id : "");
}

void hos_bind(int ctl, int slot, int input) {
    if (ctl < 0 || ctl >= HOS_CTL_N || slot < 0 || slot > HOS_BIND_KEYS) return;
    pthread_mutex_lock(&lock);
    load_locked();
    HosBinding *t = &table[ctl];
    if (slot < HOS_BIND_KEYS) {
        t->key[slot] = input > 0 ? input : 0;
        if (t->key[slot] && t->key[!slot] == t->key[slot]) t->key[!slot] = 0;   /* (one key once a row) */
        if (!t->key[0] && t->key[1]) t->key[0] = t->key[1], t->key[1] = 0;      /* (the first slot filled first) */
    } else {
        t->pad = input >= 0 && input < HOS_PAD_N ? input : -1;
    }
    save_locked(ctl);
    pthread_mutex_unlock(&lock);
}

void hos_binding_reset(int ctl) {
    if (ctl < 0 || ctl >= HOS_CTL_N) return;
    pthread_mutex_lock(&lock);
    load_locked();
    table[ctl] = defaults[ctl];
    save_locked(ctl);
    pthread_mutex_unlock(&lock);
}

void hos_bindings_reset_all(void) {
    pthread_mutex_lock(&lock);
    load_locked();
    for (int c = 0; c < HOS_CTL_N; c++) table[c] = defaults[c], save_locked(c);
    pthread_mutex_unlock(&lock);
}

int hos_binding_owner(int pad, int input, int skip_ctl, int *ctl, int *slot) {
    if (pad ? input < 0 : input <= 0) return 0;
    pthread_mutex_lock(&lock);
    load_locked();
    int found = 0;
    for (int c = 0; c < HOS_CTL_N && !found; c++) {
        if (c == skip_ctl) continue;
        if (pad && table[c].pad == input) found = 1, *ctl = c, *slot = HOS_BIND_KEYS;
        for (int s = 0; !pad && s < HOS_BIND_KEYS && !found; s++)
            if (table[c].key[s] == input) found = 1, *ctl = c, *slot = s;
    }
    pthread_mutex_unlock(&lock);
    return found;
}

uint64_t hos_controls_eval(const bool *keys, int nkeys, uint32_t pad_held, int32_t stick[4]) {
    pthread_mutex_lock(&lock);
    load_locked();
    uint64_t buttons = 0;
    for (int c = 0; c < HOS_CTL_N; c++) {
        int down = table[c].pad >= 0 && (pad_held >> table[c].pad & 1);
        for (int s = 0; s < HOS_BIND_KEYS; s++)
            down |= table[c].key[s] > 0 && table[c].key[s] < nkeys && keys[table[c].key[s]];
        if (down) buttons |= 1ull << controls[c].bit;
    }
    pthread_mutex_unlock(&lock);
    const int32_t full = 32767;
    const uint64_t b = buttons;
    /* (the stick directions as deflections; both of a pair held: none) */
    stick[0] = (int32_t)(!!(b & 1ull << 18) - !!(b & 1ull << 16)) * full;
    stick[1] = (int32_t)(!!(b & 1ull << 17) - !!(b & 1ull << 19)) * full;
    stick[2] = (int32_t)(!!(b & 1ull << 22) - !!(b & 1ull << 20)) * full;
    stick[3] = (int32_t)(!!(b & 1ull << 21) - !!(b & 1ull << 23)) * full;
    return buttons & 0xFFFF;   /* (the stick directions' bits come from the sticks: hos_input) */
}
