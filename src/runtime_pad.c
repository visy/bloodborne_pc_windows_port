/* libScePad on SDL3 gamepads and the keyboard. SDL events are pumped by the window thread
 * (gpu/shim/window.cpp); here state is only sampled.
 *
 * Keyboard layout (also with a gamepad connected: both drive the game; bbport.ini key.* lines
 * rebind it, see bind_defaults):
 *   WASD left stick, IJKL right stick (I up, K down, J left, L right),
 *   Space/Enter Cross, LShift/Esc Circle, E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2,
 *   Z L3, C R3, F1/O Options, Tab left touchpad, Backspace right touchpad, arrow keys d-pad.
 *
 * Mouse & keyboard mode (opt-in: launcher "Mouse & keyboard controls", BB_MOUSE_KEYBOARD=1,
 * bbport.ini mk_enabled=1 or the in-game menu's Controls tab; Souls-PC scheme from
 * Mrsuss60/bloodborne_pc_windows_port) replaces the layout above while it is on:
 *   mouse: motion right stick (camera; mk_sens_x/y, mk_invert_x/y, mk_deadzone, mk_smoothing),
 *   left button R1, Shift+left R2, right button L2, Shift+right L1, middle R3 (lock-on),
 *   side buttons X1 L1, X2 R2, wheel up / down the d-pad's up / down (quick items, mk_wheel;
 *   one short press per notch; upstream 0.51 #52);
 *   keys: WASD left stick, IJKL right stick, Space Circle (dodge), E/Enter Cross, R Square,
 *   X Triangle, Q R3 (lock-on), C/Z L3, Esc/F1 Options, Tab/G left touchpad, Backspace right
 *   touchpad, arrow keys and 1 2 3 4 d-pad (up down left right).
 * bbport.ini mkkey.<input>= lines rebind that layout as key.<input>= lines do the classic one;
 * pad.<input>= lines apply in both. Mouse buttons and the wheel are keys there too ("Mouse Left",
 * "Mouse Right", "Mouse Middle", "Mouse X1", "Mouse X2", "Wheel Up", "Wheel Down"; upstream 0.51
 * #52): a mouse button named in an mkkey line leaves the built-in mouse scheme above. The mouse is
 * read only in mouse & keyboard mode. The mouse is captured (relative mode) only while the menu and
 * the name box are closed.
 *
 * Stick neutral: SDL exposes no way to read a pad's calibration and some clones report a
 * biased neutral (a Switch-style pad was seen returning both sticks at a constant ~ +/-16380).
 * The neutral of each axis is taken from a quiet window after the pad opens (SDL returns zero
 * until the first report arrives, so those samples are skipped) and subtracted when all four axes
 * are biased; otherwise (a genuine pad, perhaps opened with a stick held) the neutral is 0.
 *
 * Travel: such a clone also uses only part of SDL's -32768..32767 span (its neutral sits in the
 * middle of one half), so reading the axis as -128..127 would reach only half deflection and a
 * full push would never run. Each axis is instead scaled by its own travel: a biased axis starts
 * from its neutral's magnitude and is refined by the largest push seen per direction, so both
 * directions reach full deflection even when the two travels differ by a few percent (that
 * difference is what makes one direction run and the other only walk). A genuine pad keeps the
 * plain -32768..32767 -> -128..127 read. The stick is then converted as shadPS4 does, with an
 * inner/outer dead zone (BB_PAD_DEADZONE, default 5; BB_PAD_DEADZONE_OUTER, default 127) mapping
 * the axis' travel up to full deflection. BB_PAD_CENTER=lx,ly,rx,ry overrides the neutral,
 * BB_PAD_CENTER_CAL=0 disables the measurement. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include "pad_bindings.h"
#include "pad_prompts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static HostMutex lock = HOST_MUTEX_INIT;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

/* Per-controller stick neutral (see the file header): the value each axis holds while the
 * sticks are untouched, taken from a quiet window after the pad opens. Works for any pad; a
 * genuine one settles at 0 and the subtraction is a no-op. */
#define PAD_CAL_QUIET 512              /* an axis is quiet when it moved no more than this */
#define PAD_CAL_QUIET_SAMPLES 8
#define PAD_CAL_TIMEOUT_US 3000000u    /* after this, an all-zero reading is accepted as the neutral */
#define PAD_CAL_BIAS 8192              /* |neutral| past this: a biased neutral and a reduced travel */
#define PAD_CAL_REFINE 90              /* % of the base travel a push must reach to count as the full scale */
static int cal_enabled=-1, pad_deadzone=-1, pad_deadzone_outer=-1, cal_manual;
static int cal_have_center, cal_started, cal_quiet_count;
static int cal_center[4], cal_prev[4], cal_manual_center[4];
static int cal_base[4];                /* 0: normal pad; else |neutral| of a biased axis */
static int cal_max[4][2];              /* largest push seen per direction on a biased axis */
static uint64_t cal_since;

static uint64_t now_us(void) { return host_monotonic_ns() / 1000u; }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static void cal_load(void) {
    static int loaded;
    if (loaded) return;
    loaded=1;
    const char *v=getenv("BB_PAD_CENTER_CAL"); cal_enabled=!(v && *v=='0');
    /* Inner/outer dead zone, as shadPS4's analog_deadzone: [inner, outer] maps linearly to the
     * full deflection, so an axis with a reduced travel (outer below 127) reaches full. */
    v=getenv("BB_PAD_DEADZONE"); pad_deadzone=v && *v ? atoi(v) : 5;
    if (pad_deadzone<0) pad_deadzone=0; else if (pad_deadzone>126) pad_deadzone=126;
    v=getenv("BB_PAD_DEADZONE_OUTER"); pad_deadzone_outer=v && *v ? atoi(v) : 127;
    if (pad_deadzone_outer<=pad_deadzone) pad_deadzone_outer=pad_deadzone<127 ? pad_deadzone+1 : 127;
    v=getenv("BB_PAD_CENTER");
    if (v && *v && sscanf(v,"%d,%d,%d,%d",&cal_manual_center[0],&cal_manual_center[1],
                          &cal_manual_center[2],&cal_manual_center[3])==4) {
        cal_manual=1;
        printf("Runtime: pad center (BB_PAD_CENTER): lx=%d ly=%d rx=%d ry=%d\n",
               cal_manual_center[0],cal_manual_center[1],cal_manual_center[2],cal_manual_center[3]);
    }
}
static void cal_set_base(void) {
    int biased=0;
    for (int i=0;i<4;++i) {
        const int a=abs(cal_center[i]);
        cal_base[i]=a>=PAD_CAL_BIAS ? a : 0;   /* a biased axis spans one half of the int16 range */
        cal_max[i][0]=cal_max[i][1]=0;
        biased|=cal_base[i]!=0;
    }
    if (biased)
        printf("Runtime: pad travel (biased neutral, per-axis scaling): lx=%d ly=%d rx=%d ry=%d\n",
               cal_base[0]?cal_base[0]:32768,cal_base[1]?cal_base[1]:32768,
               cal_base[2]?cal_base[2]:32768,cal_base[3]?cal_base[3]:32768);
}
/* Full-scale travel of one direction. A biased axis starts from its neutral's magnitude and is
 * refined by the largest push seen (once that push is close to the base travel), so a direction
 * whose physical travel is a few percent shorter still reaches full deflection. */
static int cal_travel(int axis,int dir) {
    const int base=cal_base[axis] ? cal_base[axis] : 32768;
    return cal_base[axis] && cal_max[axis][dir]*100>=base*PAD_CAL_REFINE ? cal_max[axis][dir] : base;
}
static void cal_reset(void) {
    cal_load();
    cal_started=0; cal_quiet_count=0; cal_since=0; memset(cal_prev,0,sizeof cal_prev);
    if (cal_manual) { memcpy(cal_center,cal_manual_center,sizeof cal_center); cal_have_center=1; }
    else if (!cal_enabled) { memset(cal_center,0,sizeof cal_center); cal_have_center=1; }
    else { cal_have_center=0; memset(cal_center,0,sizeof cal_center); memset(cal_base,0,sizeof cal_base); }
    if (cal_have_center) cal_set_base();
}
/* The value each axis holds while the sticks are untouched (SDL's pre-report zeros are skipped);
 * once it is known, the largest push seen per direction on a biased axis (see cal_travel). */
