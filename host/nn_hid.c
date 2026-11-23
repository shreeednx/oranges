/* nn::hid: controllers, touch screen, mouse, keyboard and motion, HLE'd at the API level.

   One Pro Controller (Npad No.1, full key) is connected, a USB mouse and keyboard too, and the touch screen is there.
   What they do is the window's (hos_window.c: hos_input, the keyboard and a gamepad as the pad, the mouse, its left
   button as a finger) and HOS_INPUT_SCRIPT's. Every state carries a sampling number that goes up with each read, as
   the console's do (the game drops states it has seen).

   Layouts, from Longshot's host/nn_hid.c (https://git.magicbean.dev/MagicBeanDev/Longshot, commit 67ba729d,
   GPL-3.0-or-later) and the SDK:
     Npad states (NpadCommonState): {s64 sampling; u64 buttons; s32 lx, ly, rx, ry; u32 attributes; pad}: 0x28 bytes
     TouchScreenState<N>: {s64 sampling; s32 count; s32 reserved; TouchState[N] (0x28 each)}
     TouchState: {s64 delta time; u32 attributes; s32 finger id, x, y, diameter x, diameter y, rotation; pad}
     MouseState: {s64 sampling; s32 x, y, dx, dy, wheel dx, wheel dy; u32 buttons, attributes}: 0x28 bytes
     KeyboardState: {s64 sampling; u32 modifiers, attributes; u8 keys[32]}: 0x30 bytes
     SixAxisSensorState: {s64 delta time; s64 sampling; f32 acceleration[3], angular velocity[3], angle[3],
       direction[3][3]; u32 attributes; pad}: 0x60 bytes
     VibrationDeviceHandle: {u8 style (3 full key), u8 npad id, u8 device index, u8 reserved} */
#include "hos.h"

#include <string.h>

#define NPAD_NO1 0
#define STYLE_FULLKEY 1u
#define ATTR_CONNECTED 1u
#define STATE_SIZE 0x28


static int64_t next_sampling(void) {
    static int64_t s;
    return __atomic_add_fetch(&s, 1, __ATOMIC_RELAXED);
}
static int clamp_count(int64_t n, int max) { return n < 0 ? 0 : n > max ? max : (int)n; }

/* ---- Npads ------------------------------------------------------------------------------------------------------ */
static uint32_t supported_styles = STYLE_FULLKEY;
/* GetNpadStates(State* out, int count, unsigned int const& npadId) -> how many written, the latest first */
static void npad_states(Ctx *c) {
    const int count = clamp_count((int32_t)ARG(1), 16);
    if (!hos_guest_range(ARG(0), (uint64_t)count * STATE_SIZE) || !hos_guest_range(ARG(2), 4)) { RET(0); return; }
    const int connected = G32(ARG(2)) == NPAD_NO1;
    HosInput in;
    hos_input(&in);
    const int64_t s = next_sampling();
    for (int i = 0; i < count; i++) {
        const uint64_t e = ARG(0) + (uint64_t)i * STATE_SIZE;
        memset(GP(e), 0, STATE_SIZE);
        G64(e) = (uint64_t)(s - i);
        if (!connected) continue;
        G64(e + 8) = in.buttons;
        memcpy(GP(e + 16), in.stick, 16);
        G32(e + 32) = ATTR_CONNECTED;
    }
    RET(count);
}
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid15GetNpadStyleSetERKj) { RET(G32(ARG(0)) == NPAD_NO1 ? STYLE_FULLKEY : 0); }
HLE(_ZN2nn3hid14InitializeNpadEv) { (void)c; }
HLE(_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm) { (void)c; }
HLE(_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) { supported_styles = (uint32_t)ARG(0); }
HLE(_ZN2nn3hid24GetSupportedNpadStyleSetEv) { RET(supported_styles); }
HLE(_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE) { (void)c; }
HLE(_ZN2nn3hid20GetNpadJoyAssignmentERKj) { RET(0); }   /* NpadJoyAssignmentMode_Dual */
HLE(_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj) { (void)c; }
HLE(_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj) { (void)c; }
HLE(_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE) { (void)c; }
HLE(_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_) { RET(0); }
HLE(_ZN2nn3hid18SwapNpadAssignmentERKjS2_) { RET(0); }
HLE(_ZN2nn3hid19GetPlayerLedPatternERKj) { RET(G32(ARG(0)) < 8 ? 1u << G32(ARG(0)) : 0); }
/* GetNpadControllerColor(NpadControllerColor* left/main, NpadControllerColor* right/sub, id): {main, sub} RGBA each */
HLE(_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj) {
    for (int k = 0; k < 2; k++)
        if (ARG(k)) G32(ARG(k)) = 0xFF2D2D2Du, G32(ARG(k) + 4) = 0xFFE6E6E6u;
    RET(G32(ARG(2)) == NPAD_NO1 ? 0 : HOS_RESULT(202, 604));
}
/* the controller applet: one player, on No.1 */
HLE(_ZN2nn3hid20ControllerSupportArg10SetDefaultEv) { memset(GP(ARG(0)), 0, 0x8); }
HLE(_ZN2nn3hid14SetExplainTextEPNS0_20ControllerSupportArgEPKcRKj) { (void)c; }
HLE(_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE) {
    memset(GP(ARG(0)), 0, 8);
    *(uint8_t *)GP(ARG(0)) = 1;
    G32(ARG(0) + 4) = NPAD_NO1;
    RET(0);
}

