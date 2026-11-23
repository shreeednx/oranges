/* The settings menu (F1): its controls bindings (hos_controls.c), its model and input (hos_menu.c); drawn by the
   Vulkan back end (host/vk/vk_menu.cpp). C, and C++ for the drawing. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the controls (hos_controls.c) ---------------------------------------------------------------------------------
   Each Switch input (a button, a direction of a stick) has two keys and one gamepad input. Keys are SDL scancodes (USB
   HID usage ids, 0 none); gamepad inputs SDL_GamepadButton values, the triggers after them (-1 none). Saved as
   key_<NAME>=<key>,<key> and pad_<NAME>=<input> (SDL's key names; an empty value is "none"; no line: the default). */
enum {
    HOS_CTL_A, HOS_CTL_B, HOS_CTL_X, HOS_CTL_Y, HOS_CTL_L, HOS_CTL_R, HOS_CTL_ZL, HOS_CTL_ZR, HOS_CTL_PLUS, HOS_CTL_MINUS,
    HOS_CTL_LS, HOS_CTL_RS, HOS_CTL_UP, HOS_CTL_DOWN, HOS_CTL_LEFT, HOS_CTL_RIGHT,
    HOS_CTL_LSTICK_UP, HOS_CTL_LSTICK_DOWN, HOS_CTL_LSTICK_LEFT, HOS_CTL_LSTICK_RIGHT,
    HOS_CTL_RSTICK_UP, HOS_CTL_RSTICK_DOWN, HOS_CTL_RSTICK_LEFT, HOS_CTL_RSTICK_RIGHT,
    HOS_CTL_N
};
#define HOS_BIND_KEYS 2
#define HOS_PAD_BUTTONS 26          /* (SDL_GAMEPAD_BUTTON_COUNT) */
#define HOS_PAD_LT 26               /* the triggers, as buttons (pressed past a third) */
#define HOS_PAD_RT 27
#define HOS_PAD_N 28
typedef struct { int key[HOS_BIND_KEYS]; int pad; } HosBinding;

void hos_controls_load(void);                         /* (from the options file: at the start) */
const char *hos_control_name(int ctl);                /* "A", "D-pad up", "Left stick up", ... */
const char *hos_control_id(int ctl);                  /* its options-file name: "A", "LSTICK_UP", ... */
uint64_t hos_control_bit(int ctl);                    /* its HOS_BTN_* bit */
HosBinding hos_binding(int ctl);
HosBinding hos_binding_default(int ctl);
/* slot 0, 1: a key; 2: the gamepad input. Saved at once. */
void hos_bind(int ctl, int slot, int input);
void hos_binding_reset(int ctl);
void hos_bindings_reset_all(void);
/* which control and slot has this input (another than skip_ctl): 0 when none */
int hos_binding_owner(int slot_kind_pad, int input, int skip_ctl, int *ctl, int *slot);
/* the Switch input from the keyboard (keys[]: SDL's key state, by scancode, 512 of them) and the gamepad's held
   inputs (bits by gamepad input): the buttons, and the stick deflections the keys and buttons make (-32767..32767) */
uint64_t hos_controls_eval(const bool *keys, int nkeys, uint32_t pad_held, int32_t stick[4]);
/* the gamepad input bound to a Switch button (the menu's confirm and back are A's and B's) */
int hos_control_pad(int ctl);
const char *hos_key_name(int scancode, char *buf, size_t size);   /* "Space", "Left Shift"; "" for none */
const char *hos_pad_name(int input);                              /* "Right (east)", "LT", ...; "" for none */

/* ---- the menu's own settings (hos_menu.c) ----------------------------------------------------------------------- */
enum { HOS_SET_DEAD_ZONE, HOS_SET_VIBRATION, HOS_SET_MOUSE_TOUCH, HOS_SET_N };
int hos_setting(int id);   /* (the stick dead zone in %, the vibration in %, the mouse as touch 0/1) */

/* ---- the menu's model ----------------------------------------------------------------------------------------------- */
enum { HOS_MENU_TOGGLE, HOS_MENU_UP, HOS_MENU_DOWN, HOS_MENU_LEFT, HOS_MENU_RIGHT, HOS_MENU_OK, HOS_MENU_BACK,
       HOS_MENU_TAB_NEXT, HOS_MENU_TAB_PREV, HOS_MENU_CLEAR, HOS_MENU_RESET };
int hos_menu_is_open(void);
void hos_menu_load(void);                      /* (the settings from the options file: at the start) */
void hos_menu_action(int action);
/* a key or a gamepad input pressed while the menu is open (repeat: held down, again): a binding waiting for its
   input takes it, else it moves around the menu. F1 and a gamepad's Home open and close the menu (the caller's) */
void hos_menu_key(int scancode, int shift, int repeat);
void hos_menu_pad(int input, int repeat);
/* the mouse in the console's 1280x720 screen (over the picture): moved, a left click, a right click, wheel notches
   (up positive) */
void hos_menu_mouse(float x, float y, int left, int right, float wheel);
void hos_menu_device(int pad);                 /* the last input came from a gamepad (1) or the keyboard / mouse (0) */

enum { HOS_ROW_SETTING, HOS_ROW_ACTION, HOS_ROW_BINDING, HOS_ROW_HEADING };
enum { HOS_MENU_NORMAL, HOS_MENU_CAPTURE, HOS_MENU_CONFIRM };
#define HOS_MENU_ROWS 40
typedef struct {
    int kind;                 /* HOS_ROW_* */
    char label[48];
    char value[48];           /* (setting rows: the value shown) */
    char cell[3][40];         /* (binding rows: key, key, gamepad input; "" for none) */
    bool changed;             /* not the default */
    bool restart;             /* applies after a restart */
} HosMenuRow;
typedef struct {
    bool open;
    int ntabs, tab;
    const char *tabs[4];
    int nrows, sel, col;      /* (col: the binding row's cell) */
    HosMenuRow rows[HOS_MENU_ROWS];
    int mode;                 /* HOS_MENU_*: a binding waiting for its input (capture), a question (confirm) */
    int confirm_yes;          /* (the question's answer under the cursor: 1 yes) */
    bool pad;                 /* a gamepad was the last input */
    char help[256];           /* what the selected row does */
    char question[200];
    char message[160];        /* (a note after a change, for a few seconds) */
    char prompts[200];        /* the keys or buttons at the bottom */
} HosMenuView;
void hos_menu_view(HosMenuView *v);
/* where the renderer drew things, in the console's 1280x720 screen, for the mouse: a row's cell (col -1: the row's
   name, 0-2 a binding row's cell or a setting row's value), a tab (row -1, col the tab), the question's answers (row -2,
   col 1 yes) */
typedef struct { float x0, y0, x1, y1; int row, col; } HosMenuHit;
void hos_menu_hits(const HosMenuHit *hits, int n);

#ifdef __cplusplus
}
#endif
