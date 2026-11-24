/* host/hos_options.c, hos_controls.c and hos_menu.c, built alone (no SDL, no game) by tests/test_host_menu.py.
   argv[1]: the options file (HOS_OPTIONS_FILE); argv[2]: the step ("first": change things; "again": a new process
   reads what the first wrote). Prints "ok" or the first check that failed. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hos_menu.h"
#include "hos_options.h"

void hos_log(const char *fmt, ...) {
    (void)fmt;
}
int64_t hos_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

#define CHECK(c) do { if (!(c)) { printf("failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)
enum { K_SPACE = 44, K_RETURN = 40, K_Q = 20, K_W = 26, K_F = 9, K_ESCAPE = 41 };

static int row_of(const HosMenuView *v, const char *label) {
    for (int i = 0; i < v->nrows; i++)
        if (!strcmp(v->rows[i].label, label)) return i;
    return -1;
}
static void select_row(const char *label) {   /* (its tab, then from the top, with Down) */
    HosMenuView v;
    hos_menu_view(&v);
    for (int t = 0; t < v.ntabs && row_of(&v, label) < 0; t++) hos_menu_action(HOS_MENU_TAB_NEXT), hos_menu_view(&v);
    for (int i = 0; i < HOS_MENU_ROWS; i++) hos_menu_action(HOS_MENU_UP);
    hos_menu_view(&v);
    const int want = row_of(&v, label);
    for (int i = 0; i < HOS_MENU_ROWS && v.sel != want; i++) hos_menu_action(HOS_MENU_DOWN), hos_menu_view(&v);
}