static void cal_sample(const int16_t raw[4]) {
    if (cal_have_center) {
        for (int i=0;i<4;++i) {
            if (!cal_base[i]) continue;
            const int d=raw[i]-cal_center[i], dir=d>=0, travel=dir ? d : -d;
            if (travel>cal_max[i][dir]) cal_max[i][dir]=travel;
        }
        return;
    }
    int allzero=1; for (int i=0;i<4;++i) if (raw[i]) allzero=0;
    int quiet=1; for (int i=0;i<4;++i) if (cal_started && abs(raw[i]-cal_prev[i])>PAD_CAL_QUIET) quiet=0;
    const uint64_t now=now_us();
    if (!cal_started) { cal_started=1; cal_since=now; }
    if (allzero && now-cal_since<PAD_CAL_TIMEOUT_US) quiet=0;
    if (quiet) {
        if (++cal_quiet_count>=PAD_CAL_QUIET_SAMPLES) {
            /* Only the clone pattern (all four axes biased) is taken as the neutral: a genuine pad
             * opened with a stick held steady would otherwise keep that push as its neutral. */
            int biased=0;
            for (int i=0;i<4;++i) biased+=abs(raw[i])>=PAD_CAL_BIAS;
            for (int i=0;i<4;++i) cal_center[i]=biased==4 ? raw[i] : 0;
            cal_have_center=1;
            if (biased==4)
                printf("Runtime: pad neutral: lx=%d ly=%d rx=%d ry=%d\n",cal_center[0],cal_center[1],cal_center[2],cal_center[3]);
            else if (biased)
                printf("Runtime: pad neutral 0 (%d of 4 axes off-center at open: a stick held, not a biased pad)\n",biased);
            cal_set_base();
        }
    } else cal_quiet_count=0;
    for (int i=0;i<4;++i) cal_prev[i]=raw[i];
}
/* One stick axis in the PS4 0..255 scale. The neutral is subtracted and the axis' own travel
 * (cal_travel) is read as -128..127, so a full push reaches full deflection in both directions
 * whatever part of SDL's range the pad uses; the inner/outer dead zone (BB_PAD_DEADZONE /
 * BB_PAD_DEADZONE_OUTER) then maps that travel to the full deflection, as shadPS4 does. */
static uint8_t stick_axis(int axis,int16_t raw) {
    const int d=raw-cal_center[axis];
    const int travel=cal_travel(axis,d>=0 ? 1 : 0);
    int v=travel>0 ? (int)((long long)d*128/travel) : 0;
    if (v>127) v=127; else if (v<-128) v=-128;
    const int mag=abs(v);
    if (mag<=pad_deadzone || pad_deadzone>=pad_deadzone_outer) v=0;
    else {
        int scaled=(int)(128.0*(mag-pad_deadzone)/(float)(pad_deadzone_outer-pad_deadzone));
        if (scaled>128) scaled=128;
        v = v>=0 ? scaled : -scaled;
    }
    int x=v+128;
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static uint8_t synth_touch_id = 1;
static uint64_t touch_down_us = 0;

static void touch_click(PadData *d, int right) {
    uint64_t t = now_us();
    if (!touch_down_us) {
        touch_down_us = t;
        if (++synth_touch_id > 127 || synth_touch_id == 0) synth_touch_id = 1;
    }
    d->buttons |= BTN_TOUCHPAD;
    d->touch_count = 1;
    d->touch_held_time = (uint32_t)(t - touch_down_us);
    d->touches[0] = (PadTouch){.x = right ? 1440 : 480, .y = 471, .id = synth_touch_id};
}

/* BB_GAMEPAD (the launcher's controller choice): its SDL GUID, or part of its name. Issue #15:
 * wheels and other controllers connected for good came first. */
static const char *preferred_gamepad(void) {
    static const char *want; static int read;
    if (!read) { want=getenv("BB_GAMEPAD"); if (want && !*want) want=NULL; read=1; }
    return want;
}
static int is_preferred(SDL_JoystickID id, const char *want) {
    char guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id),guid,sizeof guid);
    const char *name=SDL_GetGamepadNameForID(id);
    return !SDL_strcasecmp(guid,want) || (name && SDL_strcasestr(name,want));
}
/* Button prompts (src/pad_prompts.h; gpu/shim/bbport_button_prompts.cpp draws them; derived from
 * deadinside28/bloodborne_pc 72e5c8ce): the set the game's key guide icons show. The device whose
 * input came last decides keyboard or gamepad, the gamepad's SDL type (or bbport.ini prompt_style)
 * its set. prompt_generation changes whenever the icons must be drawn again: the set, the pad type
 * or the bindings changed. 0: nothing sampled yet (the game's icons). */
static int prompt_set=-1, prompt_keyboard_last=-1;
static SDL_GamepadType prompt_pad_type=SDL_GAMEPAD_TYPE_UNKNOWN;
static unsigned prompt_generation;
static int prompt_style(void) {
    const int style=bbgpu_prompt_style();
    return style>=0 && style<PROMPT_STYLE_COUNT ? style : PROMPT_STYLE_AUTO;
}
/* One line per gamepad connect: what SDL reports and the set it gets. */
static void prompt_log_pad(SDL_Gamepad *g, const char *suffix) {
    const SDL_GamepadType type=SDL_GetGamepadType(g);
    const int style=prompt_style();
    const int set=prompt_set_choose(style,0,type);
    const char *type_name=SDL_GetGamepadStringForType(type);
    printf("Runtime: gamepad connected: %s%s, VID:PID %04x:%04x, SDL type %s, button prompts %s%s\n",
           SDL_GetGamepadName(g),suffix,(unsigned)SDL_GetGamepadVendor(g),(unsigned)SDL_GetGamepadProduct(g),
           type_name ? type_name : "unknown",prompt_set_names[set],
           style==PROMPT_STYLE_AUTO ? " (auto)" : " (prompt_style)");
}
/* Called once per sample (under lock): which device gave input (each 0/1, both 0: none). */
static void prompt_note(SDL_Gamepad *g, int pad_active, int keyboard_active, int bindings_changed) {
    int keyboard_last=prompt_keyboard_last;
    if (keyboard_active) keyboard_last=1;
    else if (pad_active) keyboard_last=0;
    else if (keyboard_last<0) keyboard_last=g ? 0 : 1; /* before any input: a connected pad's set */
    const SDL_GamepadType type=g ? SDL_GetGamepadType(g) : prompt_pad_type;
    const int set=prompt_set_choose(prompt_style(),keyboard_last,type);
    if (set!=prompt_set || type!=prompt_pad_type || bindings_changed || !prompt_generation) {
        if (set!=prompt_set && prompt_set>=0) printf("Runtime: button prompts: %s\n",prompt_set_names[set]);
        prompt_set=set;
        prompt_pad_type=type;
        __atomic_add_fetch(&prompt_generation,1,__ATOMIC_RELAXED);
    }
    prompt_keyboard_last=keyboard_last;
}
static int stick_moved(uint8_t v) { return v<128-48 || v>128+48; }
/* BB_PAD_FILE's input (read_inject below): buttons and sticks of a scripted pad, and keyhold=
 * keys held on a scripted keyboard. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
#define KEY_INJECT_MAX 8
static int key_inject[KEY_INJECT_MAX], key_inject_count;

/* The chosen gamepad, else the first while it is not connected (checked again every second, it
 * is taken as soon as it connects); called under lock. */
/* BB_VIRTUAL_PAD=VID:PID (hex; test aid): an SDL virtual gamepad with that USB id, never pressed,
 * so a run without that pad has its SDL type (button prompts' Auto set) while BB_PAD_FILE gives the
 * input. With SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0xVID/0xPID real pads stay out of the run. */
