/* Keyboard and mouse bindings shared by src/runtime_pad.c (which reads them) and the in-game
 * menu's key binding rows (gpu/shim/bbport_overlay.cpp, which writes them): the PS4 inputs, the
 * two layouts' default keys and the names a binding line uses. bbport.ini lines
 * key.<input>=<names> (classic layout) and mkkey.<input>=<names> (mouse & keyboard mode) replace
 * an input's default keys; names are SDL key names, the short names in key_from_name and the
 * mouse names below. */
#pragma once

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    IN_CROSS, IN_CIRCLE, IN_SQUARE, IN_TRIANGLE, IN_L1, IN_R1, IN_L2, IN_R2, IN_L3, IN_R3,
    IN_OPTIONS, IN_TOUCHPAD, IN_TOUCHPAD_RIGHT, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
    IN_MOVE_UP, IN_MOVE_DOWN, IN_MOVE_LEFT, IN_MOVE_RIGHT, IN_LOOK_UP, IN_LOOK_DOWN, IN_LOOK_LEFT,
    IN_LOOK_RIGHT, IN_COUNT
};
static const char *const input_names[IN_COUNT]={
    "cross","circle","square","triangle","l1","r1","l2","r2","l3","r3","options","touchpad",
    "touchpad_right","up","down","left","right","move_up","move_down","move_left","move_right",
    "look_up","look_down","look_left","look_right",
};
/* The menu's row labels: English, Russian. */
static const char *const input_labels[IN_COUNT][2]={
    {"Cross (interact)","Крест (действие)"}, {"Circle (dodge, run)","Круг (уклонение, бег)"},
    {"Square (quick item)","Квадрат (быстрый предмет)"}, {"Triangle (blood vial)","Треугольник (флакон крови)"},
    {"L1 (transform)","L1 (трансформация)"}, {"R1 (attack)","R1 (атака)"},
    {"L2 (firearm)","L2 (огнестрел)"}, {"R2 (strong attack)","R2 (сильная атака)"},
    {"L3 (jump while running)","L3 (прыжок на бегу)"}, {"R3 (lock-on)","R3 (захват цели)"},
    {"Options (menu)","Options (меню)"}, {"Touchpad (gestures)","Тачпад (жесты)"},
    {"Touchpad right side","Правая сторона тачпада"},
    {"D-pad up","Крестовина вверх"}, {"D-pad down","Крестовина вниз"},
    {"D-pad left (left weapon)","Крестовина влево (левое оружие)"},
    {"D-pad right (right weapon)","Крестовина вправо (правое оружие)"},
    {"Move forward","Вперёд"}, {"Move back","Назад"}, {"Move left","Влево"}, {"Move right","Вправо"},
    {"Camera up","Камера вверх"}, {"Camera down","Камера вниз"}, {"Camera left","Камера влево"},
    {"Camera right","Камера вправо"},
};

/* Mouse buttons and the wheel as keys (upstream 0.51 #52): codes past SDL's scancodes. */
enum { MOUSE_KEY=SDL_SCANCODE_COUNT, MOUSE_WHEEL_UP=MOUSE_KEY+SDL_BUTTON_X2+1, MOUSE_WHEEL_DOWN };
typedef struct { const char *name; int code; } MouseKeyName;
static const MouseKeyName mouse_keys[]={
    {"Mouse Left",MOUSE_KEY+SDL_BUTTON_LEFT}, {"Mouse Right",MOUSE_KEY+SDL_BUTTON_RIGHT},
    {"Mouse Middle",MOUSE_KEY+SDL_BUTTON_MIDDLE}, {"Mouse X1",MOUSE_KEY+SDL_BUTTON_X1},
    {"Mouse X2",MOUSE_KEY+SDL_BUTTON_X2}, {"Wheel Up",MOUSE_WHEEL_UP}, {"Wheel Down",MOUSE_WHEEL_DOWN},
};
#define MOUSE_KEY_COUNT (sizeof(mouse_keys)/sizeof(*mouse_keys))

typedef struct { int input; int key; } KeyDefault;
/* Mouse & keyboard mode's keys (Mrsuss60/bloodborne_pc_windows_port's scheme); the attacks and
 * L1/L2/R2 are on the mouse (runtime_pad.c sample_mouse). */
