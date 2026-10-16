/* Button prompts: which icon set the game's key guide shows and the labels on the icons
 * (src/runtime_pad.c feeds it, gpu/shim/bbport_button_prompts.cpp draws it). Derived from
 * deadinside28/bloodborne_pc 72e5c8ce (src/runtime_pad.c's prompt part, GPL-2.0-or-later); the
 * sets, the style setting and the Nintendo letters are this port's (GPL-3.0-or-later).
 *
 * Sets: PlayStation (the game's own icons, untouched), Xbox, Nintendo, Keyboard (keys and mouse
 * buttons). The device used last picks keyboard or gamepad; the gamepad's set comes from its SDL
 * type (PS3/PS4/PS5 PlayStation, Switch and GameCube pads Nintendo, every other Xbox) unless
 * prompt_style names one. Face button letters come from SDL's labels for the set's pad type, never
 * from the position: with the Nintendo set the bottom button (the game's confirm, Cross) shows B.
 * Only the letters change; what each button does stays. */
#pragma once

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PROMPT_SET_PLAYSTATION, /* the game's own icons */
    PROMPT_SET_XBOX,
    PROMPT_SET_NINTENDO,
    PROMPT_SET_KEYBOARD,
    PROMPT_SET_COUNT
};
/* bbport.ini prompt_style / BB_PROMPT_STYLE (BbSettings::Values::prompt_style holds the index). */
enum {
    PROMPT_STYLE_AUTO, PROMPT_STYLE_PLAYSTATION, PROMPT_STYLE_XBOX, PROMPT_STYLE_NINTENDO,
    PROMPT_STYLE_KEYBOARD, PROMPT_STYLE_COUNT
};
static const char *const prompt_set_names[PROMPT_SET_COUNT]={"PlayStation","Xbox","Nintendo","Keyboard"};

/* The pad's own set (prompt_style Auto). */
static inline int prompt_set_for_type(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5:
        return PROMPT_SET_PLAYSTATION;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        return PROMPT_SET_NINTENDO;
    default:
        return PROMPT_SET_XBOX;
    }
}
/* The set shown: `keyboard_last` when the keyboard or mouse gave the last input. Keyboard style
 * shows keys always; PlayStation / Xbox / Nintendo name the gamepad's set (the keyboard still
 * shows keys while it is used). */
static inline int prompt_set_choose(int style, int keyboard_last, SDL_GamepadType type) {
    if (style==PROMPT_STYLE_KEYBOARD || keyboard_last) return PROMPT_SET_KEYBOARD;
    switch (style) {
    case PROMPT_STYLE_PLAYSTATION: return PROMPT_SET_PLAYSTATION;
    case PROMPT_STYLE_XBOX: return PROMPT_SET_XBOX;
    case PROMPT_STYLE_NINTENDO: return PROMPT_SET_NINTENDO;
    default: return prompt_set_for_type(type);
    }
}
/* The pad type whose SDL labels a set shows: the pad's own when it already is of that set (a
 * GameCube pad keeps its letters), else the set's typical pad. */
static inline SDL_GamepadType prompt_label_type(int set, SDL_GamepadType type) {
    if (prompt_set_for_type(type)==set && type!=SDL_GAMEPAD_TYPE_UNKNOWN && type!=SDL_GAMEPAD_TYPE_STANDARD)
        return type;
    return set==PROMPT_SET_NINTENDO ? SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO
         : set==PROMPT_SET_PLAYSTATION ? SDL_GAMEPAD_TYPE_PS4 : SDL_GAMEPAD_TYPE_XBOXONE;
}

