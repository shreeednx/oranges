/* The settings menu's model: its tabs and rows, what is selected, and what each input does (F1 opens it, a gamepad's
   Home too). Modelled on Longshot's Insert menu (gpu/vk/vk_menu.cpp there), much smaller: one tab so far, Controls.
   hos_window.c hands it the keys, gamepad buttons and mouse while it is open (and the game gets no input); the Vulkan
   back end draws it at each present from a snapshot (hos_menu_view; host/vk/vk_menu.cpp) and hands back where things
   are (hos_menu_hits) for the mouse. HOS_CONTROL's MENU lines drive it too (a headless run's checks).
   Every change is saved at once (hos_options.c). A binding waits for its input (6 s, Esc cancels); an input that is
   another control's is moved only once the player says yes.
   Keyboard: arrows choose and change, Enter / Space select, Delete clears a binding, R resets a row, Q / E or Tab
   switch tabs, Esc / Backspace close. Gamepad: the D-pad or the left stick, the buttons bound to A (select), B (close),
   X (clear) and Y (reset), the shoulders switch tabs. Mouse: point and click, right click closes, the wheel scrolls. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hos_menu.h"
#include "hos_options.h"

void hos_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int64_t hos_now_ns(void);

/* SDL scancodes the menu answers to */
enum { K_E = 8, K_Q = 20, K_R = 21, K_RETURN = 40, K_ESCAPE = 41, K_BACKSPACE = 42, K_TAB = 43, K_SPACE = 44,
       K_F1 = 58, K_F11 = 68, K_DELETE = 76, K_RIGHT = 79, K_LEFT = 80, K_DOWN = 81, K_UP = 82, K_KP_ENTER = 88 };
/* gamepad inputs (SDL_GamepadButton) */
enum { P_SOUTH, P_EAST, P_WEST, P_NORTH, P_GUIDE = 5, P_LSHOULDER = 9, P_RSHOULDER, P_DUP, P_DDOWN, P_DLEFT, P_DRIGHT };

#define CAPTURE_NS 6000000000LL
#define MESSAGE_NS 4000000000LL

/* ---- the settings ------------------------------------------------------------------------------------------------- */
static const struct {
    const char *key, *label, *help;
    int lo, hi, step, def;
    const char *off;   /* (the text for lo, when it is "Off") */
    const char *unit;  /* ("%", or NULL: Off / On) */
} settings_def[HOS_SET_N] = {
    [HOS_SET_DEAD_ZONE] = {"stick_dead_zone", "Stick dead zone",
                           "How far a gamepad's stick moves before the game sees it. Raise it if the player walks by "
                           "itself; lower it for finer steps.", 0, 30, 5, 10, NULL, "%"},
    [HOS_SET_VIBRATION] = {"vibration", "Vibration",
                           "How strongly the gamepad rumbles when the game asks it to. Off: never.", 0, 100, 25, 100,
                           "Off", "%"},
    [HOS_SET_MOUSE_TOUCH] = {"mouse_touch", "Mouse as touch screen",
                             "On: the mouse's left button is a finger on the touch screen, where the game reads touch. "
                             "The mouse is also the console's USB mouse, either way.", 0, 1, 1, 1, NULL, NULL},
};
static int settings[HOS_SET_N];
static int settings_loaded;