static void attach_virtual_pad(void) {
    static int done;
    if (done) return;
    done=1;
    const char *v=getenv("BB_VIRTUAL_PAD");
    unsigned vid=0, pid=0;
    if (!v || sscanf(v,"%x:%x",&vid,&pid)!=2) return;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport virtual pad";
    desc.vendor_id=(Uint16)vid;
    desc.product_id=(Uint16)pid;
    const SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    printf("Runtime: virtual gamepad %04x:%04x (BB_VIRTUAL_PAD): %s\n",vid,pid,id ? "attached" : SDL_GetError());
}
static SDL_Gamepad *current_gamepad(void) {
    static int on_preferred; static uint64_t last_scan;
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    attach_virtual_pad();
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    const char *want=preferred_gamepad();
    const uint64_t now=now_us();
    if (!gamepad || (want && !on_preferred && now-last_scan>1000000)) {
        last_scan=now;
        int count=0, pick=-1;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        for (int i=0; want && ids && i<count && pick<0; ++i) if (is_preferred(ids[i],want)) pick=i;
        if (pick<0 && !gamepad && ids && count>0) pick=0;
        if (pick>=0 && (!gamepad || SDL_GetGamepadID(gamepad)!=ids[pick])) {
            if (gamepad) SDL_CloseGamepad(gamepad);
            gamepad=SDL_OpenGamepad(ids[pick]);
            cal_reset(); /* a new pad has its own neutral */
            on_preferred=want && gamepad && is_preferred(ids[pick],want);
            if (gamepad) {
                ++connected_count;
                prompt_log_pad(gamepad,!want ? "" : on_preferred ? " (the chosen one)" : " (the chosen one is not connected)");
            }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* bbport (frame stats): the game's libc heap, read every 5 s from a game thread (it reads the pad)
 * with libc.prx's malloc_stats (export stub at libc.prx+0x1f8c0; malloc_stats_fast returns 1 in this libc, the module at image +0x56e0000):
 * in use now and at most, and what it took from the system. A heap that keeps growing is a leak. */
typedef struct { uint16_t size, version; uint32_t reserved; uint64_t max_system, system, max_in_use, in_use; } MallocManagedSize;
static void report_guest_heap(void) {
    static int enabled=-1; static uint64_t last;
    if (enabled<0) enabled=getenv("BB_FRAME_STATS")!=NULL;
    const uint64_t now=now_us();
    if (!enabled || now-last<5000000) return;
    last=now;
    const uint8_t *stub=(const uint8_t *)(0x800000000ull+0x56e0000+0x1f8c0);
    if (stub[0]!=0xff || stub[1]!=0x25) return; /* another libc */
    ABI int (*stats)(MallocManagedSize *)=(ABI int (*)(MallocManagedSize *))(uintptr_t)stub;
    MallocManagedSize m={.size=sizeof(m),.version=1};
    const int result=stats(&m);
    if (result!=0) { static int told; if (!told++) printf("Guest heap: malloc_stats returned %#x\n",(unsigned)result); return; }
    {
        printf("Guest heap: %.1f MB in use (most %.1f), %.1f MB from the system (most %.1f)\n",
               m.in_use/1048576.0,m.max_in_use/1048576.0,m.system/1048576.0,m.max_system/1048576.0);
    }
}
/* Controls: what each PS4 input is bound to. Defaults below; bbport.ini (BB_CONFIG) lines
 * key.<input>=<SDL key names> and pad.<input>=<SDL gamepad button names>, comma-separated,
 * replace an input's binding (empty: unbound). Inputs: the buttons (cross ... right, touchpad =
 * a left-side click, touchpad_right), and on the keyboard the sticks: move_* (left), look_*
 * (right), and walk (held: the move stick at a walk). Gamepad names as SDL's: a b x y back start leftstick rightstick leftshoulder
 * rightshoulder dpup dpdown dpleft dpright touchpad misc1 paddle1-4, plus lefttrigger and
 * righttrigger. The keyboard works next to a gamepad (the Steam Deck always has one): its buttons
 * add to the gamepad's, a held move/look key moves the stick all the way. */
static const uint32_t input_buttons[IN_COUNT]={
    BTN_CROSS,BTN_CIRCLE,BTN_SQUARE,BTN_TRIANGLE,BTN_L1,BTN_R1,BTN_L2,BTN_R2,BTN_L3,BTN_R3,
    BTN_OPTIONS,BTN_TOUCHPAD,0,BTN_UP,BTN_DOWN,BTN_LEFT,BTN_RIGHT,
};
#define MAX_BIND 4
enum { PAD_LEFT_TRIGGER=SDL_GAMEPAD_BUTTON_COUNT, PAD_RIGHT_TRIGGER }; /* triggers as buttons */
typedef struct { int key_count, pad_count; int keys[MAX_BIND]; int pad[MAX_BIND]; } Binding;
static Binding bindings[IN_COUNT];
static int bindings_mode=-1; /* the layout loaded: 0 classic, 1 mouse & keyboard, -1 none yet */
/* BbMouseState's button bits (0 left, 1 right, 2 middle, 3 X1, 4 X2) of an SDL button. */
static uint32_t mouse_bit(int sdl_button) {
    switch (sdl_button) {
    case SDL_BUTTON_LEFT: return 1u;
    case SDL_BUTTON_RIGHT: return 2u;
    case SDL_BUTTON_MIDDLE: return 4u;
    case SDL_BUTTON_X1: return 8u;
    case SDL_BUTTON_X2: return 16u;
    default: return 0;
    }
}
/* The mouse buttons some binding names: they leave the built-in mouse scheme (sample_mouse). */
static uint32_t mouse_bound_mask;

static void bind_defaults(int mk) {
    static const struct { int input, button; } pads[]={
        {IN_CROSS,SDL_GAMEPAD_BUTTON_SOUTH}, {IN_CIRCLE,SDL_GAMEPAD_BUTTON_EAST},
        {IN_SQUARE,SDL_GAMEPAD_BUTTON_WEST}, {IN_TRIANGLE,SDL_GAMEPAD_BUTTON_NORTH},
        {IN_L1,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {IN_R1,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {IN_L2,PAD_LEFT_TRIGGER}, {IN_R2,PAD_RIGHT_TRIGGER},
        {IN_L3,SDL_GAMEPAD_BUTTON_LEFT_STICK}, {IN_R3,SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {IN_OPTIONS,SDL_GAMEPAD_BUTTON_START},
        {IN_TOUCHPAD,SDL_GAMEPAD_BUTTON_BACK}, {IN_TOUCHPAD,SDL_GAMEPAD_BUTTON_TOUCHPAD},
        {IN_UP,SDL_GAMEPAD_BUTTON_DPAD_UP}, {IN_DOWN,SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {IN_LEFT,SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {IN_RIGHT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    memset(bindings,0,sizeof bindings);
    const KeyDefault *key_list=mk ? mk_keys : classic_keys;
    const size_t key_count=mk ? MK_KEY_COUNT : CLASSIC_KEY_COUNT;
    for (size_t i=0;i<key_count;++i) {
        Binding *b=&bindings[key_list[i].input]; b->keys[b->key_count++]=key_list[i].key;
    }
    for (size_t i=0;i<sizeof(pads)/sizeof(*pads);++i) {
        Binding *b=&bindings[pads[i].input]; b->pad[b->pad_count++]=pads[i].button;
    }
}
static int pad_button_from_name(const char *name) {
    if (!SDL_strcasecmp(name,"lefttrigger")) return PAD_LEFT_TRIGGER;
    if (!SDL_strcasecmp(name,"righttrigger")) return PAD_RIGHT_TRIGGER;
    const SDL_GamepadButton b=SDL_GetGamepadButtonFromString(name);
    return b==SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)b;
}
/* key.<input>= (classic layout) / mkkey.<input>= (mouse & keyboard mode) / pad.<input>= lines of
 * the settings file. */
static void load_bindings(int mk) {
    bind_defaults(mk);
    const char *path=getenv("BB_CONFIG");
    FILE *f=path ? fopen(path,"r") : NULL;
    if (!f) return;
    char line[512];
    while (fgets(line,sizeof line,f)) {
        const int mk_line=!strncmp(line,"mkkey.",6);
        const int keyboard=!strncmp(line,"key.",4) || mk_line, pad=!strncmp(line,"pad.",4);
        char *eq=strchr(line,'=');
        if ((!keyboard && !pad) || !eq) continue;
        if (keyboard && mk_line!=mk) continue; /* the other layout's line */
        *eq=0;
        const char *input_name=line+(mk_line ? 6 : 4);
        int input=-1;
        for (int i=0;i<IN_COUNT;++i) if (!strcmp(input_name,input_names[i])) input=i;
        if (input<0 || (pad && input>=IN_MOVE_UP)) { printf("Runtime: controls: unknown input %s\n",line); continue; }
        Binding *b=&bindings[input];
        if (keyboard) b->key_count=0; else b->pad_count=0;
        for (char *name=strtok(eq+1,",\r\n"); name; name=strtok(NULL,",\r\n")) {
            while (*name==' ') ++name;
            for (char *end=name+strlen(name); end>name && end[-1]==' ';) *--end=0;
            if (!*name) continue;
            if (keyboard) {
                const int s=key_from_name(name);
                if (s<0) printf("Runtime: controls: unknown key \"%s\" for %s\n",name,input_name);
                else if (b->key_count<MAX_BIND) b->keys[b->key_count++]=s;
            } else {
                const int button=pad_button_from_name(name);
                if (button<0) printf("Runtime: controls: unknown gamepad button \"%s\" for %s\n",name,input_name);
                else if (b->pad_count<MAX_BIND) b->pad[b->pad_count++]=button;
            }
        }
    }
    fclose(f);
}
static void note_mouse_bindings(void) {
    mouse_bound_mask=0;
    for (int i=0;i<IN_COUNT;++i)
        for (int j=0;j<bindings[i].key_count;++j) {
            const int code=bindings[i].keys[j];
            if (code>MOUSE_KEY && code<=MOUSE_KEY+SDL_BUTTON_X2) mouse_bound_mask|=mouse_bit(code-MOUSE_KEY);
        }
}
/* The mouse in mouse & keyboard mode: buttons held, and the wheel as short presses (one per notch,
 * each held and then released for 60 ms so the game sees it; upstream 0.51 mouse_wheel_step). */
static uint32_t mouse_held;
static int wheel_pending[2], wheel_phase[2]; /* [up, down]; phase 0 idle, 1 pressed, 2 released */
static uint64_t wheel_until[2];
static void mouse_wheel_step(uint64_t now, int up, int down) {
    wheel_pending[0]+=up; wheel_pending[1]+=down;
    for (int i=0;i<2;++i) {
        if (wheel_pending[i]>8) wheel_pending[i]=8;
        if (wheel_phase[i] && now<wheel_until[i]) continue;
        if (wheel_phase[i]==1) { wheel_phase[i]=2; wheel_until[i]=now+60000; }
        else if (wheel_pending[i]>0) { --wheel_pending[i]; wheel_phase[i]=1; wheel_until[i]=now+60000; }
        else wheel_phase[i]=0;
    }
}
static void mouse_wheel_reset(void) { wheel_pending[0]=wheel_pending[1]=wheel_phase[0]=wheel_phase[1]=0; }
static int mouse_key_down(int code) {
    if (code==MOUSE_WHEEL_UP) return wheel_phase[0]==1;
    if (code==MOUSE_WHEEL_DOWN) return wheel_phase[1]==1;
    return (mouse_held & mouse_bit(code-MOUSE_KEY))!=0;
}
static int key_down(const bool *k, int input) {
    for (int i=0;i<bindings[input].key_count;++i) {
        const int code=bindings[input].keys[i];
        if (code>=MOUSE_KEY ? mouse_key_down(code) : k && k[code]) return 1;
    }
    return 0;
}
/* Xbox pads: Back + d-pad left/right clicks the touchpad halves (after Supermedo/bloodborne_pc
 * a04e1e95; the idea and timing are parsifal295's, its PR #39). Pads without a touch surface
 * had only Back, a click on the left half (gestures): the right half (key items, the right choice
 * of some prompts) could not be reached. While Back is bound to nothing but the touchpad (the
 * default pad.touchpad), Back + d-pad left or right clicks that half as long as the d-pad is
 * held, and Back alone clicks the left half for 150 ms when it is released, so Back + right does
 * not open the gestures first. Back and, while Back is held, d-pad left/right then reach no other
 * binding. Pads with a touch surface, and a Back moved by pad.<input>= lines, keep the old rule. */
static int back_combo_masked; /* pad_value: 1 Back reads released, 2 also d-pad left/right */
static int back_combo_wanted(SDL_Gamepad *g) {
    if (!g || SDL_GetNumGamepadTouchpads(g)>0) return 0;
    int on_touchpad=0;
    for (int input=IN_CROSS;input<=IN_RIGHT;++input)
        for (int i=0;i<bindings[input].pad_count;++i)
            if (bindings[input].pad[i]==SDL_GAMEPAD_BUTTON_BACK) {
                if (input!=IN_TOUCHPAD) return 0;
                on_touchpad=1;
            }
    return on_touchpad;
}
/* One sample of the combo: the touchpad half to click (0 left, 1 right) or -1. */
static int back_combo_step(int back, int left, int right, int pinged, uint64_t now) {
    static int was_down, combo;
    static uint64_t click_until;
    int side=-1;
    if (back) {
        if (!was_down) { combo=0; click_until=0; }
        if (left || right) { side=right ? 1 : 0; combo=1; }
        if (pinged) combo=1; /* Back + R3 was a ping (I2): no gestures click on release */
    } else if (was_down && !combo) {
        click_until=now+150000;
    }
    was_down=back;
    if (side<0 && now<click_until) side=0;
    return side;
}
/* Back held + a stick click (R3: a ping, L3: gather the party) on pads without a touch surface.
 * 1 on the sample the combo fires (the click pressed while Back is down and `enabled` says yes);
 * that click is then kept from the game until it is released. */
typedef struct { int prev, strip; } BackClickCombo;
static int back_click_combo(BackClickCombo *c, int back_down, uint32_t *buttons, uint32_t bit,
                            int (*enabled)(void)) {
    const int down=(*buttons & bit)!=0;
    const int fired=back_down && down && !c->prev && enabled();
    if (fired) c->strip=1;
    c->prev=down;
    if (c->strip) { if (down) *buttons&=~bit; else c->strip=0; }
    return fired;
}
/* The bound gamepad buttons' state; triggers as their analog value. */
static int pad_value(SDL_Gamepad *g, int input) {
    int value=0;
    for (int i=0;i<bindings[input].pad_count;++i) {
        const int b=bindings[input].pad[i];
        if (back_combo_masked && (b==SDL_GAMEPAD_BUTTON_BACK || (back_combo_masked==2 &&
            (b==SDL_GAMEPAD_BUTTON_DPAD_LEFT || b==SDL_GAMEPAD_BUTTON_DPAD_RIGHT)))) continue;
        const int v=b==PAD_LEFT_TRIGGER ? trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER))
                  : b==PAD_RIGHT_TRIGGER ? trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                  : SDL_GetGamepadButton(g,(SDL_GamepadButton)b) ? 255 : 0;
        if (v>value) value=v;
    }
    return value;
}

/* key held for the negative / positive direction: the stick all the way, else the gamepad's. */
static uint8_t key_axis(uint8_t value, int negative, int positive) {
    return negative || positive ? (uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0)) : value;
}
/* Keyboard sticks (after Supermedo/bloodborne_pc a147eb8 and 576514c): two opposite keys held
 * together give the one pressed last ("snap tap") instead of cancelling out, so rolling from A to
 * D never passes through a neutral stick, which ends a sprint. -1, 0 or 1; `since` orders the
 * presses by sample. */
typedef struct { uint64_t negative_since, positive_since; } KeyAxisState;
static KeyAxisState key_axes[4]; /* move x, move y, look x, look y */
static uint64_t key_sample;
static int key_direction(KeyAxisState *s, int negative, int positive) {
    ++key_sample;
    s->negative_since=negative ? (s->negative_since ? s->negative_since : key_sample) : 0;
    s->positive_since=positive ? (s->positive_since ? s->positive_since : key_sample) : 0;
    if (negative && positive && s->negative_since!=s->positive_since)
        return s->positive_since>s->negative_since ? 1 : -1;
    return positive-negative;
}
/* The keyboard's move stick through direction changes (Supermedo/bloodborne_pc 4b930b10, measured
 * by cassioKenji, PR #74 there): the game picks walk or run by the stick's larger axis (a square
 * gate), and ends a sprint when the stick's direction moves 45 degrees or more between two reads,
 * while the same turn in steps keeps it. So the keyboard stick stays on the square (the larger
 * axis is 127 at every angle: W+D is the corner, a run, where the round gate's 90,90 was not), and
 * it turns at KEY_STICK_TURN_RATE deg/s but never more than KEY_STICK_MAX_STEP per read, like a
 * thumb sliding along the gate. A half turn (A <-> D, W <-> S) has no shortest way round: it goes
 * through forward, or through the right when both ways are sideways. A released direction is held
 * KEY_STICK_RELEASE_US, so letting go of one key just before pressing the next does not flash a
 * neutral stick. The angle starts fresh whenever the stick was neutral. The gamepad's sticks are
 * never touched. Returns 1 with *ox, *oy (-127..127) while the keyboard drives the stick. */
#define KEY_STICK_TURN_RATE 900.0
#define KEY_STICK_MAX_STEP (30.0*3.14159265358979323846/180.0)
#define KEY_STICK_HALF_TURN (170.0*3.14159265358979323846/180.0)
#define KEY_STICK_RELEASE_US 60000u
typedef struct { double angle; uint64_t at, released_at; int active, held_x, held_y; } KeyStickState;
static KeyStickState key_move_stick;
static int key_stick_step(KeyStickState *s, int dx, int dy, uint64_t now, int *ox, int *oy) {
    const double pi=3.14159265358979323846;
    if (!dx && !dy) {
        if (s->active) { s->active=0; s->released_at=now; }
        if ((s->held_x || s->held_y) && now-s->released_at<KEY_STICK_RELEASE_US) {
            *ox=s->held_x; *oy=s->held_y;
            return 1;
        }
        s->held_x=s->held_y=0;
        return 0;
    }
    const double target=atan2((double)dy,(double)dx);
    if (s->active) {
        const double diff=fmod(target-s->angle+3.0*pi,2.0*pi)-pi; /* -pi..pi, shortest way round */
        const double step=fmin(KEY_STICK_TURN_RATE*pi/180.0*(double)(now-s->at)/1e6,KEY_STICK_MAX_STEP);
        double dir=diff>0 ? 1.0 : -1.0;
        if (fabs(diff)>KEY_STICK_HALF_TURN) {
            /* Half way round counter-clockwise is angle+pi/2; forward is y < 0. */
            const double hy=sin(s->angle+pi/2.0), hx=cos(s->angle+pi/2.0);
            dir=hy<-1e-9 ? 1.0 : hy>1e-9 ? -1.0 : hx>0 ? 1.0 : -1.0;
        }
        if (fabs(diff)<=step) s->angle=target;
        else s->angle=atan2(sin(s->angle+dir*step),cos(s->angle+dir*step));
    } else {
        s->angle=target;
    }
    s->active=1; s->at=now;
    const double c=cos(s->angle), sn=sin(s->angle), k=127.0/fmax(fabs(c),fabs(sn));
    const int x=(int)lround(c*k), y=(int)lround(sn*k);
    *ox=s->held_x=x<-127 ? -127 : x>127 ? 127 : x;
    *oy=s->held_y=y<-127 ? -127 : y>127 ? 127 : y;
    return 1;
}
/* The walk key (IN_WALK, key.walk=, Left Alt by default; after Mrsuss60/bloodborne_pc_windows_port
 * 5e029181 by HatsyRei, who used 48): while it is held the keyboard's move stick keeps its angle
 * but its larger axis is KEY_STICK_WALK instead of 127, so the hunter walks. Measured in game
 * (2026-10-17, Hunter's Dream, one session, the left stick forward/back for 10 s each): 40 does not
 * move (the dead zone), 48 to 115 all walk at about 1.5 m/s, 127 runs at 3.2-3.6 m/s. 64 is well
 * clear of both ends. No PS4 button. */
#define KEY_STICK_WALK 64
static int key_stick_walk(int v) { return (int)lround((double)v*KEY_STICK_WALK/127.0); }
static void apply_keyboard(PadData *d, const bool *k) {
    for (int i=IN_CROSS;i<=IN_RIGHT;++i)
        if (i!=IN_TOUCHPAD && i!=IN_TOUCHPAD_RIGHT && key_down(k,i)) d->buttons|=input_buttons[i];
    if (key_down(k,IN_TOUCHPAD)) touch_click(d,0);
    if (key_down(k,IN_TOUCHPAD_RIGHT)) touch_click(d,1);
    if (key_down(k,IN_L2)) d->l2=255;
    if (key_down(k,IN_R2)) d->r2=255;
    const int lx=key_direction(&key_axes[0],key_down(k,IN_MOVE_LEFT),key_down(k,IN_MOVE_RIGHT));
    const int ly=key_direction(&key_axes[1],key_down(k,IN_MOVE_UP),key_down(k,IN_MOVE_DOWN));
    int sx, sy;
    if (key_stick_step(&key_move_stick,lx,ly,now_us(),&sx,&sy)) {
        if (key_down(k,IN_WALK)) { sx=key_stick_walk(sx); sy=key_stick_walk(sy); }
        /* All the way is 0 / 255, as key_axis gives the other sticks. */
        d->left_x=(uint8_t)(sx<=-127 ? 0 : sx>=127 ? 255 : 128+sx);
        d->left_y=(uint8_t)(sy<=-127 ? 0 : sy>=127 ? 255 : 128+sy);
    }
    const int rx=key_direction(&key_axes[2],key_down(k,IN_LOOK_LEFT),key_down(k,IN_LOOK_RIGHT));
    const int ry=key_direction(&key_axes[3],key_down(k,IN_LOOK_UP),key_down(k,IN_LOOK_DOWN));
    d->right_x=key_axis(d->right_x,rx<0,rx>0);
    d->right_y=key_axis(d->right_y,ry<0,ry>0);
}

/* Mouse & keyboard mode (from Mrsuss60/bloodborne_pc_windows_port, reworked): the mouse's speed
 * moves the right stick, so the camera turns as fast as the mouse moves whatever the frame rate
 * (their per-read delta was frame-rate dependent). Motion is taken at most every 4 ms: a second
 * read in the same frame keeps the previous value instead of seeing no motion. 960 px/s (16 px
 * per 60 Hz frame, their 8 stick units per pixel) is full deflection at sensitivity 1. */
static float look_x, look_y;
static uint64_t look_last;
/* The game's camera speed for a right stick deflection (stick units past neutral, deg/s of yaw;
 * measured in game with BB_CAM_PROBE=1, 2026-10-15): nothing up to 46 (a round dead zone: 40,40
 * turns), then a straight line of about 1.13 deg/s per unit up to 120 (83 deg/s); from about 122
 * the yaw jumps to the game's fast turn (181 deg/s). Pitch follows the same line (85 deg/s at
 * 127, no fast turn). With the look value sent to the stick as it was, the camera did not turn
 * below ~360 px/s of mouse speed at sensitivity 1 and turned at full speed from ~1000 px/s, so
 * the sensitivity sliders only moved that narrow band (Mrsuss60/bloodborne_pc_windows_port
 * 90008b39 by Mrsuss60 found the sliders ineffective too and adds a fixed jump past an assumed
 * 16/127 dead zone). Here the look value (the share of the line's top speed the mouse asks for)
 * goes through the inverse of the measured line, so the camera turns in proportion to the mouse:
 * measured 83-87 mdeg per count at sensitivity 1 from 100 to 800 px/s, half at 0.5, double at 2. A mouse at full speed (the share reaching 1) gets full deflection, the fast turn.
 * BB_MOUSE_LOOK_CURVE=0: the old straight mapping. */
static const struct { float v, rate; } look_curve[]={
    {46.7f,0.0f}, {48.0f,1.50f}, {50.0f,3.75f}, {52.0f,6.01f}, {56.0f,10.49f}, {60.0f,14.99f},
    {64.0f,19.54f}, {72.0f,28.55f}, {80.0f,37.76f}, {88.0f,46.71f}, {96.0f,56.0f},
    {104.0f,64.63f}, {112.0f,73.56f}, {116.0f,78.56f}, {120.0f,82.78f},
};
#define LOOK_CURVE_COUNT (sizeof(look_curve)/sizeof(*look_curve))
static float look_deflection(float share) {
    if (share<=0.0f) return 0.0f;
    if (share>=1.0f) return 127.0f; /* the mouse's top speed: the game's fast turn */
    const float target=share*look_curve[LOOK_CURVE_COUNT-1].rate;
    for (size_t i=1;i<LOOK_CURVE_COUNT;++i)
        if (target<=look_curve[i].rate) {
            const float r0=look_curve[i-1].rate, r1=look_curve[i].rate;
            return look_curve[i-1].v+(look_curve[i].v-look_curve[i-1].v)*(target-r0)/(r1-r0);
        }
    return look_curve[LOOK_CURVE_COUNT-1].v;
}
/* The look value (stick units, -127..127 per axis) to the stick's deflection: below the mouse dead
 * zone nothing, else the curve above on the length, the direction kept. */
static void look_stick(float lx, float ly, float dead, float *ox, float *oy) {
    static int curve=-1;
    if (curve<0) { const char *v=getenv("BB_MOUSE_LOOK_CURVE"); curve=!(v && v[0]=='0'); }
    *ox=*oy=0.0f;
    if (!curve) {
        *ox=lx>dead || lx<-dead ? lx : 0.0f;
        *oy=ly>dead || ly<-dead ? ly : 0.0f;
        return;
    }
    const float len=sqrtf(lx*lx+ly*ly);
    if (len<=dead || len<=0.0f) return;
    const float out=look_deflection(len/127.0f);
    *ox=lx/len*out;
    *oy=ly/len*out;
}
static void sample_mouse(PadData *d, const BbMouseState *m, int consumed, float dt, const bool *k) {
    if (consumed) {
        if (dt>0.0f) {
            if (dt>0.1f) dt=0.1f;
            float tx=m->dx/dt*m->sens_x*(127.0f/960.0f), ty=m->dy/dt*m->sens_y*(127.0f/960.0f);
            if (m->invert_x) tx=-tx;
            if (m->invert_y) ty=-ty;
            float s=m->smoothing; if (s<0.0f) s=0.0f; else if (s>0.95f) s=0.95f;
            look_x=look_x*s+tx*(1.0f-s);
            look_y=look_y*s+ty*(1.0f-s);
            if (look_x>127.0f) look_x=127.0f; else if (look_x<-127.0f) look_x=-127.0f;
            if (look_y>127.0f) look_y=127.0f; else if (look_y<-127.0f) look_y=-127.0f;
        }
    }
    float ox=0.0f, oy=0.0f;
    look_stick(look_x,look_y,m->deadzone*127.0f,&ox,&oy);
    /* Only while the mouse moves: the gamepad's right stick works otherwise (IJKL win over both). */
    if (ox!=0.0f || oy!=0.0f) {
        d->right_x=(uint8_t)(128+(int)(ox>=0.0f ? ox+0.5f : ox-0.5f));
        d->right_y=(uint8_t)(128+(int)(oy>=0.0f ? oy+0.5f : oy-0.5f));
    }
    const int shift=k && (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]);
    const uint32_t b=m->buttons & ~mouse_bound_mask; /* the others act through their bindings */
    if (b & 1u) d->buttons|=shift ? BTN_R2 : BTN_R1;
    if (b & 2u) d->buttons|=shift ? BTN_L1 : BTN_L2;
    if (b & 4u) d->buttons|=BTN_R3;
    if (b & 8u) d->buttons|=BTN_L1;
    if (b & 16u) d->buttons|=BTN_R2;
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    /* BB_MOUSE_TRACE=1 (live checks): the look stick and buttons at most once a second while used. */
    static int trace=-1; static uint64_t traced;
    if (trace<0) { const char *v=getenv("BB_MOUSE_TRACE"); trace=v && v[0]=='1'; }
    if (trace && (ox!=0.0f || oy!=0.0f || m->buttons || m->wheel_up || m->wheel_down) &&
        (now_us()-traced>=1000000 || m->wheel_up || m->wheel_down)) {
        traced=now_us();
        printf("Runtime: mouse look: right stick %u,%u buttons 0x%x (pad 0x%x) wheel %d/%d\n",d->right_x,d->right_y,
               m->buttons,d->buttons,m->wheel_up,m->wheel_down);
    }
}

/* BB_PAD_FILE mouse tokens (test aid, mouse & keyboard mode only; no OS input, no focus): mouse=DX,DY
 * moves the mouse DX,DY pixels a second while listed, mouse_left / mouse_right / mouse_middle /
 * mouse_x1 / mouse_x2 hold a button, wheel_up / wheel_down turn the wheel one notch each time the
 * file changes. They reach the game through the same path as the real mouse (sample_mouse). */
static struct { float rate_x, rate_y; uint32_t buttons; int wheel_up, wheel_down; } mouse_inject;
static void mouse_test_input(BbMouseState *m, int consume, float dt) {
    m->buttons|=mouse_inject.buttons;
    if (!consume) return;
    /* dt capped at 0.1 s as sample_mouse caps it (the rate then reads the same after a long gap
     * between samples: a game hitch, or pad-test starved on a busy machine, dropped the motion). */
    if (dt>0.0f) { const float t=dt<0.1f ? dt : 0.1f; m->dx+=mouse_inject.rate_x*t; m->dy+=mouse_inject.rate_y*t; }
    m->wheel_up+=mouse_inject.wheel_up; m->wheel_down+=mouse_inject.wheel_down;
    mouse_inject.wheel_up=mouse_inject.wheel_down=0;
}

static void sample_host(PadData *d) {
    report_guest_heap();
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    /* Mouse & keyboard mode (live: the menu switches it): its own key layout. */
    const uint64_t now=d->timestamp;
    const float dt=look_last ? (float)(now-look_last)/1e6f : 0.0f;
    const int consume=!look_last || dt>=0.004f;
    BbMouseState mouse;
    const int mk=bbgpu_mouse_keyboard(&mouse,consume);
    if (!mk) { look_x=look_y=0.0f; look_last=0; }
    else if (consume) look_last=now;
    static int bindings_generation;
    int bindings_changed=0;
    if (bindings_mode!=mk || mouse.bindings_generation!=bindings_generation) {
        bindings_changed=1;
        bindings_generation=mouse.bindings_generation;
        load_bindings(mk);
        note_mouse_bindings();
        if (bindings_mode>=0 && bindings_mode!=mk)
            printf("Runtime: controls: %s layout\n",mk ? "mouse & keyboard" : "classic keyboard");
        else if (bindings_mode>=0) printf("Runtime: controls: key bindings read again (in-game menu)\n");
        bindings_mode=mk;
    }
    if (bbgpu_overlay_captures_input() || bbgpu_text_input_is_active()) { /* settings menu open or typing: neutral input */
        look_x=look_y=0.0f; mouse_held=0; mouse_wheel_reset();
        prompt_note(g,0,0,bindings_changed); /* a key rebound in the menu: its prompt now */
        return;
    }
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    /* BB_PAD_FILE keyhold= keys: held as if pressed on the keyboard (bindings, prompts). */
    static bool key_state[SDL_SCANCODE_COUNT];
    if (key_inject_count) {
        if (k) memcpy(key_state,k,sizeof key_state); else memset(key_state,0,sizeof key_state);
        for (int i=0;i<key_inject_count;++i) key_state[key_inject[i]]=true;
        k=key_state;
    }
    if (mk) mouse_test_input(&mouse,consume,dt);
    mouse_held=mk ? mouse.buttons : 0;
    if (mk && mouse.wheel) mouse_wheel_step(now,mouse.wheel_up,mouse.wheel_down);
    else mouse_wheel_reset();
    if (g) {
        int touch_right=0;
        const int back_combo=back_combo_wanted(g);
        const int back_down=back_combo && SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_BACK);
        back_combo_masked=back_combo ? (back_down ? 2 : 1) : 0;
        for (int i=IN_CROSS;i<=IN_RIGHT;++i) {
            const int v=pad_value(g,i);
            if (i==IN_L2) d->l2=(uint8_t)v;
            if (i==IN_R2) d->r2=(uint8_t)v;
            if (i==IN_TOUCHPAD_RIGHT) touch_right=v>30;
            else if (v>30) d->buttons|=input_buttons[i];
        }
        static const SDL_GamepadAxis stick_axes[4]={SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY,
                                                    SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY};
        int16_t raw[4];
        for (int i=0;i<4;++i) raw[i]=(int16_t)SDL_GetGamepadAxis(g,stick_axes[i]);
        cal_load();
        cal_sample(raw);
        uint8_t *axis_out[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
        for (int i=0;i<4;++i)
            *axis_out[i]=stick_axis(i,raw[i]); /* center 0 until the neutral is known */
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        back_combo_masked=0;
        if (back_combo) {
            /* I2 pings on pads without a touch surface: Back held + R3 is the ping (the touchpad
             * click only comes on Back's release here, so ping_combo never sees it held); I5
             * Back held + L3 gathers the party (host). The stick click then reaches the game only
             * after it is released. */
            static BackClickCombo back_r3, back_l3;
            int pinged=0;
            if (back_click_combo(&back_r3,back_down,&d->buttons,BTN_R3,bbgpu_ping_combo_enabled)) {
                pinged=1;
                bbgpu_ping_request();
                puts("Runtime: ping (Back + R3)");
            }
            if (back_click_combo(&back_l3,back_down,&d->buttons,BTN_L3,bbgpu_gather_combo_enabled)) {
                pinged=1;
                bbgpu_gather_request("Back + L3");
                puts("Runtime: gather (Back + L3)");
            }
            const int side=back_combo_step(back_down,
                back_down && SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_DPAD_LEFT),
                back_down && SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_DPAD_RIGHT),pinged,d->timestamp);
            if (side>=0 && !d->touch_count) touch_click(d,side);
        }
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
        if (touch_right) touch_click(d,1);
    }
    /* Button prompts: the device used last (a scripted pad file's buttons and sticks are the pad's). */
    {
        int pad_active=(g && (d->buttons || d->l2>30 || d->r2>30 || d->touch_count ||
                              stick_moved(d->left_x) || stick_moved(d->left_y) ||
                              stick_moved(d->right_x) || stick_moved(d->right_y))) || injected.buttons;
        for (int i=0;i<4;++i) pad_active|=injected.stick[i]>=0 && stick_moved((uint8_t)injected.stick[i]);
        int keyboard_active=mk && (mouse.buttons || mouse.wheel_up || mouse.wheel_down ||
                                   mouse.dx*mouse.dx+mouse.dy*mouse.dy>4.0f);
        /* The walk key alone (Alt, as in Alt+Tab) moves nothing: the prompts stay. */
        for (int i=0;i<IN_COUNT && !keyboard_active;++i) keyboard_active=i!=IN_WALK && key_down(k,i);
        prompt_note(g,pad_active,keyboard_active,bindings_changed);
    }
    if (mk) sample_mouse(d,&mouse,consume,dt,k);
    /* Alt+Enter toggles fullscreen (window.cpp): its Enter is not the game's Cross. The bound
     * mouse buttons and the wheel count without a keyboard state too. */
    if (!(k && (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]) &&
          (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]))) apply_keyboard(d,k);
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
/* runtime_pad_director_press: buttons the party director holds (title "Continue" on its own, no
 * OS input); OR-ed into every sample like the pad file's. */
static volatile uint32_t director_buttons;
void runtime_pad_director_press(uint32_t buttons) { director_buttons=buttons; }
/* runtime_pad_director_sticks: the sticks the director holds (0..255; -1 leaves that axis to the
 * player): the world warper's camera sweeps and walk loops (gpu/shim/party/world_warper.h). */
static volatile int director_stick[4]={-1,-1,-1,-1};
void runtime_pad_director_sticks(int lx, int ly, int rx, int ry) {
    director_stick[0]=lx; director_stick[1]=ly; director_stick[2]=rx; director_stick[3]=ry;
}
/* runtime_pad_last_buttons: the buttons of the last sample the game read (test aids' logs). */
static volatile uint32_t last_sample_buttons;
uint32_t runtime_pad_last_buttons(void) { return last_sample_buttons; }
/* runtime_pad_last_left: the left stick of the last sample the game read, lx | ly << 8 (0..255,
 * 128 neutral; the party's host-death lock log compares it with the chr's movement). */
static volatile uint32_t last_sample_left=0x8080u;
uint32_t runtime_pad_last_left(void) { return last_sample_left; }
/* runtime_pad_cross_presses: Cross presses (rising edges) the game read, counted here so a short
 * tap is never missed by a reader that looks once a frame (the game menu's button rows). */
static volatile uint32_t cross_presses;
uint32_t runtime_pad_cross_presses(void) { return cross_presses; }
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check;
#ifndef _WIN32
    static struct timespec mtime;
#endif
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
#ifdef _WIN32
    static FILETIME last_ft;
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    GetFileTime(hFile, NULL, NULL, &ft);
    CloseHandle(hFile);
    if (ft.dwLowDateTime == last_ft.dwLowDateTime && ft.dwHighDateTime == last_ft.dwHighDateTime) return;
    last_ft = ft;
#else
    struct stat st;
    if (stat(path,&st)!=0) return;
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    static const struct { const char *name; uint32_t bit; } mouse_names[]={
        {"mouse_left",1u}, {"mouse_right",2u}, {"mouse_middle",4u}, {"mouse_x1",8u}, {"mouse_x2",16u},
    };
    mouse_inject.rate_x=mouse_inject.rate_y=0.0f;
    mouse_inject.buttons=0;
    injected.buttons=0;
    injected.touch_side=-1;
    key_inject_count=0;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
        for (size_t i=0;i<sizeof(mouse_names)/sizeof(*mouse_names);++i)
            if (!strcmp(token,mouse_names[i].name)) mouse_inject.buttons|=mouse_names[i].bit;
        if (!strncmp(token,"mouse=",6)) {
            char *end=NULL;
            mouse_inject.rate_x=strtof(token+6,&end);
            mouse_inject.rate_y=end && *end==',' ? strtof(end+1,NULL) : 0.0f;
        }
        if (!strcmp(token,"wheel_up")) ++mouse_inject.wheel_up;
        if (!strcmp(token,"wheel_down")) ++mouse_inject.wheel_down;
        /* keyhold=NAME: a binding line's key name (Space; Left_Shift for Left Shift; rshift) held
         * on the keyboard while listed, through the bindings like a real key (prompt tests). */
        if (!strncmp(token,"keyhold=",8)) {
            char name[64]; snprintf(name,sizeof name,"%s",token+8);
            for (char *c=name;*c;++c) if (*c=='_') *c=' ';
            const int sc=key_from_name(name);
            if (sc<0 || sc>=SDL_SCANCODE_COUNT) printf("Runtime: pad file: unknown key %s\n",name);
            else if (key_inject_count<KEY_INJECT_MAX) key_inject[key_inject_count++]=sc;
        }
        /* key=NAME: a key press queued into the game window (the overlay menu: key=Insert). */
        if (!strncmp(token,"key=",4) && !bbgpu_test_key(token+4)) printf("Runtime: pad file: unknown key %s\n",token+4);
    }
    fclose(f);
    if (mouse_inject.rate_x!=0.0f || mouse_inject.rate_y!=0.0f || mouse_inject.buttons)
        printf("Runtime: pad file: mouse %.0f,%.0f px/s buttons 0x%x\n",mouse_inject.rate_x,mouse_inject.rate_y,
               mouse_inject.buttons);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d keys %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3],key_inject_count);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* Touches as the DualShock 4 reports them: every finger that goes down gets a new id (1..127, kept
 * while it stays down) and the time since the first one went down. With id 0 and no hold time
 * the game ignored touchpad presses: no gesture menu from the touchpad, Back or Tab. */