static int first(void) {
    bool keys[512] = {0};
    int32_t stick[4];
    /* the defaults */
    const int32_t rest[4] = {0, 0, 0, 0};
    keys[K_SPACE] = keys[K_W] = 1;
    CHECK(hos_controls_eval(keys, 512, 0, rest, stick) == (1u << 0));   /* A */
    CHECK(stick[0] == 0 && stick[1] == 32767);                           /* W: the left stick up */
    keys[K_SPACE] = keys[K_W] = 0;
    CHECK(hos_controls_eval(keys, 512, 1u << 1, rest, stick) == (1u << 0));   /* the gamepad's right button: A */
    CHECK(hos_controls_eval(keys, 512, 1u << HOS_PAD_LT, rest, stick) == (1u << 8));   /* LT: ZL */
    /* the gamepad's sticks through their bindings: analog, each to itself by default */
    const int32_t tilted[4] = {-12000, 20000, 30000, -5000};
    CHECK(hos_controls_eval(keys, 512, hos_pad_stick_bits(tilted), tilted, stick) == 0);
    CHECK(stick[0] == -12000 && stick[1] == 20000 && stick[2] == 30000 && stick[3] == -5000);
    /* swapped: the right stick's up on the left stick's up; and D-pad up on the left stick pushed up past half */
    hos_bind(HOS_CTL_LSTICK_UP, 2, HOS_PAD_LSTICK_UP + 4);
    hos_bind(HOS_CTL_UP, 2, HOS_PAD_LSTICK_UP);
    const int32_t up[4] = {0, 30000, 0, 9000};
    CHECK(hos_controls_eval(keys, 512, hos_pad_stick_bits(up), up, stick) == (1u << 13));   /* D-pad up */
    CHECK(stick[1] == 9000 && stick[3] == 9000);
    hos_binding_reset(HOS_CTL_LSTICK_UP);
    hos_binding_reset(HOS_CTL_UP);
    int ctl, slot;
    CHECK(hos_binding_owner(0, K_Q, -1, &ctl, &slot) && ctl == HOS_CTL_L && slot == 0);

    /* the menu opens on the Graphics tab: Console mode, docked by default, handheld one step right (saved) */
    hos_menu_load();
    hos_menu_action(HOS_MENU_TOGGLE);
    CHECK(hos_menu_is_open());
    HosMenuView v;
    hos_menu_view(&v);
    CHECK(v.ntabs == 2 && v.tab == 0 && !strcmp(v.tabs[0], "Graphics") && !strcmp(v.tabs[1], "Controls"));
    CHECK(row_of(&v, "B") < 0 && v.sel == row_of(&v, "Console mode"));
    CHECK(!strcmp(v.rows[v.sel].value, "Docked (1920x1080)") && !v.rows[v.sel].changed);
    CHECK(hos_setting(HOS_SET_CONSOLE_MODE) == HOS_MODE_DOCKED);
    hos_menu_action(HOS_MENU_RIGHT);
    hos_menu_view(&v);
    CHECK(!strcmp(v.rows[v.sel].value, "Handheld (1280x720)") && v.rows[v.sel].changed);
    CHECK(hos_setting(HOS_SET_CONSOLE_MODE) == HOS_MODE_HANDHELD);
    select_row("Scaling");
    hos_menu_view(&v);
    CHECK(row_of(&v, "Sharpness") < 0);   /* (only with FSR 1) */
    hos_menu_action(HOS_MENU_OK), hos_menu_action(HOS_MENU_OK), hos_menu_action(HOS_MENU_OK);
    hos_menu_view(&v);
    CHECK(hos_setting(HOS_SET_FILTER) == HOS_FILTER_FSR && !strcmp(v.rows[v.sel].value, "FSR 1"));
    CHECK(row_of(&v, "Sharpness") == v.sel + 1 && !strcmp(v.rows[v.sel + 1].value, "80%"));
    hos_menu_action(HOS_MENU_DOWN), hos_menu_action(HOS_MENU_RIGHT), hos_menu_action(HOS_MENU_RIGHT);
    CHECK(hos_setting(HOS_SET_SHARPNESS) == 100);   /* (no higher) */
    hos_menu_action(HOS_MENU_UP);
    hos_menu_action(HOS_MENU_OK);   /* (round to the first) */
    hos_menu_view(&v);
    CHECK(hos_setting(HOS_SET_FILTER) == HOS_FILTER_LINEAR && row_of(&v, "Sharpness") < 0);
    CHECK(v.sel == row_of(&v, "Scaling"));
    hos_menu_action(HOS_MENU_LEFT);   /* (no lower) */
    hos_menu_view(&v);
    CHECK(!strcmp(v.rows[v.sel].value, "Smooth"));
    hos_setting_set(HOS_SET_FULLSCREEN, 1);   /* (F11) */
    select_row("Full screen");
    hos_menu_view(&v);
    CHECK(!strcmp(v.rows[v.sel].value, "On"));

    /* the Controls tab: Q for B's first key, which L has: asked, then moved */
    select_row("B");
    hos_menu_view(&v);
    CHECK(v.tab == 1);
    CHECK(v.rows[v.sel].kind == HOS_ROW_BINDING && !strcmp(v.rows[v.sel].cell[1], "#42"));   /* Backspace (no SDL names) */
    hos_menu_key(K_RETURN, 0, 0);
    hos_menu_view(&v);
    CHECK(v.mode == HOS_MENU_CAPTURE);
    hos_menu_key(K_Q, 0, 0);
    hos_menu_view(&v);
    CHECK(v.mode == HOS_MENU_CONFIRM && strstr(v.question, "already bound to L"));
    CHECK(v.confirm_yes == 0);              /* (No first) */
    hos_menu_action(HOS_MENU_RIGHT);
    hos_menu_action(HOS_MENU_OK);
    hos_menu_view(&v);
    CHECK(v.mode == HOS_MENU_NORMAL && v.rows[v.sel].changed);
    CHECK(hos_binding(HOS_CTL_B).key[0] == K_Q && hos_binding(HOS_CTL_L).key[0] == 0);
    keys[K_Q] = 1;
    CHECK(hos_controls_eval(keys, 512, 0, rest, stick) == (1u << 1));   /* Q: B now, no longer L */
    keys[K_Q] = 0;

    /* Esc cancels a capture */
    hos_menu_key(K_RETURN, 0, 0);
    hos_menu_key(K_ESCAPE, 0, 0);
    hos_menu_view(&v);
    CHECK(v.mode == HOS_MENU_NORMAL && hos_binding(HOS_CTL_B).key[0] == K_Q);
    /* the gamepad cell: F (a key) is not taken there; the left button is Y's: asked, answered no */
    hos_menu_action(HOS_MENU_RIGHT), hos_menu_action(HOS_MENU_RIGHT);
    hos_menu_key(K_RETURN, 0, 0);
    hos_menu_key(K_F, 0, 0);
    hos_menu_view(&v);
    CHECK(v.mode == HOS_MENU_CAPTURE);
    hos_menu_pad(2, 0);
    hos_menu_view(&v);
    CHECK(v.mode == HOS_MENU_CONFIRM && strstr(v.question, "bound to Y"));
    hos_menu_action(HOS_MENU_BACK);
    CHECK(hos_binding(HOS_CTL_B).pad == 0 && hos_binding(HOS_CTL_Y).pad == 2);
    /* Delete clears the cell */
    hos_menu_action(HOS_MENU_CLEAR);
    CHECK(hos_binding(HOS_CTL_B).pad == -1);

    /* a stick direction taken by the gamepad cell: the right stick's down for the left stick's up (the right stick's
       own row has it: asked, yes) */
    select_row("Left stick up");
    hos_menu_action(HOS_MENU_OK);
    CHECK(hos_menu_capturing());
    hos_menu_pad(HOS_PAD_LSTICK_UP + 5, 0);
    hos_menu_view(&v);
    CHECK(v.mode == HOS_MENU_CONFIRM && strstr(v.question, "bound to Right stick down"));
    hos_menu_action(HOS_MENU_LEFT), hos_menu_action(HOS_MENU_OK);
    CHECK(hos_binding(HOS_CTL_LSTICK_UP).pad == HOS_PAD_LSTICK_UP + 5 && hos_binding(HOS_CTL_RSTICK_DOWN).pad == -1);
    hos_menu_view(&v);
    CHECK(!strcmp(v.rows[v.sel].cell[2], "Right stick down"));
    hos_binding_reset(HOS_CTL_LSTICK_UP), hos_binding_reset(HOS_CTL_RSTICK_DOWN);

    /* the Controller row: there while gamepads are connected (the selection stays on its row), Auto first */
    const char names[2][64] = {"Steam Controller", "Pro Controller"};
    const char pad_keys[2][40] = {"03000000de2800000512000000000000", "050000007e0500000920000000000000"};
    select_row("Stick dead zone");
    hos_menu_pads(2, names, pad_keys, 0);
    hos_menu_view(&v);
    CHECK(v.rows[0].kind == HOS_ROW_CONTROLLER && !strcmp(v.rows[0].value, "Auto: Steam Controller"));
    CHECK(!strcmp(v.rows[v.sel].label, "Stick dead zone"));
    char key[40];
    const int gen = hos_menu_controller(key, sizeof key);
    CHECK(key[0] == 0);
    select_row("Controller");
    hos_menu_action(HOS_MENU_RIGHT), hos_menu_action(HOS_MENU_RIGHT);   /* (Auto, Steam Controller, Pro Controller) */
    CHECK(hos_menu_controller(key, sizeof key) != gen && !strcmp(key, pad_keys[1]));
    char saved[64];
    CHECK(hos_option("controller", saved, sizeof saved) && !strcmp(saved, pad_keys[1]));
    hos_menu_pads(1, names, pad_keys, 0);   /* (the Pro Controller gone: kept, shown as away) */
    hos_menu_view(&v);
    CHECK(!strcmp(v.rows[0].value, "Pro Controller (not connected)"));
    hos_menu_action(HOS_MENU_RESET);        /* (back to Auto) */
    CHECK(hos_menu_controller(key, sizeof key) && key[0] == 0 && !hos_option("controller", saved, sizeof saved));
    hos_menu_pads(0, names, pad_keys, -1);
    hos_menu_view(&v);
    CHECK(v.rows[0].kind != HOS_ROW_CONTROLLER && !strcmp(v.rows[v.sel].label, "Stick dead zone"));

    /* a setting: the dead zone up one step, saved */
    select_row("Stick dead zone");
    hos_menu_action(HOS_MENU_RIGHT);
    CHECK(hos_setting(HOS_SET_DEAD_ZONE) == 15);
    /* Back closes */
    hos_menu_action(HOS_MENU_BACK);
    CHECK(!hos_menu_is_open());
    /* the options file keeps a line it does not know */
    hos_option_set("future_setting", "7");
    return 0;
}