static void settings_load(void) {
    if (settings_loaded) return;
    settings_loaded = 1;
    for (int i = 0; i < HOS_SET_N; i++) {
        int v = hos_option_int(settings_def[i].key, settings_def[i].def);
        if (v < settings_def[i].lo) v = settings_def[i].lo;
        if (v > settings_def[i].hi) v = settings_def[i].hi;
        __atomic_store_n(&settings[i], v, __ATOMIC_RELAXED);
    }
}
int hos_setting(int id) {
    if (id < 0 || id >= HOS_SET_N) return 0;
    if (!__atomic_load_n(&settings_loaded, __ATOMIC_ACQUIRE)) hos_menu_load();
    return __atomic_load_n(&settings[id], __ATOMIC_RELAXED);
}
static void setting_put(int id, int v) {
    if (v < settings_def[id].lo) v = settings_def[id].lo;
    if (v > settings_def[id].hi) v = settings_def[id].hi;
    __atomic_store_n(&settings[id], v, __ATOMIC_RELAXED);
    if (v == settings_def[id].def) {
        hos_option_unset(settings_def[id].key);
    } else {
        char s[16];
        snprintf(s, sizeof s, "%d", v);
        hos_option_set(settings_def[id].key, s);
    }
}
static void setting_text(int id, char *out, size_t size) {
    const int v = settings[id];
    if (settings_def[id].off && v == settings_def[id].lo) snprintf(out, size, "%s", settings_def[id].off);
    else if (!settings_def[id].unit) snprintf(out, size, "%s", v ? "On" : "Off");
    else snprintf(out, size, "%d%s", v, settings_def[id].unit);
}

/* ---- the rows ------------------------------------------------------------------------------------------------------ */
typedef struct { int kind, id; } Row;   /* (id: a setting, an action, or a control) */
enum { ACT_RESET_ALL };
static Row rows[HOS_MENU_ROWS];
static int nrows;
static void build_rows(void) {
    nrows = 0;
    for (int i = 0; i < HOS_SET_N; i++) rows[nrows++] = (Row){HOS_ROW_SETTING, i};
    rows[nrows++] = (Row){HOS_ROW_ACTION, ACT_RESET_ALL};
    rows[nrows++] = (Row){HOS_ROW_HEADING, 0};
    for (int c = 0; c < HOS_CTL_N; c++) rows[nrows++] = (Row){HOS_ROW_BINDING, c};
}

/* ---- the state (under lock) ------------------------------------------------------------------------------------------ */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int open_;   /* (read without the lock: hos_menu_is_open) */
static int tab, sel, col, pad_last;
static int mode = HOS_MENU_NORMAL;
static int64_t capture_end;
enum { Q_CONFLICT, Q_RESET_ALL };
static struct { int what, ctl, slot, input, other_ctl, other_slot, yes; } q;
static char message[160];
static int64_t message_end;
static HosMenuHit hits[256];
static int nhits;
static float mouse_x = -1, mouse_y = -1;

static const char *const tab_names[] = {"Controls"};
#define NTABS 1

void hos_menu_load(void) {
    pthread_mutex_lock(&lock);
    settings_load();
    if (!nrows) build_rows();
    pthread_mutex_unlock(&lock);
    hos_controls_load();
}
int hos_menu_is_open(void) { return __atomic_load_n(&open_, __ATOMIC_ACQUIRE); }
int hos_menu_capturing(void) {
    pthread_mutex_lock(&lock);
    if (mode == HOS_MENU_CAPTURE && hos_now_ns() > capture_end) mode = HOS_MENU_NORMAL;
    const int c = open_ && mode == HOS_MENU_CAPTURE;
    pthread_mutex_unlock(&lock);
    return c;
}

static void say(const char *fmt, const char *a, const char *b, const char *c) {
    snprintf(message, sizeof message, fmt, a, b, c);
    message_end = hos_now_ns() + MESSAGE_NS;
}

static int selectable(int r) { return r >= 0 && r < nrows && rows[r].kind != HOS_ROW_HEADING; }
static void move_sel(int d) {
    int r = sel;
    do r += d;
    while (r >= 0 && r < nrows && !selectable(r));
    if (selectable(r)) sel = r;
}