static void touch_ids(PadData *d) {
    static uint8_t next_id=1, ids[2]; static int down[2]; static uint64_t since;
    for (int i=0;i<2;++i) {
        const int now=i<d->touch_count;
        if (now && !down[i]) { ids[i]=next_id; next_id=next_id==127 ? 1 : next_id+1; }
        down[i]=now;
        if (now) d->touches[i].id=ids[i];
    }
    if (!d->touch_count) since=0;
    else if (!since) since=d->timestamp;
    d->touch_held_time=d->touch_count ? (uint32_t)(d->timestamp-since) : 0;
}
/* I2 pings (gpu/shim/party/party_ping.h): in a party with pings on, holding the touchpad (Tab)
 * and pressing R3 (Q / C) places a ping. The touchpad press is held back from the game until it
 * is released - then the game gets a short click (the Gestures menu opens on release) - or R3
 * comes: a ping, and the game sees neither button until both are released. I5 (party_gather.h):
 * on the host, the touchpad held + L3 (Z / C) gathers the party the same way. */
static void ping_combo(PadData *d) {
    static int state, right, r3_prev, l3_prev; /* state 0 idle, 1 touchpad held back, 2 combo sent */
    static uint32_t combo_button; /* the combo's stick click (BTN_R3 / BTN_L3), held back too */
    static uint64_t click_until;
    const uint64_t now=now_us();
    const int tp=(d->buttons & BTN_TOUCHPAD)!=0, r3=(d->buttons & BTN_R3)!=0, l3=(d->buttons & BTN_L3)!=0;
    const int r3_rise=r3 && !r3_prev, l3_rise=l3 && !l3_prev;
    r3_prev=r3;
    l3_prev=l3;
    switch (state) {
    case 0:
        if (!tp || !(bbgpu_ping_combo_enabled() || bbgpu_gather_combo_enabled())) {
            if (click_until) {
                if (now<click_until) touch_click(d,right);
                else click_until=0;
            }
            return;
        }
        state=1; click_until=0;
        right=d->touch_count>0 && d->touches[0].x>=960;
        /* the stick click pressed on the same sample (a director's tap) counts too */
        /* fall through */
    case 1:
        if (r3_rise && bbgpu_ping_combo_enabled()) {
            state=2; combo_button=BTN_R3;
            bbgpu_ping_request(); puts("Runtime: ping (touchpad + R3)");
            break;
        }
        if (l3_rise && bbgpu_gather_combo_enabled()) {
            state=2; combo_button=BTN_L3;
            bbgpu_gather_request("touchpad + L3"); puts("Runtime: gather (touchpad + L3)");
            break;
        }
        if (!tp) { state=0; click_until=now+120000; touch_click(d,right); return; }
        break;
    default:
        if (!tp && !(d->buttons & combo_button)) { state=0; return; }
        break;
    }
    d->buttons&=~BTN_TOUCHPAD;
    if (state==2) d->buttons&=~combo_button;
    d->touch_count=0;
    touch_down_us=0;
}
/* Set while the settings menu or the name box holds the pad: the press that closed it (Cross on
 * the name, Insert/L3+R3) stays held, and must not reach the game as a new press. Buttons are
 * held neutral until everything is released (from Supermedo/bloodborne_pc 1.4). */