static int again(void) {
    /* what the first run saved */
    CHECK(hos_binding(HOS_CTL_B).key[0] == K_Q && hos_binding(HOS_CTL_B).pad == -1);
    CHECK(hos_binding(HOS_CTL_L).key[0] == 0);
    CHECK(hos_setting(HOS_SET_DEAD_ZONE) == 15);
    CHECK(hos_option_int("future_setting", 0) == 7);
    CHECK(hos_option_int("fullscreen", 0) == 1 && hos_option_int("console_mode", 0) == 1);
    /* the environment's HOS_HANDHELD=0 for this run: docked, the file's handheld kept */
    CHECK(hos_setting(HOS_SET_CONSOLE_MODE) == HOS_MODE_DOCKED && hos_setting(HOS_SET_FULLSCREEN) == 1);
    /* Reset all: every key_ / pad_ line gone, the rest kept */
    hos_bindings_reset_all();
    char v[64];
    CHECK(!hos_option("key_B", v, sizeof v) && !hos_option("key_L", v, sizeof v) && !hos_option("pad_B", v, sizeof v));
    CHECK(hos_option_int("stick_dead_zone", 0) == 15 && hos_option_int("future_setting", 0) == 7);
    CHECK(hos_binding(HOS_CTL_L).key[0] == K_Q);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    setenv("HOS_OPTIONS_FILE", argv[1], 1);
    if (!strcmp(argv[2], "again")) setenv("HOS_HANDHELD", "0", 1);
    else unsetenv("HOS_HANDHELD");
    const int r = !strcmp(argv[2], "first") ? first() : again();
    if (!r) printf("ok\n");
    return r;
}
