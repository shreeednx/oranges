/* The window and the player's input (SDL3), and scripted input for test runs.

   hos_window_run runs the game: with a window, the guest's main thread goes to a host thread of its own and this one
   (the process's first: SDL wants its video calls there) pumps the window's events and draws, once a display refresh.
   Without one (a build without SDL3, HOS_NO_WINDOW=1, or no display) the game runs on this thread, as before.

   There is no renderer yet (rx_null.c draws nothing), so the window shows a status page: frames, fps, and what the
   input does. The Vulkan back end will present the game's window textures here instead.

   Input. hos_input() is the merged state nn_hid.c reads: the keyboard and the first gamepad as Npad No.1 (a Pro
   Controller) through the bindings (hos_controls.c: the defaults, a gamepad's buttons by position as on a Nintendo
   pad, are listed there), the mouse as the console's USB mouse and, while its left button is down, as one finger on
   the touch screen (the menu's Mouse as touch screen), and the keyboard as the console's USB keyboard too.
   F1 (or a gamepad's Home) opens the settings menu (hos_menu.c, drawn by host/vk): while it is open the game gets no
   input at all, and what was held when it closes stays away from the game until it is let go. F11: full screen.
   The game's vibration goes to the gamepad's motors (hos_rumble, times the menu's Vibration). The sound device
   (hos_sound.c) starts here too.
   HOS_INPUT_SCRIPT=<file>: input from a script, added to the player's: one line per change, "<seconds> <what>...",
   held from that time (since boot) until the next line. What: button names (A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT
   RIGHT LS RS), LSTICK=x,y / RSTICK=x,y (-1..1, up positive), TOUCH=x,y (1280x720), QUIT (ends the program). '#'
   starts a comment. The last line's inputs are held to the end: end a script with a line of none (a script cut
   after "265.8 A" held A down, and the game saw no press after it).
   HOS_SCREENSHOT=<file.bmp>[,seconds]: the window's picture, saved once, that long after the start (default 10).
   HOS_CONTROL=<fifo>: input while the game runs (made if missing; with a random island a fixed script goes astray).
   Lines run in order, each to its end before the next: "<seconds> [inputs...]" holds those inputs (the script's
   names) that long and releases them ("0.2 A", "3 LSTICK=0,1", "1.5": nothing), "SHOT <file.bmp>" saves the next
   presented picture (the Vulkan back end's), "DUMP" logs every thread (hos_kernel_dump), "PROFILE <s> [thread]" samples
   one (HOS_PROFILE), "QUIT" ends the program. Added to the player's and the script's. The settings menu: "MENU <what>"
   with OPEN, CLOSE, UP, DOWN, LEFT, RIGHT, OK, BACK, CLEAR, RESET, TAB (its actions), KEY <SDL key name> or PAD <SDL
   gamepad button name> (a press, as from the keyboard or the gamepad). For finding things in the
   guest's memory while it runs: "MEM <file>" writes every mapped writable page into a sparse file at its guest address
   (not a snapshot: the game runs on), "PEEK <hex address> <bytes> <file>" writes those bytes, "POKE <hex address>
   <hex bytes>" stores bytes (in memory order). */
#include "hos.h"
#include "hos_menu.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#ifdef HOS_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static HosInput player;   /* the window's (under input_lock) */
static float rumble[2];   /* the motors' amplitudes the game asks for (the window loop sends them to the gamepad) */

void hos_rumble(int motor, float amplitude) {
    if (amplitude != amplitude || amplitude < 0) amplitude = 0;   /* (NaN too) */
    __atomic_store(&rumble[motor & 1], &amplitude, __ATOMIC_RELAXED);
}

/* ---- buttons ------------------------------------------------------------------------------------------------------ */
static const struct { const char *name; uint64_t bit; } button_names[] = {
    {"A", HOS_BTN_A},         {"B", HOS_BTN_B},       {"X", HOS_BTN_X},         {"Y", HOS_BTN_Y},
    {"LS", HOS_BTN_LS},       {"RS", HOS_BTN_RS},     {"L", HOS_BTN_L},         {"R", HOS_BTN_R},
    {"ZL", HOS_BTN_ZL},       {"ZR", HOS_BTN_ZR},     {"PLUS", HOS_BTN_PLUS},   {"MINUS", HOS_BTN_MINUS},
    {"LEFT", HOS_BTN_LEFT},   {"UP", HOS_BTN_UP},     {"RIGHT", HOS_BTN_RIGHT}, {"DOWN", HOS_BTN_DOWN},
};

static int32_t clamp_stick(double v) {
    v *= 32767.0;
    return v > 32767 ? 32767 : v < -32767 ? -32767 : (int32_t)v;
}