static int hold_after_capture;
static void sample(PadData *d) {
    /* Read first: the file's key= steps also drive the open settings menu (live tests). */
    read_inject();
    sample_host(d);
    bbgpu_reflex_on_input_sampled(); /* NVIDIA Reflex input marker (no-op without Reflex) */
    if (bbgpu_overlay_captures_input() || bbgpu_text_input_is_active()) { hold_after_capture=1; return; }
    if (hold_after_capture) {
        if (d->buttons || d->l2>30 || d->r2>30) { d->buttons=0; d->l2=d->r2=0; touch_ids(d); return; }
        hold_after_capture=0;
    }
    record_sample(d);
    replay_sample(d);
    d->buttons|=injected.buttons|director_buttons;
    if ((d->buttons & BTN_CROSS) && !(last_sample_buttons & BTN_CROSS)) cross_presses=cross_presses+1;
    last_sample_buttons=d->buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (!d->touch_count) touch_down_us = 0;
    ping_combo(d);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    if (director_buttons & BTN_L2) d->l2=255;
    if (director_buttons & BTN_R2) d->r2=255;
    for (int i=0;i<4;++i) { const int v=director_stick[i]; if (v>=0) *axes[i]=(uint8_t)(v>255 ? 255 : v); }
    last_sample_left=(uint32_t)d->left_x|(uint32_t)d->left_y<<8;
    touch_ids(d);
}