/* ---- touch screen, mouse, keyboard -------------------------------------------------------------------------------- */
static void touch_state(uint64_t e, int capacity, int64_t sampling, const HosInput *in) {
    memset(GP(e), 0, 16 + 0x28ull * (uint64_t)capacity);
    G64(e) = (uint64_t)sampling;
    if (!in->touch || capacity < 1) return;
    G32(e + 8) = 1;
    const uint64_t t = e + 16;
    G32(t + 12) = (uint32_t)in->touch_x, G32(t + 16) = (uint32_t)in->touch_y;   /* finger 0, delta time 0 */
    G32(t + 20) = 15, G32(t + 24) = 15;   /* (a fingertip's size) */
}
HLE(_ZN2nn3hid21InitializeTouchScreenEv) { (void)c; }
HLE(_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE) {
    HosInput in;
    hos_input(&in);
    touch_state(ARG(0), 1, next_sampling(), &in);
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    const uint64_t size = 16 + 16 * 0x28;
    const int count = clamp_count((int32_t)ARG(1), 16);
    if (!hos_guest_range(ARG(0), size * (uint64_t)count)) { RET(0); return; }
    HosInput in;
    hos_input(&in);
    const int64_t s = next_sampling();
    for (int i = 0; i < count; i++) touch_state(ARG(0) + size * (uint64_t)i, 16, s - i, &in);
    RET(count);
}
HLE(_ZN2nn3hid15InitializeMouseEv) { (void)c; }
/* GetMouseStates(MouseState* out, int count): the latest first; the deltas since the read before */
HLE(_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi) {
    static int32_t last_x, last_y, last_wheel;
    const int count = clamp_count((int32_t)ARG(1), 16);
    if (!hos_guest_range(ARG(0), 0x28ull * (uint64_t)count)) { RET(0); return; }
    HosInput in;
    hos_input(&in);
    const int32_t dx = in.mouse_x - last_x, dy = in.mouse_y - last_y, dw = in.wheel - last_wheel;
    last_x = in.mouse_x, last_y = in.mouse_y, last_wheel = in.wheel;
    const int64_t s = next_sampling();
    for (int i = 0; i < count; i++) {
        const uint64_t e = ARG(0) + 0x28ull * (uint64_t)i;
        memset(GP(e), 0, 0x28);
        G64(e) = (uint64_t)(s - i);
        G32(e + 8) = (uint32_t)in.mouse_x, G32(e + 12) = (uint32_t)in.mouse_y;
        if (!i) G32(e + 16) = (uint32_t)dx, G32(e + 20) = (uint32_t)dy, G32(e + 28) = (uint32_t)dw;
        G32(e + 32) = in.mouse_buttons;
        G32(e + 36) = (in.mouse_in ? 1u : 0) | 2u;   /* MouseAttribute: Transferable (over the picture), IsConnected */
    }
    RET(count);
}
HLE(_ZN2nn3hid18InitializeKeyboardEv) { (void)c; }
HLE(_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE) {
    HosInput in;
    hos_input(&in);
    memset(GP(ARG(0)), 0, 0x30);
    G64(ARG(0)) = (uint64_t)next_sampling();
    G32(ARG(0) + 8) = in.modifiers;
    G32(ARG(0) + 12) = 1;   /* KeyboardAttribute: IsConnected */
    memcpy(GP(ARG(0) + 16), in.keys, 32);
}
/* GetKeyCode(u16* out, int usage, KeyboardModifierSet, KeyboardLayout): the character of a key, US layout (any
   layout asked), or 0 */