/* ---- scripted input ----------------------------------------------------------------------------------------------- */
typedef struct {
    int64_t at_ns;
    uint64_t buttons;
    int32_t stick[4];
    int touch, quit;
    int32_t touch_x, touch_y;
} ScriptStep;
static ScriptStep *script;
static int script_len;
static void *script_quit(void *p);

/* one input of a script line (upper-cased in place): 0 when it is not one */
static int parse_input(char *tok, ScriptStep *s) {
    for (char *p = tok; *p && *p != '='; p++) *p = (char)toupper((unsigned char)*p);
    double x, y;
    if (sscanf(tok, "LSTICK=%lf,%lf", &x, &y) == 2) return s->stick[0] = clamp_stick(x), s->stick[1] = clamp_stick(y), 1;
    if (sscanf(tok, "RSTICK=%lf,%lf", &x, &y) == 2) return s->stick[2] = clamp_stick(x), s->stick[3] = clamp_stick(y), 1;
    if (sscanf(tok, "TOUCH=%lf,%lf", &x, &y) == 2) return s->touch = 1, s->touch_x = (int32_t)x, s->touch_y = (int32_t)y, 1;
    if (!strcmp(tok, "QUIT")) return s->quit = 1;
    for (size_t i = 0; i < sizeof button_names / sizeof *button_names; i++)
        if (!strcmp(tok, button_names[i].name)) return s->buttons |= button_names[i].bit, 1;
    return 0;
}

static void script_load(void) {
    const char *path = getenv("HOS_INPUT_SCRIPT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) hos_fatal(NULL, "HOS_INPUT_SCRIPT: %s cannot be read", path);
    char line[1024];
    int n = 0, cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *tok = strtok(line, " \t\r\n");
        if (!tok) continue;
        ScriptStep s = {0};
        s.at_ns = (int64_t)(strtod(tok, NULL) * 1e9);
        while ((tok = strtok(NULL, " \t\r\n")))
            if (!parse_input(tok, &s)) hos_fatal(NULL, "HOS_INPUT_SCRIPT: %s: \"%s\" is not an input", path, tok);
        if (n == cap) script = realloc(script, sizeof *script * (size_t)(cap = cap ? 2 * cap : 64));
        script[n++] = s;
    }
    fclose(f);
    script_len = n;
    hos_log("input: %d steps from %s", n, path);
    for (int i = 0; i < n; i++)
        if (script[i].quit) {
            pthread_t t;
            pthread_create(&t, NULL, script_quit, &script[i].at_ns);
            pthread_detach(t);
            break;
        }
}

/* the script's step at this time, or NULL */
static const ScriptStep *script_now(void) {
    const ScriptStep *cur = NULL;
    const int64_t now = hos_now_ns();
    for (int i = 0; i < script_len && script[i].at_ns <= now; i++) cur = &script[i];
    return cur;
}

/* the script's QUIT, on time whether or not the game reads input */
static void *script_quit(void *p) {
    const int64_t at = *(const int64_t *)p;
    for (int64_t left; (left = at - hos_now_ns()) > 0;) {
        struct timespec ts = {(time_t)(left / 1000000000LL), (long)(left % 1000000000LL)};
        nanosleep(&ts, NULL);
    }
    hos_log("input: the script ends the program");
    hos_exit(0);
}

/* ---- HOS_CONTROL: input and screenshots from a FIFO while the game runs ------------------------------------------- */
static ScriptStep live;   /* the control's input now (under input_lock) */
static pthread_mutex_t shot_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shot_cond = PTHREAD_COND_INITIALIZER;
static char shot_path[512];
static int shot_state;   /* 1: asked for, 2: taken */