/* Button prompts, asked by the renderer's thread (gpu/shim/bbport_button_prompts.cpp). */
unsigned runtime_pad_prompt_generation(void) { return __atomic_load_n(&prompt_generation,__ATOMIC_RELAXED); }
/* The set shown (PROMPT_SET_*), -1 before the first sample. */
int runtime_pad_prompt_set(void) { return __atomic_load_n(&prompt_set,__ATOMIC_RELAXED); }
/* Mouse & keyboard mode: the mouse turns the camera (the right stick's prompt is the mouse). */
int runtime_pad_mouse_look(void) { return __atomic_load_n(&bindings_mode,__ATOMIC_RELAXED)==1; }
/* The prompt of PS input `name` (input_names: cross ... look_right) in the set shown: its label
 * (UTF-8) in `label`, the kind as returned (pad_prompts.h: 0 the game's icon, 'K' 'M' 'F' 'S'
 * 'C'). `single`: an icon of one input (a mouse button may stand for it); 0 for a key cluster
 * (the d-pad's or a stick's keys). Mouse & keyboard mode's mouse scheme (sample_mouse) counts
 * for the buttons it drives: Left R1, Shift+Left R2, Right L2, Shift+Right L1, Middle R3. */
int runtime_pad_prompt(const char *name, int single, char *label, int size) {
    if (!name || !label || size<8) return 0;
    label[0]=0;
    int input=-1;
    for (int i=0;i<IN_COUNT;++i) if (!strcmp(name,input_names[i])) input=i;
    if (input<0) return 0;
    host_lock(&lock);
    const int set=prompt_set, mk=bindings_mode==1, loaded=bindings_mode>=0;
    const SDL_GamepadType type=prompt_pad_type;
    const Binding b=bindings[input];
    const uint32_t mouse_bound=mouse_bound_mask;
    host_unlock(&lock);
    if (!loaded || set<0 || set==PROMPT_SET_PLAYSTATION) return 0;
    if (set!=PROMPT_SET_KEYBOARD) {
        if (input==IN_WALK) return 0; /* a keyboard-only input */
        if (input>=IN_MOVE_UP) { snprintf(label,(size_t)size,"%s",input<IN_LOOK_UP ? "LS" : "RS"); return 'C'; }
        if (b.pad_count==0) return 0;
        return prompt_pad_label(set,type,b.pad[0],label,size);
    }
    /* The keyboard: with mouse & keyboard mode a mouse button first (the hand is on it). */
    if (single && mk) {
        for (int i=0;i<b.key_count;++i)
            if (b.keys[i]>=MOUSE_KEY) {
                const char *n=key_name(b.keys[i]);
                snprintf(label,(size_t)size,"%s",!strncmp(n,"Mouse ",6) ? n+6 : n);
                return 'M';
            }
        static const struct { int input; uint32_t bit; const char *label; } scheme[]={
            {IN_R1,1u,"Left"}, {IN_R2,1u,"\xe2\x87\xa7Left"}, {IN_L2,2u,"Right"},
            {IN_L1,2u,"\xe2\x87\xa7Right"}, {IN_R3,4u,"Middle"},
        };
        for (size_t i=0;i<sizeof(scheme)/sizeof(*scheme);++i)
            if (scheme[i].input==input && !(mouse_bound & scheme[i].bit)) {
                snprintf(label,(size_t)size,"%s",scheme[i].label);
                return 'M';
            }
    }
    for (int i=0;i<b.key_count;++i)
        if (b.keys[i]<MOUSE_KEY) {
            prompt_key_label(key_name(b.keys[i]),label,size);
            return 'K';
        }
    return 0;
}

static ABI int32_t pad_init(void) { host_lock(&lock); initialized=1; host_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    host_lock(&lock);
    int already=opened; opened=1;
    host_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    bbgpu_reflex_pre_input_sample(); /* NVIDIA Reflex: driver pacing before the simulation reads input */
    host_lock(&lock);
    sample(data); ++reads;
    host_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    host_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    host_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    host_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    host_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
