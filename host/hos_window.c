/* The window and the player's input (SDL3), and scripted input for test runs.

   hos_window_run runs the game: with a window, the guest's main thread goes to a host thread of its own and this one
   (the process's first: SDL wants its video calls there) pumps the window's events and draws, once a display refresh.
   Without one (a build without SDL3, HOS_NO_WINDOW=1, or no display) the game runs on this thread, as before.

   There is no renderer yet (rx_null.c draws nothing), so the window shows a status page: frames, fps, and what the
   input does. The Vulkan back end will present the game's window textures here instead.

   Input. hos_input() is the merged state nn_hid.c reads: the keyboard and the first gamepad as Npad No.1 (a Pro
   Controller), the mouse as the console's USB mouse and, while its left button is down, as one finger on the touch
   screen, and the keyboard as the console's USB keyboard too. A gamepad's buttons map by position (south is B, east
   A, west Y, north X), as on a Nintendo pad. Keys:
       left stick W A S D        right stick I J K L       D-pad arrow keys
       A Space / Enter           B Left Shift / Backspace  X E          Y F
       L Q   R R   ZL Z   ZR C   + Escape   - Tab          stick clicks X (L), N (R)
       F11: full screen
   The game's vibration goes to the gamepad's motors (hos_rumble).
   HOS_INPUT_SCRIPT=<file>: input from a script, added to the player's: one line per change, "<seconds> <what>...",
   held from that time (since boot) until the next line. What: button names (A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT
   RIGHT LS RS), LSTICK=x,y / RSTICK=x,y (-1..1, up positive), TOUCH=x,y (1280x720), QUIT (ends the program). '#'
   starts a comment.
   HOS_SCREENSHOT=<file.bmp>[,seconds]: the window's picture, saved once, that long after the start (default 10). */
#include "hos.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        while ((tok = strtok(NULL, " \t\r\n"))) {
            for (char *p = tok; *p && *p != '='; p++) *p = (char)toupper((unsigned char)*p);
            double x, y;
            int known = 0;
            if (sscanf(tok, "LSTICK=%lf,%lf", &x, &y) == 2) s.stick[0] = clamp_stick(x), s.stick[1] = clamp_stick(y), known = 1;
            else if (sscanf(tok, "RSTICK=%lf,%lf", &x, &y) == 2) s.stick[2] = clamp_stick(x), s.stick[3] = clamp_stick(y), known = 1;
            else if (sscanf(tok, "TOUCH=%lf,%lf", &x, &y) == 2) s.touch = 1, s.touch_x = (int32_t)x, s.touch_y = (int32_t)y, known = 1;
            else if (!strcmp(tok, "QUIT")) s.quit = 1, known = 1;
            for (size_t i = 0; i < sizeof button_names / sizeof *button_names && !known; i++)
                if (!strcmp(tok, button_names[i].name)) s.buttons |= button_names[i].bit, known = 1;
            if (!known) hos_fatal(NULL, "HOS_INPUT_SCRIPT: %s: \"%s\" is not an input", path, tok);
        }
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
    pthread_mutex_lock(&input_lock);
    *out = player;
    pthread_mutex_unlock(&input_lock);
    const ScriptStep *s = script_now();
    if (s) {
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
static SDL_Renderer *renderer;
static SDL_Gamepad *pad;

static const struct { SDL_Scancode key; uint64_t bit; } key_buttons[] = {
    {SDL_SCANCODE_SPACE, HOS_BTN_A},       {SDL_SCANCODE_RETURN, HOS_BTN_A},
    {SDL_SCANCODE_LSHIFT, HOS_BTN_B},      {SDL_SCANCODE_BACKSPACE, HOS_BTN_B},
    {SDL_SCANCODE_E, HOS_BTN_X},           {SDL_SCANCODE_F, HOS_BTN_Y},
    {SDL_SCANCODE_Q, HOS_BTN_L},           {SDL_SCANCODE_R, HOS_BTN_R},
    {SDL_SCANCODE_Z, HOS_BTN_ZL},          {SDL_SCANCODE_C, HOS_BTN_ZR},
    {SDL_SCANCODE_ESCAPE, HOS_BTN_PLUS},   {SDL_SCANCODE_TAB, HOS_BTN_MINUS},
    {SDL_SCANCODE_X, HOS_BTN_LS},          {SDL_SCANCODE_N, HOS_BTN_RS},
    {SDL_SCANCODE_UP, HOS_BTN_UP},         {SDL_SCANCODE_DOWN, HOS_BTN_DOWN},
    {SDL_SCANCODE_LEFT, HOS_BTN_LEFT},     {SDL_SCANCODE_RIGHT, HOS_BTN_RIGHT},
};
static const struct { SDL_GamepadButton b; uint64_t bit; } pad_buttons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, HOS_BTN_B},          {SDL_GAMEPAD_BUTTON_EAST, HOS_BTN_A},
    {SDL_GAMEPAD_BUTTON_WEST, HOS_BTN_Y},           {SDL_GAMEPAD_BUTTON_NORTH, HOS_BTN_X},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, HOS_BTN_L},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, HOS_BTN_R},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, HOS_BTN_LS},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, HOS_BTN_RS},
    {SDL_GAMEPAD_BUTTON_START, HOS_BTN_PLUS},       {SDL_GAMEPAD_BUTTON_BACK, HOS_BTN_MINUS},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, HOS_BTN_UP},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, HOS_BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, HOS_BTN_LEFT},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, HOS_BTN_RIGHT},
};