/* an input for the selected binding's cell, once it is no other control's (or the player said so) */
static void bind_now(int ctl, int slot, int input) {
    hos_bind(ctl, slot, input);
    capture_end = 0;
    mode = HOS_MENU_NORMAL;
}
static void captured(int pad, int input) {
    const int ctl = rows[sel].id, slot = col;
    int other, other_slot;
    if (hos_binding_owner(pad, input, ctl, &other, &other_slot)) {
        q.what = Q_CONFLICT, q.ctl = ctl, q.slot = slot, q.input = input, q.other_ctl = other;
        q.other_slot = other_slot, q.yes = 0;
        mode = HOS_MENU_CONFIRM;
        return;
    }
    bind_now(ctl, slot, input);
}
static void answer(int yes) {
    mode = HOS_MENU_NORMAL;
    if (!yes) return;
    if (q.what == Q_RESET_ALL) {
        hos_bindings_reset_all();
        say("%s%s%s", "Every binding is back to its default.", "", "");
        return;
    }
    char name[64], n[96];
    if (q.slot == HOS_BIND_KEYS) snprintf(n, sizeof n, "The gamepad's %s", hos_pad_name(q.input));
    else snprintf(n, sizeof n, "%s", hos_key_name(q.input, name, sizeof name));
    hos_bind(q.other_ctl, q.other_slot, q.slot == HOS_BIND_KEYS ? -1 : 0);
    hos_bind(q.ctl, q.slot, q.input);
    say("%s is now %s's (no longer %s's).", n, hos_control_name(q.ctl), hos_control_name(q.other_ctl));
}

static void set_open(int o) {
    if (o == open_) return;
    if (o && !nrows) build_rows();
    if (o && !selectable(sel)) sel = 0;
    mode = HOS_MENU_NORMAL;
    __atomic_store_n(&open_, o, __ATOMIC_RELEASE);
    hos_log("menu: %s", o ? "open" : "closed");
}

static void action_locked(int a) {
    if (a == HOS_MENU_TOGGLE) {
        set_open(!open_);
        return;
    }
    if (!open_) return;
    if (mode == HOS_MENU_CAPTURE) {   /* (waiting for an input: only Back cancels) */
        if (a == HOS_MENU_BACK) mode = HOS_MENU_NORMAL;
        return;
    }
    if (mode == HOS_MENU_CONFIRM) {
        if (a == HOS_MENU_LEFT || a == HOS_MENU_RIGHT || a == HOS_MENU_UP || a == HOS_MENU_DOWN) q.yes = !q.yes;
        else if (a == HOS_MENU_OK) answer(q.yes);
        else if (a == HOS_MENU_BACK) answer(0);
        return;
    }
    const Row *r = &rows[sel];
    switch (a) {
    case HOS_MENU_BACK: set_open(0); break;
    case HOS_MENU_UP: move_sel(-1); break;
    case HOS_MENU_DOWN: move_sel(1); break;
    case HOS_MENU_TAB_NEXT: tab = (tab + 1) % NTABS; break;
    case HOS_MENU_TAB_PREV: tab = (tab + NTABS - 1) % NTABS; break;
    case HOS_MENU_LEFT:
    case HOS_MENU_RIGHT: {
        const int d = a == HOS_MENU_RIGHT ? 1 : -1;
        if (r->kind == HOS_ROW_SETTING) setting_put(r->id, settings[r->id] + d * settings_def[r->id].step);
        else if (r->kind == HOS_ROW_BINDING) col = col + d < 0 ? 0 : col + d > 2 ? 2 : col + d;
        break;
    }
    case HOS_MENU_OK:
        if (r->kind == HOS_ROW_SETTING) {   /* (round from the top back to the bottom) */
            const int v = settings[r->id] + settings_def[r->id].step;
            setting_put(r->id, v > settings_def[r->id].hi ? settings_def[r->id].lo : v);
        } else if (r->kind == HOS_ROW_ACTION) {
            q.what = Q_RESET_ALL, q.yes = 0;
            mode = HOS_MENU_CONFIRM;
        } else if (r->kind == HOS_ROW_BINDING) {
            mode = HOS_MENU_CAPTURE;
            capture_end = hos_now_ns() + CAPTURE_NS;
        }
        break;
    case HOS_MENU_CLEAR:
        if (r->kind == HOS_ROW_BINDING) hos_bind(r->id, col, col == HOS_BIND_KEYS ? -1 : 0);
        break;
    case HOS_MENU_RESET:
        if (r->kind == HOS_ROW_BINDING) hos_binding_reset(r->id);
        else if (r->kind == HOS_ROW_SETTING) setting_put(r->id, settings_def[r->id].def);
        break;
    }
}
void hos_menu_action(int a) {
    pthread_mutex_lock(&lock);
    if (!nrows) build_rows();
    action_locked(a);
    pthread_mutex_unlock(&lock);
}