int hos_control_shot(char *path, size_t size) {
    if (!__atomic_load_n(&shot_state, __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&shot_lock);
    const int asked = shot_state == 1;
    if (asked) snprintf(path, size, "%s", shot_path);
    pthread_mutex_unlock(&shot_lock);
    return asked;
}

void hos_control_shot_done(void) {
    pthread_mutex_lock(&shot_lock);
    shot_state = 2;
    pthread_cond_broadcast(&shot_cond);
    pthread_mutex_unlock(&shot_lock);
}

static void control_sleep(double seconds) {
    if (seconds <= 0) return;
    struct timespec ts = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
    nanosleep(&ts, NULL);
}

/* HOS_CONTROL's MENU <what> [name] */
static void control_menu(const char *what, const char *name) {
    static const struct { const char *name; int action; } actions[] = {
        {"UP", HOS_MENU_UP}, {"DOWN", HOS_MENU_DOWN}, {"LEFT", HOS_MENU_LEFT}, {"RIGHT", HOS_MENU_RIGHT},
        {"OK", HOS_MENU_OK}, {"BACK", HOS_MENU_BACK}, {"CLEAR", HOS_MENU_CLEAR}, {"RESET", HOS_MENU_RESET},
        {"TAB", HOS_MENU_TAB_NEXT},
    };
    if (!what) return;
    while (name && (*name == ' ' || *name == '\t')) name++;
    if (!strcasecmp(what, "OPEN") || !strcasecmp(what, "CLOSE")) {
        if (hos_menu_is_open() != !strcasecmp(what, "OPEN")) hos_menu_action(HOS_MENU_TOGGLE);
        return;
    }
    for (size_t i = 0; i < sizeof actions / sizeof *actions; i++)
        if (!strcasecmp(what, actions[i].name)) {
            hos_menu_action(actions[i].action);
            return;
        }
#ifdef HOS_HAVE_SDL3
    if (!strcasecmp(what, "KEY") && name) {
        const SDL_Scancode sc = SDL_GetScancodeFromName(name);
        if (sc != SDL_SCANCODE_UNKNOWN) {
            hos_menu_key((int)sc, 0, 0);
            return;
        }
    }
    if (!strcasecmp(what, "PAD") && name) {
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
        if (b != SDL_GAMEPAD_BUTTON_INVALID) {
            hos_menu_pad((int)b, 0);
            return;
        }
        if (!strcasecmp(name, "lefttrigger") || !strcasecmp(name, "righttrigger")) {
            hos_menu_pad(!strcasecmp(name, "lefttrigger") ? HOS_PAD_LT : HOS_PAD_RT, 0);
            return;
        }
    }
#endif
    hos_log("control: \"MENU %s%s%s\" is not a menu input", what, name ? " " : "", name ? name : "");
}

/* one line: "SHOT <path>" (the next presented picture; the line returns once it is saved, 5 s at most), "DUMP" (every
   thread), "PROFILE <s> [thread name]" (HOS_PROFILE's samples, MainThread's by default), "QUIT", or
   "<seconds> [inputs...]": the inputs held that long, then released */
static void control_line(char *line) {
    char *tok = strtok(line, " \t\r\n");
    if (!tok || *tok == '#') return;
    if (!strcasecmp(tok, "SHOT")) {
        const char *path = strtok(NULL, "\r\n");
        if (!path) return;
        while (*path == ' ' || *path == '\t') path++;
        pthread_mutex_lock(&shot_lock);
        snprintf(shot_path, sizeof shot_path, "%s", path);
        __atomic_store_n(&shot_state, 1, __ATOMIC_RELEASE);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 5;
        while (shot_state == 1)
            if (pthread_cond_timedwait(&shot_cond, &shot_lock, &until)) break;
        if (shot_state == 1) hos_log("control: no picture presented for %s", shot_path);
        shot_state = 0;
        pthread_mutex_unlock(&shot_lock);
        return;
    }
    if (!strcasecmp(tok, "DUMP")) {
        hos_kernel_dump("control: DUMP");
        return;
    }
    if (!strcasecmp(tok, "PROFILE")) {
        const char *secs = strtok(NULL, " \t\r\n"), *name = strtok(NULL, "\r\n");
        hos_kernel_profile(secs ? atof(secs) : 5, name ? name : "MainThread");
        return;
    }
    if (!strcasecmp(tok, "QUIT")) {
        hos_log("control: QUIT");
        hos_exit(0);
    }
    if (!strcasecmp(tok, "MENU")) {
        control_menu(strtok(NULL, " \t\r\n"), strtok(NULL, "\r\n"));
        return;
    }
    if (!strcasecmp(tok, "MEM")) {
        const char *path = strtok(NULL, "\r\n");
        if (!path) return;
        while (*path == ' ' || *path == '\t') path++;
        const int64_t n = hos_mem_dump(path);
        if (n < 0) hos_log("control: MEM %s failed", path);
        else hos_log("control: MEM %s (%lld MB)", path, (long long)(n >> 20));
        return;
    }
    if (!strcasecmp(tok, "PEEK") || !strcasecmp(tok, "POKE")) {
        const int poke = !strcasecmp(tok, "POKE");
        const char *at = strtok(NULL, " \t\r\n"), *what = strtok(NULL, " \t\r\n"), *path = strtok(NULL, "\r\n");
        if (!at || !what) return;
        const uint64_t a = strtoull(at, NULL, 16);
        if (poke) {   /* POKE <hex address> <hex bytes, in memory order> */
            const size_t n = strlen(what) / 2;
            uint8_t *p = hos_guest_range(a, n);
            if (!p) { hos_log("control: POKE %#llx: not guest memory", (unsigned long long)a); return; }
            for (size_t i = 0; i < n; i++) {
                unsigned v;
                if (sscanf(what + 2 * i, "%2x", &v) != 1) break;
                __atomic_store_n(&p[i], (uint8_t)v, __ATOMIC_RELAXED);
            }
            return;
        }
        /* PEEK <hex address> <bytes> <file>: the bytes, raw, into the file (written whole, then renamed into place) */
        const size_t n = strtoull(what, NULL, 0);
        const uint8_t *p = hos_guest_range(a, n);
        if (!p || !path) { hos_log("control: PEEK %#llx: not guest memory", (unsigned long long)a); return; }
        while (*path == ' ' || *path == '\t') path++;
        char tmp[4200];
        snprintf(tmp, sizeof tmp, "%s.part", path);
        FILE *f = fopen(tmp, "wb");
        if (!f) return;
        fwrite(p, 1, n, f);
        fclose(f);
        rename(tmp, path);
        return;
    }
    char *end;
    const double seconds = strtod(tok, &end);
    if (end == tok) {
        hos_log("control: \"%s\" is not a duration, SHOT or QUIT", tok);
        return;
    }
    ScriptStep s = {0};
    while ((tok = strtok(NULL, " \t\r\n")))
        if (!parse_input(tok, &s)) {
            hos_log("control: \"%s\" is not an input", tok);
            return;
        }
    pthread_mutex_lock(&input_lock);
    live = s;
    pthread_mutex_unlock(&input_lock);
    control_sleep(seconds);
    pthread_mutex_lock(&input_lock);
    memset(&live, 0, sizeof live);
    pthread_mutex_unlock(&input_lock);
}

static void *control_thread(void *p) {
    const char *path = p;
    char line[1024];
    for (;;) {
        FILE *f = fopen(path, "r");   /* (a FIFO: blocks until a writer opens it, read to its end, open again) */
        if (!f) {
            hos_log("control: %s cannot be opened", path);
            return NULL;
        }
        while (fgets(line, sizeof line, f)) control_line(line);
        fclose(f);
    }
}

static void control_start(void) {
    const char *path = getenv("HOS_CONTROL");
    if (!path || !*path) return;
    struct stat st;
    if (stat(path, &st) && mkfifo(path, 0600)) hos_fatal(NULL, "HOS_CONTROL: %s cannot be made", path);
    pthread_t t;
    pthread_create(&t, NULL, control_thread, (void *)path);
    pthread_detach(t);
    hos_log("control: reading %s", path);
}

/* stick directions as buttons, as the console sets them (past half way) */
static uint64_t stick_buttons(const int32_t s[4]) {
    const int32_t t = 16384;
    uint64_t b = 0;
    if (s[0] < -t) b |= HOS_BTN_LSTICK_LEFT;
    if (s[1] > t) b |= HOS_BTN_LSTICK_UP;
    if (s[0] > t) b |= HOS_BTN_LSTICK_RIGHT;
    if (s[1] < -t) b |= HOS_BTN_LSTICK_DOWN;
    if (s[2] < -t) b |= HOS_BTN_RSTICK_LEFT;
    if (s[3] > t) b |= HOS_BTN_RSTICK_UP;
    if (s[2] > t) b |= HOS_BTN_RSTICK_RIGHT;
    if (s[3] < -t) b |= HOS_BTN_RSTICK_DOWN;
    return b;
}

void hos_input(HosInput *out) {
    if (hos_menu_is_open()) {   /* (the menu has the input: the game none, the scripts' included) */
        memset(out, 0, sizeof *out);
        return;
    }
    pthread_mutex_lock(&input_lock);
    *out = player;
    const ScriptStep live_now = live;
    pthread_mutex_unlock(&input_lock);
    const ScriptStep *steps[2] = {script_now(), &live_now};
    for (int k = 0; k < 2; k++) {
        const ScriptStep *s = steps[k];
        if (!s) continue;
        out->buttons |= s->buttons;
        for (int i = 0; i < 4; i++)
            if (s->stick[i]) out->stick[i] = s->stick[i];
        if (s->touch) out->touch = 1, out->touch_x = s->touch_x, out->touch_y = s->touch_y;
    }
    out->buttons |= stick_buttons(out->stick);
}

/* ---- the window --------------------------------------------------------------------------------------------------- */
#ifdef HOS_HAVE_SDL3
typedef struct { void (*fn)(void); } GuestMain;
static void *guest_thread(void *p) {
    ((GuestMain *)p)->fn();
    hos_log("the game's main thread ended");
    hos_exit(0);
}

static SDL_Window *window;
static SDL_Renderer *renderer;   /* (the status page's: none when the Vulkan back end presents the game) */
static SDL_Gamepad *pad;

/* a gamepad axis to a stick value: SDL's -32768..32767 with y down, the menu's dead zone (in % of full) */
static int32_t pad_axis(SDL_GamepadAxis a, int flip) {
    int32_t v = SDL_GetGamepadAxis(pad, a);
    if (flip) v = -v;
    const int32_t dead = hos_setting(HOS_SET_DEAD_ZONE) * 32767 / 100;
    if (v > -dead && v < dead) return 0;
    return v < -32767 ? -32767 : v;
}
/* the gamepad's sticks: left x, y, right x, y, up positive, after the dead zone (0 without a gamepad) */
static void pad_axes(int32_t axes[4]) {
    if (!pad) {
        memset(axes, 0, 4 * sizeof *axes);
        return;
    }
    axes[0] = pad_axis(SDL_GAMEPAD_AXIS_LEFTX, 0), axes[1] = pad_axis(SDL_GAMEPAD_AXIS_LEFTY, 1);
    axes[2] = pad_axis(SDL_GAMEPAD_AXIS_RIGHTX, 0), axes[3] = pad_axis(SDL_GAMEPAD_AXIS_RIGHTY, 1);
}
/* the gamepad's held inputs, a bit each (hos_menu.h: its buttons, the triggers, the sticks' directions) */
static uint64_t pad_held(const int32_t axes[4]) {
    uint64_t held = 0;
    if (!pad) return 0;
    for (int b = 0; b < HOS_PAD_BUTTONS; b++)
        if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) held |= 1ull << b;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 10000) held |= 1ull << HOS_PAD_LT;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 10000) held |= 1ull << HOS_PAD_RT;
    return held | hos_pad_stick_bits(axes);
}
static int hold_off;   /* the menu closed with something held: the game sees nothing until it is all let go */

