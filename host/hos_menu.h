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
   HID usage ids, 0 none); gamepad inputs SDL_GamepadButton values, then the triggers, then the sticks' directions
   (-1 none). Saved as key_<NAME>=<key>,<key> and pad_<NAME>=<input> (SDL's key names; an empty value is "none"; no
   line: the default). */
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
#define HOS_PAD_LSTICK_UP 28        /* the sticks' directions: up, down, left, right of the left stick, then the right's */
#define HOS_PAD_N 36
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
/* the Switch input from the keyboard (keys[]: SDL's key state, by scancode) and the gamepad: its held inputs (a bit
   by gamepad input; a stick direction held past half way) and its sticks (axes[4]: left x, y, right x, y, -32767..32767,
   up positive, after the dead zone). Returns the buttons; stick[4] gets the Switch's sticks: a stick direction bound
   to a gamepad stick's direction takes its amount, to a key or a button all the way. */
uint64_t hos_controls_eval(const bool *keys, int nkeys, uint64_t pad_held, const int32_t axes[4], int32_t stick[4]);
/* the gamepad's held inputs as hos_controls_eval takes them, the stick directions' bits from the axes */
uint64_t hos_pad_stick_bits(const int32_t axes[4]);
/* the gamepad input bound to a Switch button (the menu's confirm and back are A's and B's) */
int hos_control_pad(int ctl);
const char *hos_key_name(int scancode, char *buf, size_t size);   /* "Space", "Left Shift"; "" for none */
const char *hos_pad_name(int input);                              /* "Right (east)", "LT", ...; "" for none */

/* ---- the menu's own settings (hos_menu.c) -----------------------------------------------------------------------
   Graphics: the console mode (HOS_MODE_*: what nn::oe reports, so the resolution the game renders at; HOS_HANDHELD=0/1
   overrides it), full screen 0/1, V-Sync 0/1 (HOS_VSYNC), the filter scaling the picture to the window
   (HOS_FILTER_*). Controls: the stick dead zone in %, the vibration in %, the mouse as touch 0/1. Read them where
   they apply (each read is cheap): a change in the menu takes effect at the next read. */
enum { HOS_SET_CONSOLE_MODE, HOS_SET_FULLSCREEN, HOS_SET_VSYNC, HOS_SET_FILTER,
       HOS_SET_DEAD_ZONE, HOS_SET_VIBRATION, HOS_SET_MOUSE_TOUCH, HOS_SET_N };
enum { HOS_MODE_DOCKED, HOS_MODE_HANDHELD };
enum { HOS_FILTER_LINEAR, HOS_FILTER_NEAREST, HOS_FILTER_INTEGER };
int hos_setting(int id);
void hos_setting_set(int id, int value);   /* (saved; e.g. F11 for full screen) */

/* ---- the menu's model ----------------------------------------------------------------------------------------------- */
enum { HOS_MENU_TOGGLE, HOS_MENU_UP, HOS_MENU_DOWN, HOS_MENU_LEFT, HOS_MENU_RIGHT, HOS_MENU_OK, HOS_MENU_BACK,
       HOS_MENU_TAB_NEXT, HOS_MENU_TAB_PREV, HOS_MENU_CLEAR, HOS_MENU_RESET };
int hos_menu_is_open(void);
int hos_menu_capturing(void);                  /* a binding waits for its input */
void hos_menu_load(void);                      /* (the settings from the options file: at the start) */
void hos_menu_action(int action);
/* a key or a gamepad input pressed while the menu is open (repeat: held down, again): a binding waiting for its
   input takes it, else it moves around the menu. F1, a gamepad's Home and Select + Start open and close the menu
   (the caller's) */
void hos_menu_key(int scancode, int shift, int repeat);
void hos_menu_pad(int input, int repeat);
/* the mouse in the console's 1280x720 screen (over the picture): moved, a left click, a right click, wheel notches
   (up positive) */
void hos_menu_mouse(float x, float y, int left, int right, float wheel);
void hos_menu_device(int pad);                 /* the last input came from a gamepad (1) or the keyboard / mouse (0) */
/* the connected gamepads, for the Controller row (shown while there is one): their names and keys (SDL's GUID of the
   model, "#2" ... after it for a second one of the same model), and which one the game has (-1 none) */
#define HOS_MENU_PADS 8
void hos_menu_pads(int n, const char names[][64], const char keys[][40], int in_use);
/* the chosen gamepad's key ("" Auto: Steam Input's pad if there is one, else the first connected), and a number that
   changes each time the choice does */
int hos_menu_controller(char *key, size_t size);

enum { HOS_ROW_SETTING, HOS_ROW_ACTION, HOS_ROW_BINDING, HOS_ROW_HEADING, HOS_ROW_CONTROLLER };
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