void hos_menu_key(int sc, int shift, int repeat) {
    pthread_mutex_lock(&lock);
    pad_last = 0;
    if (open_ && mode == HOS_MENU_CAPTURE) {
        if (!repeat) {
            if (sc == K_ESCAPE) mode = HOS_MENU_NORMAL;
            else if (sc == K_F1 || sc == K_F11) say("%s is the menu's (F11: full screen).", sc == K_F1 ? "F1" : "F11", "", "");
            else if (col < HOS_BIND_KEYS) captured(0, sc);
        }
        pthread_mutex_unlock(&lock);
        return;
    }
    int a = -1;
    switch (sc) {
    case K_UP: a = HOS_MENU_UP; break;
    case K_DOWN: a = HOS_MENU_DOWN; break;
    case K_LEFT: a = HOS_MENU_LEFT; break;
    case K_RIGHT: a = HOS_MENU_RIGHT; break;
    case K_RETURN: case K_KP_ENTER: case K_SPACE: a = repeat ? -1 : HOS_MENU_OK; break;
    case K_ESCAPE: case K_BACKSPACE: a = repeat ? -1 : HOS_MENU_BACK; break;
    case K_DELETE: a = HOS_MENU_CLEAR; break;
    case K_R: a = repeat ? -1 : HOS_MENU_RESET; break;
    case K_Q: a = HOS_MENU_TAB_PREV; break;
    case K_E: a = HOS_MENU_TAB_NEXT; break;
    case K_TAB: a = shift ? HOS_MENU_TAB_PREV : HOS_MENU_TAB_NEXT; break;
    }
    if (a >= 0) action_locked(a);
    pthread_mutex_unlock(&lock);
}

void hos_menu_pad(int input, int repeat) {
    pthread_mutex_lock(&lock);
    pad_last = 1;
    if (open_ && mode == HOS_MENU_CAPTURE) {
        if (!repeat && col == HOS_BIND_KEYS && input != P_GUIDE) captured(1, input);
        pthread_mutex_unlock(&lock);
        return;
    }
    /* (A, B, X and Y: the buttons the Switch's are bound to, as the game answers to them; the bottom and the right
       button when those are not face buttons) */
    int ok = hos_control_pad(HOS_CTL_A), back = hos_control_pad(HOS_CTL_B);
    if (ok < P_SOUTH || ok > P_NORTH) ok = P_EAST;
    if (back < P_SOUTH || back > P_NORTH || back == ok) back = ok == P_SOUTH ? P_EAST : P_SOUTH;
    const int clear = hos_control_pad(HOS_CTL_X), reset = hos_control_pad(HOS_CTL_Y);
    int a = -1;
    if (input == P_DUP) a = HOS_MENU_UP;
    else if (input == P_DDOWN) a = HOS_MENU_DOWN;
    else if (input == P_DLEFT) a = HOS_MENU_LEFT;
    else if (input == P_DRIGHT) a = HOS_MENU_RIGHT;
    else if (repeat) a = -1;
    else if (input == ok) a = HOS_MENU_OK;
    else if (input == back) a = HOS_MENU_BACK;
    else if (input == clear) a = HOS_MENU_CLEAR;
    else if (input == reset) a = HOS_MENU_RESET;
    else if (input == P_LSHOULDER) a = HOS_MENU_TAB_PREV;
    else if (input == P_RSHOULDER) a = HOS_MENU_TAB_NEXT;
    if (a >= 0) action_locked(a);
    pthread_mutex_unlock(&lock);
}