/* nn::hid KeyboardModifier bits */
static uint32_t modifiers(SDL_Keymod m) {
    uint32_t r = 0;
    if (m & SDL_KMOD_CTRL) r |= 1u << 0;
    if (m & SDL_KMOD_SHIFT) r |= 1u << 1;
    if (m & SDL_KMOD_LALT) r |= 1u << 2;
    if (m & SDL_KMOD_RALT) r |= 1u << 3;
    if (m & SDL_KMOD_GUI) r |= 1u << 4;
    if (m & SDL_KMOD_CAPS) r |= 1u << 8;
    if (m & SDL_KMOD_SCROLL) r |= 1u << 9;
    if (m & SDL_KMOD_NUM) r |= 1u << 10;
    return r;
}

/* the mouse in the console's 1280x720 screen (the picture letterboxed into the window), and its buttons */
static SDL_MouseButtonFlags mouse_console(float *x, float *y) {
    float wx, wy;
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(&wx, &wy);
    *x = *y = 0;
    if (renderer) {
        SDL_RenderCoordinatesFromWindow(renderer, wx, wy, x, y);
    } else {   /* (the Vulkan present letterboxes the picture into the window the same way) */
        int ww = 1280, wh = 720;
        SDL_GetWindowSize(window, &ww, &wh);
        const float scale = SDL_min((float)ww / 1280, (float)wh / 720);
        if (scale > 0) *x = (wx - (ww - 1280 * scale) / 2) / scale, *y = (wy - (wh - 720 * scale) / 2) / scale;
    }
    return mb;
}