/* A key's name on a key cap: short, arrows as arrows (UTF-8). */
static inline void prompt_key_label(const char *sdl_name, char *out, int size) {
    static const struct { const char *sdl, *label; } names[]={
        {"Left Shift","Shift"}, {"Right Shift","RShift"}, {"Left Ctrl","Ctrl"}, {"Right Ctrl","RCtrl"},
        {"Left Alt","Alt"}, {"Right Alt","AltGr"}, {"Return","Enter"}, {"Keypad Enter","Enter"},
        {"Backspace","Bksp"}, {"Escape","Esc"}, {"CapsLock","Caps"}, {"Delete","Del"},
        {"Insert","Ins"}, {"PageUp","PgUp"}, {"PageDown","PgDn"}, {"Space","Space"},
        {"Up","\xe2\x86\x91"}, {"Down","\xe2\x86\x93"}, {"Left","\xe2\x86\x90"}, {"Right","\xe2\x86\x92"},
    };
    if (size<1) return;
    const char *name=sdl_name && *sdl_name ? sdl_name : "?";
    for (size_t i=0;i<sizeof(names)/sizeof(*names);++i)
        if (!strcmp(name,names[i].sdl)) { snprintf(out,(size_t)size,"%s",names[i].label); return; }
    if (!strncmp(name,"Keypad ",7)) { snprintf(out,(size_t)size,"Num%s",name+7); return; }
    snprintf(out,(size_t)size,"%s",name);
}

/* Prompt kinds (bbport_button_prompts.cpp draws each): 0 none (the game's icon stays), 'K' a key,
 * 'M' a mouse button (Left Right Middle X1 X2, Wheel Up, Wheel Down; a chord prefix may come
 * first), 'F' a face button (A B X Y), 'S' a shoulder or trigger (LB RB LT RT; L R ZL ZR), 'C'
 * another round button or a stick (LS RS Menu View + -). */
#define PROMPT_PAD_LEFT_TRIGGER SDL_GAMEPAD_BUTTON_COUNT
#define PROMPT_PAD_RIGHT_TRIGGER (SDL_GAMEPAD_BUTTON_COUNT+1)
/* The label of gamepad button `button` (SDL's, or the two triggers above) in a pad set. */
static inline int prompt_pad_label(int set, SDL_GamepadType type, int button, char *out, int size) {
    if (size<4) return 0;
    out[0]=0;
    if (set!=PROMPT_SET_XBOX && set!=PROMPT_SET_NINTENDO) return 0;
    const int nintendo=set==PROMPT_SET_NINTENDO;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH: {
        const SDL_GamepadButtonLabel l=SDL_GetGamepadButtonLabelForType(prompt_label_type(set,type),
                                                                       (SDL_GamepadButton)button);
        const char *text=l==SDL_GAMEPAD_BUTTON_LABEL_A ? "A" : l==SDL_GAMEPAD_BUTTON_LABEL_B ? "B"
                        : l==SDL_GAMEPAD_BUTTON_LABEL_X ? "X" : l==SDL_GAMEPAD_BUTTON_LABEL_Y ? "Y" : NULL;
        if (!text) return 0; /* no letter: the game's icon */
        snprintf(out,(size_t)size,"%s",text);
        return 'F';
    }
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: snprintf(out,(size_t)size,"%s",nintendo ? "L" : "LB"); return 'S';
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: snprintf(out,(size_t)size,"%s",nintendo ? "R" : "RB"); return 'S';
    case PROMPT_PAD_LEFT_TRIGGER: snprintf(out,(size_t)size,"%s",nintendo ? "ZL" : "LT"); return 'S';
    case PROMPT_PAD_RIGHT_TRIGGER: snprintf(out,(size_t)size,"%s",nintendo ? "ZR" : "RT"); return 'S';
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: snprintf(out,(size_t)size,"LS"); return 'C';
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: snprintf(out,(size_t)size,"RS"); return 'C';
    case SDL_GAMEPAD_BUTTON_START: snprintf(out,(size_t)size,"%s",nintendo ? "+" : "Menu"); return 'C';
    case SDL_GAMEPAD_BUTTON_BACK: snprintf(out,(size_t)size,"%s",nintendo ? "-" : "View"); return 'C';
    case SDL_GAMEPAD_BUTTON_MISC1: snprintf(out,(size_t)size,"%s",nintendo ? "Cap" : "Share"); return 'C';
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: snprintf(out,(size_t)size,"Pad"); return 'C';
    default: return 0; /* the d-pad and paddles: the game's icons fit any pad */
    }
}

#ifdef __cplusplus
}
#endif