void hos_menu_device(int pad) { __atomic_store_n(&pad_last, pad, __ATOMIC_RELAXED); }

void hos_menu_hits(const HosMenuHit *h, int n) {
    pthread_mutex_lock(&lock);
    nhits = n < (int)(sizeof hits / sizeof *hits) ? n : (int)(sizeof hits / sizeof *hits);
    memcpy(hits, h, (size_t)nhits * sizeof *hits);
    pthread_mutex_unlock(&lock);
}

static const HosMenuHit *hit_at(float x, float y) {
    for (int i = nhits - 1; i >= 0; i--)   /* (the last drawn is on top: a question over the rows) */
        if (x >= hits[i].x0 && x < hits[i].x1 && y >= hits[i].y0 && y < hits[i].y1) return &hits[i];
    return NULL;
}

void hos_menu_mouse(float x, float y, int left, int right, float wheel) {
    pthread_mutex_lock(&lock);
    if (!open_) {
        pthread_mutex_unlock(&lock);
        return;
    }
    const int moved = x != mouse_x || y != mouse_y;
    mouse_x = x, mouse_y = y;
    if (moved || left || right || wheel) pad_last = 0;
    const HosMenuHit *h = hit_at(x, y);
    if (mode == HOS_MENU_CAPTURE) {   /* (a click anywhere: never mind) */
        if (left || right) mode = HOS_MENU_NORMAL;
    } else if (mode == HOS_MENU_CONFIRM) {
        if (h && h->row == -2 && moved) q.yes = h->col == 1;
        if (left && h && h->row == -2) answer(h->col == 1);
        else if (right) answer(0);
    } else {
        if (h && h->row >= 0 && selectable(h->row) && (moved || left)) {
            sel = h->row;
            if (rows[sel].kind == HOS_ROW_BINDING && h->col >= 0) col = h->col;
        }
        if (left && h && h->row == -1) tab = h->col;
        if (left && h && h->row >= 0 && selectable(h->row)) {
            const Row *r = &rows[sel];
            if (r->kind == HOS_ROW_SETTING && h->col >= 0)   /* (the value's left half lowers it, its right half raises it) */
                action_locked(x < (h->x0 + h->x1) / 2 ? HOS_MENU_LEFT : HOS_MENU_RIGHT);
            else if (r->kind != HOS_ROW_BINDING || h->col >= 0)
                action_locked(HOS_MENU_OK);
        }
        if (right) action_locked(HOS_MENU_BACK);
        for (int i = 0; i < (int)(wheel > 0 ? wheel : -wheel) && i < 10; i++) move_sel(wheel > 0 ? -1 : 1);
    }
    pthread_mutex_unlock(&lock);
}

/* ---- the snapshot to draw ------------------------------------------------------------------------------------------ */
static void binding_cells(int ctl, HosBinding b, char cell[3][40]) {
    (void)ctl;
    hos_key_name(b.key[0], cell[0], 40);
    hos_key_name(b.key[1], cell[1], 40);
    snprintf(cell[2], 40, "%s", hos_pad_name(b.pad));
}