/* a gamepad axis to a stick value: SDL's -32768..32767 with y down, a small dead zone */
static int32_t pad_axis(SDL_GamepadAxis a, int flip) {
    int32_t v = SDL_GetGamepadAxis(pad, a);
    if (flip) v = -v;
    if (v > -3000 && v < 3000) return 0;
    return v < -32767 ? -32767 : v;
}

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

static void read_input(int32_t wheel) {
    HosInput in = {0};
    const bool *k = SDL_GetKeyboardState(NULL);
    for (size_t i = 0; i < sizeof key_buttons / sizeof *key_buttons; i++)
        if (k[key_buttons[i].key]) in.buttons |= key_buttons[i].bit;
    const int32_t full = 32767;
    int32_t ks[4] = {(k[SDL_SCANCODE_D] - k[SDL_SCANCODE_A]) * full, (k[SDL_SCANCODE_W] - k[SDL_SCANCODE_S]) * full,
                     (k[SDL_SCANCODE_L] - k[SDL_SCANCODE_J]) * full, (k[SDL_SCANCODE_I] - k[SDL_SCANCODE_K]) * full};
    for (int i = 0; i < 4; i++) in.stick[i] = ks[i];
    if (pad) {
        for (size_t i = 0; i < sizeof pad_buttons / sizeof *pad_buttons; i++)
            if (SDL_GetGamepadButton(pad, pad_buttons[i].b)) in.buttons |= pad_buttons[i].bit;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 10000) in.buttons |= HOS_BTN_ZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 10000) in.buttons |= HOS_BTN_ZR;
        const int32_t ps[4] = {pad_axis(SDL_GAMEPAD_AXIS_LEFTX, 0), pad_axis(SDL_GAMEPAD_AXIS_LEFTY, 1),
                               pad_axis(SDL_GAMEPAD_AXIS_RIGHTX, 0), pad_axis(SDL_GAMEPAD_AXIS_RIGHTY, 1)};
        for (int i = 0; i < 4; i++)
            if (!in.stick[i]) in.stick[i] = ps[i];
    }
    /* the keyboard as a keyboard: HID usage ids are SDL's scancodes */
    for (int sc = 0; sc < 256; sc++)
        if (k[sc]) in.keys[sc / 8] |= (uint8_t)(1u << (sc % 8));
    in.modifiers = modifiers(SDL_GetModState());
    /* the mouse, in the console's 1280x720 screen */
    float wx, wy, x = 0, y = 0;
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(&wx, &wy);
    SDL_RenderCoordinatesFromWindow(renderer, wx, wy, &x, &y);
    in.mouse_x = (int32_t)x, in.mouse_y = (int32_t)y, in.wheel = wheel;
    in.mouse_in = x >= 0 && y >= 0 && x < 1280 && y < 720 && (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_FOCUS);
    if (mb & SDL_BUTTON_LMASK) in.mouse_buttons |= 1u << 0;
    if (mb & SDL_BUTTON_RMASK) in.mouse_buttons |= 1u << 1;
    if (mb & SDL_BUTTON_MMASK) in.mouse_buttons |= 1u << 2;
    if (mb & SDL_BUTTON_X2MASK) in.mouse_buttons |= 1u << 3;   /* Forward */
    if (mb & SDL_BUTTON_X1MASK) in.mouse_buttons |= 1u << 4;   /* Back */
    if ((mb & SDL_BUTTON_LMASK) && in.mouse_in) in.touch = 1, in.touch_x = in.mouse_x, in.touch_y = in.mouse_y;
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
    SDL_RenderDebugText(renderer, 16, 140, "WASD stick  IJKL right stick  arrows D-pad");
    SDL_RenderDebugText(renderer, 16, 152, "Space/Enter A  Shift/Backspace B  E X  F Y  Q L  R R  Z ZL  C ZR");
    SDL_RenderDebugText(renderer, 16, 164, "Esc +  Tab -  X/N stick clicks  F11 full screen");
    SDL_SetRenderScale(renderer, 1, 1);
}