static const KeyDefault mk_keys[]={
    {IN_CROSS,SDL_SCANCODE_E}, {IN_CROSS,SDL_SCANCODE_RETURN}, {IN_CROSS,SDL_SCANCODE_KP_ENTER},
    {IN_CIRCLE,SDL_SCANCODE_SPACE}, {IN_SQUARE,SDL_SCANCODE_R}, {IN_TRIANGLE,SDL_SCANCODE_X}, {IN_TRIANGLE,SDL_SCANCODE_F}, {IN_TRIANGLE,SDL_SCANCODE_V},
    {IN_L3,SDL_SCANCODE_C}, {IN_L3,SDL_SCANCODE_Z}, {IN_R3,SDL_SCANCODE_Q},
    {IN_OPTIONS,SDL_SCANCODE_ESCAPE}, {IN_OPTIONS,SDL_SCANCODE_F1},
    {IN_TOUCHPAD,SDL_SCANCODE_TAB}, {IN_TOUCHPAD,SDL_SCANCODE_G}, {IN_TOUCHPAD_RIGHT,SDL_SCANCODE_BACKSPACE},
    {IN_UP,SDL_SCANCODE_UP}, {IN_UP,SDL_SCANCODE_1}, {IN_DOWN,SDL_SCANCODE_DOWN}, {IN_DOWN,SDL_SCANCODE_2},
    {IN_LEFT,SDL_SCANCODE_LEFT}, {IN_LEFT,SDL_SCANCODE_3}, {IN_RIGHT,SDL_SCANCODE_RIGHT}, {IN_RIGHT,SDL_SCANCODE_4},
    {IN_MOVE_UP,SDL_SCANCODE_W}, {IN_MOVE_DOWN,SDL_SCANCODE_S}, {IN_MOVE_LEFT,SDL_SCANCODE_A},
    {IN_MOVE_RIGHT,SDL_SCANCODE_D}, {IN_LOOK_UP,SDL_SCANCODE_I}, {IN_LOOK_DOWN,SDL_SCANCODE_K},
    {IN_LOOK_LEFT,SDL_SCANCODE_J}, {IN_LOOK_RIGHT,SDL_SCANCODE_L},
    {IN_UP,MOUSE_WHEEL_UP}, {IN_DOWN,MOUSE_WHEEL_DOWN}, /* quick items (mk_wheel) */
};
/* The classic layout's keys. */
static const KeyDefault classic_keys[]={
    {IN_CROSS,SDL_SCANCODE_SPACE}, {IN_CROSS,SDL_SCANCODE_RETURN}, {IN_CROSS,SDL_SCANCODE_KP_ENTER},
    {IN_CIRCLE,SDL_SCANCODE_LSHIFT}, {IN_CIRCLE,SDL_SCANCODE_ESCAPE}, {IN_SQUARE,SDL_SCANCODE_E},
    {IN_TRIANGLE,SDL_SCANCODE_Q}, {IN_L1,SDL_SCANCODE_1}, {IN_R1,SDL_SCANCODE_3},
    {IN_L2,SDL_SCANCODE_R}, {IN_R2,SDL_SCANCODE_F}, {IN_L3,SDL_SCANCODE_Z}, {IN_R3,SDL_SCANCODE_C},
    {IN_OPTIONS,SDL_SCANCODE_F1}, {IN_OPTIONS,SDL_SCANCODE_O}, {IN_TOUCHPAD,SDL_SCANCODE_TAB},
    {IN_TOUCHPAD,SDL_SCANCODE_G},
    {IN_TOUCHPAD_RIGHT,SDL_SCANCODE_BACKSPACE},
    {IN_UP,SDL_SCANCODE_UP}, {IN_DOWN,SDL_SCANCODE_DOWN}, {IN_LEFT,SDL_SCANCODE_LEFT}, {IN_RIGHT,SDL_SCANCODE_RIGHT},
    {IN_MOVE_UP,SDL_SCANCODE_W}, {IN_MOVE_DOWN,SDL_SCANCODE_S}, {IN_MOVE_LEFT,SDL_SCANCODE_A},
    {IN_MOVE_RIGHT,SDL_SCANCODE_D}, {IN_LOOK_UP,SDL_SCANCODE_I}, {IN_LOOK_DOWN,SDL_SCANCODE_K},
    {IN_LOOK_LEFT,SDL_SCANCODE_J}, {IN_LOOK_RIGHT,SDL_SCANCODE_L},
};
#define MK_KEY_COUNT (sizeof(mk_keys)/sizeof(*mk_keys))
#define CLASSIC_KEY_COUNT (sizeof(classic_keys)/sizeof(*classic_keys))

/* A binding name's code (key or mouse), -1 when unknown. */
static inline int key_from_name(const char *name) {
    for (size_t i=0;i<MOUSE_KEY_COUNT;++i)
        if (!SDL_strcasecmp(name,mouse_keys[i].name)) return mouse_keys[i].code;
    /* Short names SDL does not know (its own: Right Shift, Return, Escape); issue #2's rshift / enter. */
    static const struct { const char *name; SDL_Scancode code; } aliases[]={
        {"rshift",SDL_SCANCODE_RSHIFT}, {"lshift",SDL_SCANCODE_LSHIFT}, {"shift",SDL_SCANCODE_LSHIFT},
        {"enter",SDL_SCANCODE_RETURN}, {"return",SDL_SCANCODE_RETURN}, {"esc",SDL_SCANCODE_ESCAPE},
        {"escape",SDL_SCANCODE_ESCAPE}, {"ctrl",SDL_SCANCODE_LCTRL}, {"lctrl",SDL_SCANCODE_LCTRL},
        {"rctrl",SDL_SCANCODE_RCTRL}, {"alt",SDL_SCANCODE_LALT}, {"lalt",SDL_SCANCODE_LALT},
        {"ralt",SDL_SCANCODE_RALT}, {"altgr",SDL_SCANCODE_RALT}, {"bksp",SDL_SCANCODE_BACKSPACE},
        {"del",SDL_SCANCODE_DELETE}, {"ins",SDL_SCANCODE_INSERT}, {"pgup",SDL_SCANCODE_PAGEUP},
        {"pgdn",SDL_SCANCODE_PAGEDOWN}, {"pgdown",SDL_SCANCODE_PAGEDOWN},
    };
    for (size_t i=0;i<sizeof(aliases)/sizeof(*aliases);++i)
        if (!SDL_strcasecmp(name,aliases[i].name)) return (int)aliases[i].code;
    const SDL_Scancode s=SDL_GetScancodeFromName(name);
    return s==SDL_SCANCODE_UNKNOWN ? -1 : (int)s;
}
/* The name a binding line uses for a code ("" when it has none). */
static inline const char *key_name(int code) {
    for (size_t i=0;i<MOUSE_KEY_COUNT;++i) if (mouse_keys[i].code==code) return mouse_keys[i].name;
    if (code<=SDL_SCANCODE_UNKNOWN || code>=SDL_SCANCODE_COUNT) return "";
    return SDL_GetScancodeName((SDL_Scancode)code);
}

#ifdef __cplusplus
}
#endif