void hos_menu_view(HosMenuView *v) {
    memset(v, 0, sizeof *v);
    pthread_mutex_lock(&lock);
    if (!nrows) build_rows();
    const int64_t now = hos_now_ns();
    if (mode == HOS_MENU_CAPTURE && now > capture_end) mode = HOS_MENU_NORMAL;
    v->open = open_;
    v->ntabs = NTABS, v->tab = tab;
    for (int i = 0; i < NTABS; i++) v->tabs[i] = tab_names[i];
    v->nrows = nrows, v->sel = sel, v->col = col, v->mode = mode, v->pad = pad_last, v->confirm_yes = q.yes;
    for (int i = 0; i < nrows; i++) {
        HosMenuRow *o = &v->rows[i];
        const Row *r = &rows[i];
        o->kind = r->kind;
        switch (r->kind) {
        case HOS_ROW_SETTING:
            snprintf(o->label, sizeof o->label, "%s", settings_def[r->id].label);
            setting_text(r->id, o->value, sizeof o->value);
            o->changed = settings[r->id] != settings_def[r->id].def;
            break;
        case HOS_ROW_ACTION: snprintf(o->label, sizeof o->label, "Reset all controls"); break;
        case HOS_ROW_HEADING: snprintf(o->label, sizeof o->label, "Switch input"); break;
        case HOS_ROW_BINDING: {
            const HosBinding b = hos_binding(r->id), d = hos_binding_default(r->id);
            snprintf(o->label, sizeof o->label, "%s", hos_control_name(r->id));
            binding_cells(r->id, b, o->cell);
            o->changed = memcmp(&b, &d, sizeof b) != 0;
            break;
        }
        }
    }
    const Row *r = &rows[sel];
    if (r->kind == HOS_ROW_SETTING) {
        snprintf(v->help, sizeof v->help, "%s", settings_def[r->id].help);
    } else if (r->kind == HOS_ROW_ACTION) {
        snprintf(v->help, sizeof v->help, "Every binding back to its default (the keyboard and gamepad map oranges "
                 "starts with). The settings above stay as they are.");
    } else if (r->kind == HOS_ROW_BINDING) {
        char d[3][40];
        binding_cells(r->id, hos_binding_default(r->id), d);
        snprintf(v->help, sizeof v->help, "The keys and the gamepad input that press the Switch's %s. Default: %s%s%s, %s.",
                 hos_control_name(r->id), d[0][0] ? d[0] : "no key", d[1][0] ? " or " : "", d[1],
                 d[2][0] ? d[2] : "no gamepad input");
    }
    if (mode == HOS_MENU_CONFIRM && q.what == Q_RESET_ALL) {
        snprintf(v->question, sizeof v->question, "Put every binding back to its default?");
    } else if (mode == HOS_MENU_CONFIRM) {
        char name[64], n[96];
        if (q.slot == HOS_BIND_KEYS) snprintf(n, sizeof n, "The gamepad's %s", hos_pad_name(q.input));
        else snprintf(n, sizeof n, "%s", hos_key_name(q.input, name, sizeof name));
        snprintf(v->question, sizeof v->question, "%s is already bound to %s. Take it from %s and bind it to %s?", n,
                 hos_control_name(q.other_ctl), hos_control_name(q.other_ctl), hos_control_name(q.ctl));
    }
    if (now < message_end) snprintf(v->message, sizeof v->message, "%s", message);
    if (mode == HOS_MENU_CAPTURE)
        snprintf(v->prompts, sizeof v->prompts, col == HOS_BIND_KEYS ? "Press the gamepad button for %s.   Esc: cancel"
                                                                    : "Press the key for %s.   Esc: cancel",
                 hos_control_name(r->id));
    else if (mode == HOS_MENU_CONFIRM)
        snprintf(v->prompts, sizeof v->prompts, pad_last ? "D-pad: choose   A: answer   B: no" : "Left / Right: choose   Enter: answer   Esc: no");
    else if (pad_last)
        snprintf(v->prompts, sizeof v->prompts, "%s",
                 r->kind == HOS_ROW_BINDING ? "D-pad: choose   A: change   X: clear   Y: default   B: close"
                 : r->kind == HOS_ROW_SETTING ? "D-pad: choose and change   Y: default   B: close"
                                              : "D-pad: choose   A: select   B: close");
    else
        snprintf(v->prompts, sizeof v->prompts, "%s",
                 r->kind == HOS_ROW_BINDING ? "Arrows: choose   Enter: change   Delete: clear   R: default   Esc: close"
                 : r->kind == HOS_ROW_SETTING ? "Arrows: choose and change   R: default   Esc: close"
                                              : "Arrows: choose   Enter: select   Esc: close");
    pthread_mutex_unlock(&lock);
}