static void read_input(int32_t wheel) {
    HosInput in = {0};
    int nkeys = 0;
    const bool *k = SDL_GetKeyboardState(&nkeys);
    int32_t axes[4];
    pad_axes(axes);
    in.buttons = hos_controls_eval(k, nkeys, pad_held(axes), axes, in.stick);
    /* the keyboard as a keyboard: HID usage ids are SDL's scancodes */
    for (int sc = 0; sc < 256; sc++)
        if (k[sc]) in.keys[sc / 8] |= (uint8_t)(1u << (sc % 8));
    in.modifiers = modifiers(SDL_GetModState());
    /* the mouse, in the console's 1280x720 screen */
    float x, y;
    const SDL_MouseButtonFlags mb = mouse_console(&x, &y);
    in.mouse_x = (int32_t)x, in.mouse_y = (int32_t)y, in.wheel = wheel;
    in.mouse_in = x >= 0 && y >= 0 && x < 1280 && y < 720 && (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_FOCUS);
    if (mb & SDL_BUTTON_LMASK) in.mouse_buttons |= 1u << 0;
    if (mb & SDL_BUTTON_RMASK) in.mouse_buttons |= 1u << 1;
    if (mb & SDL_BUTTON_MMASK) in.mouse_buttons |= 1u << 2;
    if (mb & SDL_BUTTON_X2MASK) in.mouse_buttons |= 1u << 3;   /* Forward */
    if (mb & SDL_BUTTON_X1MASK) in.mouse_buttons |= 1u << 4;   /* Back */
    if ((mb & SDL_BUTTON_LMASK) && in.mouse_in && hos_setting(HOS_SET_MOUSE_TOUCH))
        in.touch = 1, in.touch_x = in.mouse_x, in.touch_y = in.mouse_y;
    if (hold_off) {   /* (after the menu: everything let go first) */
        if (in.buttons || stick_buttons(in.stick) || in.mouse_buttons || hos_menu_is_open()) {
            memset(&in, 0, sizeof in);
        } else {
            hold_off = 0;
        }
    }
    pthread_mutex_lock(&input_lock);
    player = in;
    pthread_mutex_unlock(&input_lock);
}