static int window_open(void) {
    const char *off = getenv("HOS_NO_WINDOW");
    if (off && *off && strcmp(off, "0")) return 0;
    SDL_SetAppMetadata("oranges", NULL, "dev.oranges");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        hos_log("window: none (%s): the game runs without one", SDL_GetError());
        return 0;
    }
    if (!SDL_CreateWindowAndRenderer("oranges", 1280, 720, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        hos_log("window: none (%s): the game runs without one", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderLogicalPresentation(renderer, 1280, 720, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    hos_log("window: 1280x720 (%s, %s)", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer));
    return 1;
}

/* HOS_SCREENSHOT=<file.bmp>[,seconds]: the window's picture, once, that long after the start (default 10 s) */
static void screenshot(void) {
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

static void window_loop(void) {
    float wheel = 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                hos_log("window: closed");
                hos_exit(0);
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad && (pad = SDL_OpenGamepad(e.gdevice.which)))
                    hos_log("input: gamepad \"%s\" is the Pro Controller", SDL_GetGamepadName(pad));
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
                    hos_log("input: gamepad \"%s\" removed", SDL_GetGamepadName(pad));
                    SDL_CloseGamepad(pad);
                    pad = NULL;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                wheel += e.wheel.y;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat)
                    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                break;
            default:
                break;
            }
        }
        read_input((int32_t)SDL_floorf(wheel));
        if (pad) {   /* (each frame, for a little longer than one: it stops by itself when the game stops) */
            float lo, hi;
            __atomic_load(&rumble[0], &lo, __ATOMIC_RELAXED);
            __atomic_load(&rumble[1], &hi, __ATOMIC_RELAXED);
            SDL_RumbleGamepad(pad, (Uint16)(SDL_min(lo, 1.0f) * 65535), (Uint16)(SDL_min(hi, 1.0f) * 65535), 100);
        }
        draw_status();   /* (vsync paces the loop) */
        screenshot();
        SDL_RenderPresent(renderer);
    }
}
#endif

void hos_window_run(void (*guest_main)(void)) {
    script_load();
#ifdef HOS_HAVE_SDL3
    if (window_open()) {
        static GuestMain g;
        g.fn = guest_main;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 64 << 20);   /* (host frames of lifted code: the main thread's call chains) */
        pthread_t t;
        if (pthread_create(&t, &a, guest_thread, &g)) hos_fatal(NULL, "the game's main thread could not start");
        window_loop();
    }
#endif
    guest_main();
}