HLE(_ZN2nn3hid10GetKeyCodeEPtiNS_4util10BitFlagSetILi32ENS0_16KeyboardModifierEEENS2_7BitPackIjNS0_14KeyboardLayoutEEE) {
    static const char plain[] = "1234567890\n\x1b\b\t -=[]\\#;'`,./", shifted[] = "!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
    const int usage = (int)ARG(1);
    const uint32_t mods = (uint32_t)ARG(2);
    const int shift = (mods >> 1) & 1, caps = (mods >> 8) & 1;
    uint16_t code = 0;
    if (usage >= 4 && usage <= 29) code = (uint16_t)((shift ^ caps ? 'A' : 'a') + usage - 4);
    else if (usage >= 30 && usage < 30 + (int)sizeof plain - 1) code = (uint8_t)(shift ? shifted : plain)[usage - 30];
    if (ARG(0)) *(uint16_t *)GP(ARG(0)) = code;
}

/* ---- motion: a pad lying still ------------------------------------------------------------------------------------ */
HLE(_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    const int count = clamp_count((int32_t)ARG(1), 2);
    for (int i = 0; i < count; i++) G32(ARG(0) + 4ull * (uint64_t)i) = 0x100u + (uint32_t)i;
    RET(count);
}
HLE(_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE) { (void)c; }
HLE(_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE) {
    const int count = clamp_count((int32_t)ARG(1), 16);
    if (!hos_guest_range(ARG(0), 0x60ull * (uint64_t)count)) { RET(0); return; }
    const int64_t s = next_sampling();
    for (int i = 0; i < count; i++) {
        uint8_t *e = GP(ARG(0) + 0x60ull * (uint64_t)i);
        memset(e, 0, 0x60);
        const int64_t dt = 5000000, sn = s - i;
        memcpy(e, &dt, 8);
        memcpy(e + 8, &sn, 8);
        const float g = -1.0f, one = 1.0f;
        memcpy(e + 16 + 8, &g, 4);                                      /* acceleration z: lying flat */
        for (int k = 0; k < 3; k++) memcpy(e + 52 + 4 * (4 * k), &one, 4);   /* direction: identity */
        const uint32_t attr = 1;
        memcpy(e + 88, &attr, 4);
    }
    RET(count);
}

/* ---- vibration: handles for the SDK's vibration classes, and the values to the gamepad (hos_window.c) ------------ */
HLE(_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    const int count = clamp_count((int32_t)ARG(1), 2);
    for (int i = 0; i < count; i++) {
        uint8_t *h = GP(ARG(0) + 4ull * (uint64_t)i);
        h[0] = 3, h[1] = (uint8_t)G32(ARG(2)), h[2] = (uint8_t)i, h[3] = 0;
    }
    RET(count);
}
HLE(_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE) { (void)c; }
/* SendVibrationValue(handle, VibrationValue {f32 amplitude low, frequency low, amplitude high, frequency high}): to the
   gamepad's motors, the left device's to the low one and the right's to the high one, each its stronger band */
HLE(_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE) {
    const uint8_t *h = GP(ARG(0));
    float v[4];
    memcpy(v, GP(ARG(1)), sizeof v);
    hos_rumble(h[2] ? 1 : 0, v[0] > v[2] ? v[0] : v[2]);
}