static void draw_status(void) {
    extern uint64_t hos_present_count;
    static uint64_t last_count;
    static int64_t last_ns;
    static double fps;
    const int64_t now = hos_now_ns();
    const uint64_t n = __atomic_load_n(&hos_present_count, __ATOMIC_RELAXED);
    if (now - last_ns >= 1000000000LL) {
        fps = last_ns ? (double)(n - last_count) * 1e9 / (double)(now - last_ns) : 0;
        last_count = n, last_ns = now;
    }
    HosInput in;
    hos_input(&in);
    char line[256];
    SDL_SetRenderDrawColor(renderer, 16, 18, 24, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderScale(renderer, 2, 2);
    SDL_SetRenderDrawColor(renderer, 255, 170, 60, 255);
    SDL_RenderDebugText(renderer, 16, 16, "oranges");
    SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
    SDL_RenderDebugText(renderer, 16, 32, "no renderer yet: the game runs, nothing is drawn");
    snprintf(line, sizeof line, "frames presented %llu   %.1f fps", (unsigned long long)n, fps);
    SDL_RenderDebugText(renderer, 16, 56, line);
    int o = snprintf(line, sizeof line, "buttons");
    for (size_t i = 0; i < sizeof button_names / sizeof *button_names; i++)
        if (in.buttons & button_names[i].bit) o += snprintf(line + o, sizeof line - (size_t)o, " %s", button_names[i].name);
    SDL_RenderDebugText(renderer, 16, 80, line);
    snprintf(line, sizeof line, "sticks %6d %6d   %6d %6d", in.stick[0], in.stick[1], in.stick[2], in.stick[3]);
    SDL_RenderDebugText(renderer, 16, 92, line);
    snprintf(line, sizeof line, "mouse %4d %4d  buttons %x  touch %s", in.mouse_x, in.mouse_y, in.mouse_buttons,
             in.touch ? "yes" : "no");
    SDL_RenderDebugText(renderer, 16, 104, line);
    snprintf(line, sizeof line, "gamepad: %s", pad ? SDL_GetGamepadName(pad) : "none (the keyboard is the pad)");
    SDL_RenderDebugText(renderer, 16, 116, line);
    SDL_SetRenderDrawColor(renderer, 150, 150, 150, 255);
    SDL_RenderDebugText(renderer, 16, 140, "default keys: WASD stick  IJKL right stick  arrows D-pad");
    SDL_RenderDebugText(renderer, 16, 152, "Space/Enter A  Shift/Backspace B  E X  F Y  Q L  R R  Z ZL  C ZR");
    SDL_RenderDebugText(renderer, 16, 164, "Esc +  Tab -  X/N stick clicks  F11 full screen");
    SDL_SetRenderScale(renderer, 1, 1);
}

/* Started by Steam: its variables in the log, and X11 before Wayland. Steam Input chooses a game's controller
   layout by the window in front, which it follows through X11: with our window a native Wayland one, it kept its
   Desktop layout (the Steam Controller a keyboard and mouse) where XWayland got the game's. SDL_VIDEO_DRIVER chooses
   for itself; HOS_WAYLAND=1 keeps Wayland. */
static void steam_start(void) {
    const char *game = getenv("SteamGameId"), *app = getenv("SteamAppId");
    if (!(game && *game) && !(app && *app)) return;
    const char *vars[] = {"SteamGameId", "SteamAppId", "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD",
                          "SDL_GAMECONTROLLER_IGNORE_DEVICES", "SDL_VIDEO_DRIVER"};
    for (size_t i = 0; i < sizeof vars / sizeof *vars; i++) {
        const char *v = getenv(vars[i]);
        if (v) hos_log("steam: %s=%.200s", vars[i], v);
    }
    const char *driver = getenv("SDL_VIDEO_DRIVER"), *wayland = getenv("HOS_WAYLAND"), *x11 = getenv("DISPLAY");
    if ((driver && *driver) || (wayland && *wayland && strcmp(wayland, "0")) || !(x11 && *x11)) return;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11,wayland");
    hos_log("steam: started by Steam: an X11 window (XWayland), for Steam Input's layout (HOS_WAYLAND=1: Wayland)");
}

static int window_open(void) {
    const char *off = getenv("HOS_NO_WINDOW");
    if (off && *off && strcmp(off, "0")) return 0;
    steam_start();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        hos_log("window: none (%s): the game runs without one", SDL_GetError());
        return 0;
    }
#ifdef HOS_HAVE_VULKAN
    /* the game's picture: a Vulkan window the back end presents to (host/vk); the status page if it does not start */
    if ((window = SDL_CreateWindow("oranges", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN))) {
        if (rx_vk_init(window)) {
            hos_log("window: 1280x720 (%s, Vulkan)", SDL_GetCurrentVideoDriver());
            return 1;
        }
        SDL_DestroyWindow(window);
        window = NULL;
    }
#endif
    if (!SDL_CreateWindowAndRenderer("oranges", 1280, 720, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        hos_log("window: none (%s): the game runs without one", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);   /* (not the sound's) */
        return 0;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderLogicalPresentation(renderer, 1280, 720, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    hos_log("window: 1280x720 (%s, %s)", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer));
    return 1;
}

/* HOS_SCREENSHOT=<file.bmp>[,seconds]: the window's picture, once, that long after the start (default 10 s) */
static void screenshot(void) {
    if (!renderer) return;   /* (the Vulkan back end takes its own: host/vk/vk_device.cpp) */
    static const char *path;
    static int64_t at = -1;
    static char file[1024];
    if (at == -1) {
        const char *s = getenv("HOS_SCREENSHOT");
        at = -2;
        if (!s || !*s) return;
        snprintf(file, sizeof file, "%s", s);
        char *comma = strrchr(file, ',');
        double secs = 10;
        if (comma) *comma = 0, secs = atof(comma + 1);
        path = file, at = (int64_t)(secs * 1e9);
    }
    if (at < 0 || hos_now_ns() < at) return;
    at = -2;
    SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
    if (shot && SDL_SaveBMP(shot, path)) hos_log("window: screenshot %s", path);
    else hos_log("window: screenshot %s failed (%s)", path, SDL_GetError());
    SDL_DestroySurface(shot);
}

/* the menu's navigation by the gamepad's left stick (as the D-pad) and a held D-pad: again after 0.4 s, then every
   90 ms */
static void menu_pad_repeat(void) {
    static int dir = -1, from_stick;
    static int64_t next;
    if (!pad || !hos_menu_is_open()) {
        dir = -1;
        return;
    }
    static const SDL_GamepadButton dpad[4] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                              SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    static const int actions[4] = {HOS_MENU_UP, HOS_MENU_DOWN, HOS_MENU_LEFT, HOS_MENU_RIGHT};
    int d = -1, stick = 0;
    for (int i = 0; i < 4 && d < 0; i++)
        if (SDL_GetGamepadButton(pad, dpad[i])) d = i;
    /* (the left stick moves the selection, but not while a binding waits for its input, nor after that until it is
       back at rest: a stick pushed to bind it would move on as well) */
    static int stick_off;
    const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    const int t = 20000;
    if (hos_menu_capturing()) stick_off = 1;
    else if (stick_off && abs(x) < 8000 && abs(y) < 8000) stick_off = 0;
    if (d < 0 && !stick_off) {
        if (abs(y) >= abs(x) && (y < -t || y > t)) d = y < 0 ? 0 : 1, stick = 1;
        else if (x < -t || x > t) d = x < 0 ? 2 : 3, stick = 1;
    }
    const int64_t now = hos_now_ns();
    if (d != dir || stick != from_stick) {
        dir = d, from_stick = stick, next = now + 400000000LL;
        if (d >= 0 && stick) hos_menu_device(1), hos_menu_action(actions[d]);   /* (a D-pad press came as an event) */
        return;
    }
    if (d >= 0 && now >= next) {
        next = now + 90000000LL;
        if (stick) hos_menu_action(actions[d]);
        else hos_menu_pad((int)dpad[d], 1);
    }
}

/* a stick pushed while a binding waits for a gamepad input: that stick direction (past 60%, from below 30%) */
static void menu_stick_capture(void) {
    static uint64_t armed;   /* (the directions that were near rest since the wait began) */
    if (!pad || !hos_menu_capturing()) {
        armed = 0;
        return;
    }
    const SDL_GamepadAxis ax[4] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                   SDL_GAMEPAD_AXIS_RIGHTY};
    int32_t v[4];
    for (int i = 0; i < 4; i++) v[i] = SDL_GetGamepadAxis(pad, ax[i]) * (i & 1 ? -1 : 1);   /* (up positive) */
    for (int k = 0; k < 8; k++) {
        const int stick = k / 4, dir = k % 4;   /* (up, down, left, right) */
        const int32_t a = (dir == 0 || dir == 3 ? 1 : -1) * v[stick * 2 + (dir < 2)];
        if (a < 32767 * 3 / 10) armed |= 1ull << k;
        else if (a > 32767 * 6 / 10 && (armed >> k & 1)) {
            hos_menu_device(1);
            hos_menu_pad(HOS_PAD_LSTICK_UP + k, 0);
            armed = 0;
            return;
        }
    }
}

static void window_loop(void) {
    float wheel = 0;
    int was_open = 0;
    const int menu = !renderer;   /* (the menu is drawn by the Vulkan back end: not on the status page) */
    for (;;) {
        SDL_Event e;
        float menu_wheel = 0;
        int click = 0, right_click = 0;
        while (SDL_PollEvent(&e)) {
            const int open = hos_menu_is_open();
            switch (e.type) {
            case SDL_EVENT_QUIT:
                hos_log("window: closed");
                hos_exit(0);
            case SDL_EVENT_GAMEPAD_ADDED: {
                const SDL_JoystickID id = e.gdevice.which;
                hos_log("input: gamepad \"%s\" (%04x:%04x, type %s%s", SDL_GetGamepadNameForID(id),
                        SDL_GetGamepadVendorForID(id), SDL_GetGamepadProductForID(id),
                        SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(id)),
                        pad ? "): not used, the first one is" : ")");
                if (!pad && (pad = SDL_OpenGamepad(id)))
                    hos_log("input: gamepad \"%s\" is the Pro Controller%s", SDL_GetGamepadName(pad),
                            SDL_GetGamepadSteamHandle(pad) ? " (Steam Input's)" : "");
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
                    hos_log("input: gamepad \"%s\" removed", SDL_GetGamepadName(pad));
                    SDL_CloseGamepad(pad);
                    pad = NULL;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (open) menu_wheel += e.wheel.y;
                else wheel += e.wheel.y;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (open) click |= e.button.button == SDL_BUTTON_LEFT, right_click |= e.button.button == SDL_BUTTON_RIGHT;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat)
                    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                else if (e.key.scancode == SDL_SCANCODE_F1 && !e.key.repeat && menu)
                    hos_menu_device(0), hos_menu_action(HOS_MENU_TOGGLE);
                else if (open)
                    hos_menu_key((int)e.key.scancode, (e.key.mod & SDL_KMOD_SHIFT) != 0, e.key.repeat);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (!pad || SDL_GetGamepadID(pad) != e.gbutton.which) break;
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE && menu)
                    hos_menu_device(1), hos_menu_action(HOS_MENU_TOGGLE);
                else if (open)
                    hos_menu_pad(e.gbutton.button, 0);
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {   /* (a trigger pressed: an input a binding can take) */
                static int16_t last[2];
                const int t = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0
                            : e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? 1 : -1;
                if (t < 0 || !pad || SDL_GetGamepadID(pad) != e.gaxis.which) break;
                if (open && last[t] <= 10000 && e.gaxis.value > 10000) hos_menu_pad(t ? HOS_PAD_RT : HOS_PAD_LT, 0);
                last[t] = e.gaxis.value;
                break;
            }
            default:
                break;
            }
        }
        if (hos_menu_is_open()) {   /* (the mouse, in the console's screen: the menu is drawn over the picture) */
            float x, y;
            mouse_console(&x, &y);
            hos_menu_mouse(x, y, click, right_click, menu_wheel);
            menu_pad_repeat();
            menu_stick_capture();
            was_open = 1;
        } else if (was_open) {
            was_open = 0, hold_off = 1;
        }
        read_input((int32_t)SDL_floorf(wheel));
        if (pad) {   /* (each frame, for a little longer than one: it stops by itself when the game stops) */
            float lo, hi;
            __atomic_load(&rumble[0], &lo, __ATOMIC_RELAXED);
            __atomic_load(&rumble[1], &hi, __ATOMIC_RELAXED);
            const float k = hos_menu_is_open() ? 0.0f : (float)hos_setting(HOS_SET_VIBRATION) / 100.0f;
            SDL_RumbleGamepad(pad, (Uint16)(SDL_min(lo * k, 1.0f) * 65535), (Uint16)(SDL_min(hi * k, 1.0f) * 65535), 100);
        }
        if (renderer) {
            draw_status();   /* (vsync paces the loop) */
            screenshot();
            SDL_RenderPresent(renderer);
        } else {   /* (the game presents its frames itself: events and input a few times a frame) */
            SDL_WaitEventTimeout(NULL, 4);
        }
    }
}
#endif

void hos_window_run(void (*guest_main)(void)) {
    hos_menu_load();   /* (the settings and the controls, from the options file) */
    script_load();
    control_start();
#ifdef HOS_HAVE_SDL3
    SDL_SetAppMetadata("oranges", NULL, "dev.oranges");
    const int windowed = window_open();
    hos_sound_init(windowed);
    if (windowed) {
        static GuestMain g;
        g.fn = guest_main;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 64 << 20);   /* (host frames of lifted code: the main thread's call chains) */
        pthread_t t;
        if (pthread_create(&t, &a, guest_thread, &g)) hos_fatal(NULL, "the game's main thread could not start");
        window_loop();
    }
#else
    hos_sound_init(0);
#endif
#ifdef HOS_HAVE_VULKAN
    rx_vk_init(NULL);   /* (no window: the back end draws all the same, for HOS_SCREENSHOT) */
#endif
    guest_main();
}
