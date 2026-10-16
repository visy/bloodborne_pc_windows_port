#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "test_portable.h"
#ifdef _WIN32
#define usleep(us) Sleep((us) / 1000)
#endif
#include "../src/runtime_pad.c"

static int capture;
int bbgpu_overlay_captures_input(void) { return capture; }
int bbgpu_text_input_is_active(void) { return 0; }
static int ping_on, pings;
int bbgpu_ping_combo_enabled(void) { return ping_on; }
void bbgpu_ping_request(void) { ++pings; }
void bbgpu_reflex_pre_input_sample(void) {}
void bbgpu_reflex_on_input_sampled(void) {}
/* Mouse & keyboard mode (after Mrsuss60/bloodborne_pc_windows_port's test stubs). */
static int mk_mode;
static float mk_dx;
static uint32_t mk_buttons;
static int mk_wheel_down;
static int bind_generation; /* the in-game menu rewrote key lines (BbMouseState.bindings_generation) */
int bbgpu_mouse_keyboard(BbMouseState *state, int consume_motion) {
    memset(state,0,sizeof(*state));
    state->bindings_generation=bind_generation;
    state->dx=mk_dx; state->buttons=mk_buttons;
    state->sens_x=state->sens_y=1.0f; state->deadzone=0.05f; state->smoothing=0.0f;
    state->wheel=1;
    if (consume_motion) { mk_dx=0.0f; state->wheel_down=mk_wheel_down; mk_wheel_down=0; }
    return mk_mode;
}
int bbgpu_test_key(const char *name) { (void)name; return 1; }
static int prompt_style_setting; /* bbport.ini prompt_style (PROMPT_STYLE_*) */
int bbgpu_prompt_style(void) { return prompt_style_setting; }

/* Button prompts (src/pad_prompts.h): the pad type's set, the prompt_style override, SDL's letters
 * (Nintendo: the bottom button is B), short key names. */
static void test_prompt_logic(void) {
    char l[32];
    assert(prompt_set_for_type(SDL_GAMEPAD_TYPE_PS4)==PROMPT_SET_PLAYSTATION &&
           prompt_set_for_type(SDL_GAMEPAD_TYPE_PS5)==PROMPT_SET_PLAYSTATION &&
           prompt_set_for_type(SDL_GAMEPAD_TYPE_PS3)==PROMPT_SET_PLAYSTATION);
    assert(prompt_set_for_type(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO)==PROMPT_SET_NINTENDO &&
           prompt_set_for_type(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR)==PROMPT_SET_NINTENDO &&
           prompt_set_for_type(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT)==PROMPT_SET_NINTENDO &&
           prompt_set_for_type(SDL_GAMEPAD_TYPE_GAMECUBE)==PROMPT_SET_NINTENDO);
    assert(prompt_set_for_type(SDL_GAMEPAD_TYPE_XBOXONE)==PROMPT_SET_XBOX &&
           prompt_set_for_type(SDL_GAMEPAD_TYPE_XBOX360)==PROMPT_SET_XBOX &&
           prompt_set_for_type(SDL_GAMEPAD_TYPE_STANDARD)==PROMPT_SET_XBOX &&
           prompt_set_for_type(SDL_GAMEPAD_TYPE_UNKNOWN)==PROMPT_SET_XBOX);
    /* Auto: the last device; an override names the pad's set, Keyboard wins always. */
    assert(prompt_set_choose(PROMPT_STYLE_AUTO,1,SDL_GAMEPAD_TYPE_PS5)==PROMPT_SET_KEYBOARD);
    assert(prompt_set_choose(PROMPT_STYLE_AUTO,0,SDL_GAMEPAD_TYPE_PS5)==PROMPT_SET_PLAYSTATION);
    assert(prompt_set_choose(PROMPT_STYLE_AUTO,0,SDL_GAMEPAD_TYPE_XBOX360)==PROMPT_SET_XBOX);
    assert(prompt_set_choose(PROMPT_STYLE_NINTENDO,0,SDL_GAMEPAD_TYPE_XBOX360)==PROMPT_SET_NINTENDO);
    assert(prompt_set_choose(PROMPT_STYLE_NINTENDO,1,SDL_GAMEPAD_TYPE_XBOX360)==PROMPT_SET_KEYBOARD);
    assert(prompt_set_choose(PROMPT_STYLE_XBOX,0,SDL_GAMEPAD_TYPE_PS4)==PROMPT_SET_XBOX);
    assert(prompt_set_choose(PROMPT_STYLE_PLAYSTATION,0,SDL_GAMEPAD_TYPE_XBOXONE)==PROMPT_SET_PLAYSTATION);
    assert(prompt_set_choose(PROMPT_STYLE_KEYBOARD,0,SDL_GAMEPAD_TYPE_XBOXONE)==PROMPT_SET_KEYBOARD);
    /* Letters by SDL's labels for the set's pad type: Nintendo swaps them, the function stays. */
    static const struct { int set; SDL_GamepadType type; int button; const char *label; } letters[]={
        {PROMPT_SET_XBOX,SDL_GAMEPAD_TYPE_XBOXONE,SDL_GAMEPAD_BUTTON_SOUTH,"A"},
        {PROMPT_SET_XBOX,SDL_GAMEPAD_TYPE_XBOXONE,SDL_GAMEPAD_BUTTON_EAST,"B"},
        {PROMPT_SET_XBOX,SDL_GAMEPAD_TYPE_XBOXONE,SDL_GAMEPAD_BUTTON_WEST,"X"},
        {PROMPT_SET_XBOX,SDL_GAMEPAD_TYPE_XBOXONE,SDL_GAMEPAD_BUTTON_NORTH,"Y"},
        /* the 8BitDo case: an Xbox 360 type pad with Nintendo letters chosen */
        {PROMPT_SET_NINTENDO,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_BUTTON_SOUTH,"B"},
        {PROMPT_SET_NINTENDO,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_BUTTON_EAST,"A"},
        {PROMPT_SET_NINTENDO,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_BUTTON_WEST,"Y"},
        {PROMPT_SET_NINTENDO,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_BUTTON_NORTH,"X"},
        {PROMPT_SET_NINTENDO,SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,SDL_GAMEPAD_BUTTON_SOUTH,"B"},
        /* Xbox chosen for a Switch pad: Xbox letters */
        {PROMPT_SET_XBOX,SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,SDL_GAMEPAD_BUTTON_SOUTH,"A"},
        {PROMPT_SET_XBOX,SDL_GAMEPAD_TYPE_PS4,SDL_GAMEPAD_BUTTON_NORTH,"Y"},
    };
    for (size_t i=0;i<sizeof(letters)/sizeof(*letters);++i) {
        assert(prompt_pad_label(letters[i].set,letters[i].type,letters[i].button,l,sizeof l)=='F');
        if (strcmp(l,letters[i].label)) { printf("letter %zu: %s, not %s\n",i,l,letters[i].label); assert(0); }
    }
    assert(prompt_pad_label(PROMPT_SET_NINTENDO,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,l,sizeof l)=='S' && !strcmp(l,"L"));
    assert(prompt_pad_label(PROMPT_SET_NINTENDO,SDL_GAMEPAD_TYPE_XBOX360,PROMPT_PAD_RIGHT_TRIGGER,l,sizeof l)=='S' && !strcmp(l,"ZR"));
    assert(prompt_pad_label(PROMPT_SET_NINTENDO,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_BUTTON_START,l,sizeof l)=='C' && !strcmp(l,"+"));
    assert(prompt_pad_label(PROMPT_SET_NINTENDO,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_BUTTON_BACK,l,sizeof l)=='C' && !strcmp(l,"-"));
    assert(prompt_pad_label(PROMPT_SET_XBOX,SDL_GAMEPAD_TYPE_XBOX360,PROMPT_PAD_LEFT_TRIGGER,l,sizeof l)=='S' && !strcmp(l,"LT"));
    assert(prompt_pad_label(PROMPT_SET_XBOX,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_BUTTON_START,l,sizeof l)=='C' && !strcmp(l,"Menu"));
    assert(prompt_pad_label(PROMPT_SET_XBOX,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_BUTTON_DPAD_UP,l,sizeof l)==0); /* the game's d-pad */
    assert(prompt_pad_label(PROMPT_SET_PLAYSTATION,SDL_GAMEPAD_TYPE_PS4,SDL_GAMEPAD_BUTTON_SOUTH,l,sizeof l)==0);
    /* Key caps: short names, arrows. */
    static const struct { const char *sdl, *cap; } keys[]={
        {"Escape","Esc"}, {"Backspace","Bksp"}, {"Return","\xe2\x86\xb5"}, {"Left Shift","\xe2\x87\xa7"},
        {"Up","\xe2\x86\x91"}, {"Left","\xe2\x86\x90"}, {"Keypad 5","Num5"}, {"Space","\xe2\x90\xa3"},
        {"PageDown","PgDn"}, {"E","E"}, {"",""},
    };
    for (size_t i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        prompt_key_label(keys[i].sdl,l,sizeof l);
        assert(!strcmp(l,*keys[i].sdl ? keys[i].cap : "?"));
    }
}
#ifdef _WIN32
uint64_t host_monotonic_ns(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#endif
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

/* Keyboard sticks (after Supermedo/bloodborne_pc a147eb8, 576514c, 4b930b10): last-wins opposite
 * keys, a square gate (the larger axis all the way: a run at every angle), turns in steps of at
 * most 30 degrees and a released direction held 60 ms (cassioKenji's measurements, PR #74 there).
 * Classic layout: W/A/S/D move. */
static double stick_deg(int x, int y) { return atan2((double)y,(double)x)*180.0/3.14159265358979323846; }
static void test_keyboard_sticks(void) {
    static bool k[SDL_SCANCODE_COUNT];
    PadData d;
    memset(&key_move_stick,0,sizeof(key_move_stick));
    memset(&d,0,sizeof(d)); d.left_x=d.left_y=d.right_x=d.right_y=128;
    k[SDL_SCANCODE_A]=true; apply_keyboard(&d,k); assert(d.left_x==0 && d.left_y==128);
    k[SDL_SCANCODE_D]=true; apply_keyboard(&d,k);
    assert(d.left_x!=128 || d.left_y!=128); /* D pressed last: never a neutral stick on the way */
    k[SDL_SCANCODE_A]=k[SDL_SCANCODE_D]=false;
    memset(&key_move_stick,0,sizeof(key_move_stick));
    memset(&d,0,sizeof(d)); d.left_x=d.left_y=d.right_x=d.right_y=128;
    k[SDL_SCANCODE_W]=k[SDL_SCANCODE_D]=true; apply_keyboard(&d,k);
    assert(d.left_x==255 && d.left_y==0); /* the square's corner: the larger axis all the way */
    k[SDL_SCANCODE_W]=k[SDL_SCANCODE_D]=false;
    memset(&key_move_stick,0,sizeof(key_move_stick));

    /* Timed: a 60 Hz read; A then D turns through forward, at most 30 degrees a read, the larger
     * axis always 127, and arrives. */
    KeyStickState s; memset(&s,0,sizeof(s));
    uint64_t t=1000000; int x, y, reads=0;
    assert(key_stick_step(&s,-1,0,t,&x,&y) && x==-127 && y==0);
    double prev=stick_deg(x,y);
    do {
        t+=16667;
        assert(key_stick_step(&s,1,0,t,&x,&y));
        const double now=stick_deg(x,y);
        double turn=fabs(fmod(now-prev+540.0,360.0)-180.0);
        assert(turn<=30.5);
        assert((abs(x)==127 || abs(y)==127));
        if (y) assert(y<0); /* the half turn goes through forward (W), not back */
        prev=now; ++reads;
    } while ((x!=127 || y!=0) && reads<40);
    assert(x==127 && y==0 && reads>=6 && reads<=20);
    /* A long gap between reads still turns at most 30 degrees. */
    t+=200000;
    assert(key_stick_step(&s,0,-1,t,&x,&y));
    assert(fabs(fmod(stick_deg(x,y)+540.0,360.0)-180.0)<=30.5+0.0); /* from 0 deg by 30 */
    /* Released: the last direction is held 60 ms, then neutral. */
    int hx=x, hy=y;
    assert(key_stick_step(&s,0,0,t+=1000,&x,&y) && x==hx && y==hy);
    assert(key_stick_step(&s,0,0,t+=40000,&x,&y) && x==hx && y==hy);
    assert(!key_stick_step(&s,0,0,t+=30000,&x,&y));
    /* Pressed again after neutral: the new direction at once. */
    assert(key_stick_step(&s,0,1,t+=16667,&x,&y) && x==0 && y==127);
    /* A sideways half turn (W <-> S ... here S -> W) goes through the right. */
    t+=16667; assert(key_stick_step(&s,0,-1,t,&x,&y) && x>0);
}

/* Xbox pads: Back + d-pad left/right clicks the touchpad halves (after Supermedo/bloodborne_pc
 * a04e1e95, parsifal295's PR #39); Back alone clicks the left half for 150 ms on release. */
static void test_back_combo(void) {
    uint64_t t=1000000;
    assert(back_combo_step(0,0,0,0,t)==-1);
    assert(back_combo_step(1,0,0,0,t+=10000)==-1);       /* Back held alone: nothing yet */
    assert(back_combo_step(0,0,0,0,t+=10000)==0);        /* released: a left-half click */
    assert(back_combo_step(0,0,0,0,t+=100000)==0);       /* ... for 150 ms */
    assert(back_combo_step(0,0,0,0,t+=60000)==-1);
    assert(back_combo_step(1,0,0,0,t+=10000)==-1);
    assert(back_combo_step(1,0,1,0,t+=10000)==1);        /* Back + right: the right half */
    assert(back_combo_step(1,0,1,0,t+=10000)==1);        /* while the d-pad is held */
    assert(back_combo_step(1,0,0,0,t+=10000)==-1);
    assert(back_combo_step(0,0,0,0,t+=10000)==-1);       /* no left click after a combo */
    assert(back_combo_step(1,1,0,0,t+=10000)==0);        /* Back + left: the left half */
    assert(back_combo_step(0,0,0,0,t+=10000)==-1);
    assert(back_combo_step(1,0,0,1,t+=10000)==-1);     /* Back + R3: a ping (I2) */
    assert(back_combo_step(0,0,0,0,t+=10000)==-1);     /* no gestures click after it */
    /* The masking: Back reads released, and while it is held, d-pad left/right too. Without a
     * gamepad pad_value reads nothing, so only the binding rule is checked here. */
    assert(back_combo_wanted(NULL)==0);
}

int main(void) {
    char path[512];
    load_bindings(0);
    test_keyboard_sticks();
    test_back_combo();
    const int created=bb_test_temp_file(path,sizeof(path),"bbport-pad-test-");
    assert(created==0);
    bb_test_setenv("BB_PAD_FILE",path);
    /* bbport.ini controls: buttons moved, a trigger as a button and a button as a trigger. */
    char config[512];
    const int config_created=bb_test_temp_file(config,sizeof(config),"bbport-pad-config-");
    assert(config_created==0);
    const char controls[]="upscaler=fsr3\npad.cross=b\npad.circle=a\npad.r2=rightshoulder\n"
                          "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\nmkkey.triangle=T\n"
                          "mkkey.square=R, Mouse X1\n";
    const int written=bb_test_write_file(config,controls,sizeof(controls)-1);
    assert(written==0);
    bb_test_setenv("BB_CONFIG",config);
    bb_test_setenv("SDL_VIDEODRIVER","dummy");
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    /* A new touch gets a new id (1..127), as from a DualShock 4: the game ignores id 0. */
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471 && data.touches[0].id==1);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==2);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==3);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    /* The capture above holds the pad neutral until everything is released (hold_after_capture,
     * runtime_pad.c): the touchpad button was still down. Released now, input flows again. */
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==(BTN_CROSS|BTN_R2|BTN_R1) && data.r2==255);
    assert(bindings[IN_CROSS].key_count==2 && bindings[IN_CROSS].keys[0]==SDL_SCANCODE_X &&
           bindings[IN_CROSS].keys[1]==SDL_SCANCODE_SPACE);
    /* Mouse & keyboard mode: its own keys (mkkey.* lines, key.* ignored), the left button R1, the
     * mouse's speed on the right stick. The gamepad is released first (hold_after_capture). */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768)); /* raw: released */
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    mk_mode=1; mk_buttons=1;
    assert(pad_read_state(1,&data)==0);
    assert(bindings[IN_CROSS].keys[0]==SDL_SCANCODE_E && bindings[IN_TRIANGLE].key_count==1 &&
           bindings[IN_TRIANGLE].keys[0]==SDL_SCANCODE_T && bindings[IN_CIRCLE].keys[0]==SDL_SCANCODE_SPACE);
    assert(data.buttons==BTN_R1 && data.right_x==128);
    usleep(20000);
    mk_dx=100.0f; /* 5000 px/s: full deflection */
    assert(pad_read_state(1,&data)==0 && data.right_x==255);
    /* The measured camera curve (deadzone compensation, after 90008b39 by Mrsuss60): the
     * slowest motion past the mouse dead zone already turns (past the game's 46.7), the turn
     * grows with the motion, stays below the game's fast turn (from ~122) until full speed. */
    assert(look_deflection(0.0f)==0.0f && look_deflection(0.01f)>46.7f && look_deflection(0.01f)<48.0f);
    assert(look_deflection(0.5f)<look_deflection(0.6f) && look_deflection(0.999f)<=120.0f &&
           look_deflection(1.0f)==127.0f);
    {
        float ox=0.0f, oy=0.0f;
        look_stick(4.0f,0.0f,6.35f,&ox,&oy);     /* below the mouse dead zone */
        assert(ox==0.0f && oy==0.0f);
        look_stick(30.0f,-30.0f,6.35f,&ox,&oy);  /* round: the direction kept */
        assert(ox>33.0f && oy<-33.0f && fabsf(ox+oy)<0.01f);
    }
    /* Upstream 0.51 #52: mouse names in bindings; a bound side button leaves the built-in scheme
     * (X1 is Square here, not L1); the wheel a short d-pad press per notch; pad-file mouse tokens. */
    assert(key_from_name("Mouse Left")==MOUSE_KEY+SDL_BUTTON_LEFT && key_from_name("wheel down")==MOUSE_WHEEL_DOWN);
    assert(bindings[IN_SQUARE].key_count==2 && bindings[IN_SQUARE].keys[1]==MOUSE_KEY+SDL_BUTTON_X1);
    mk_buttons=8;
    usleep(5000);
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_SQUARE) && !(data.buttons & BTN_L1));
    mk_buttons=0;
    usleep(5000);
    mk_wheel_down=1;
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_DOWN));
    usleep(70000);
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_DOWN));
    usleep(70000);
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_DOWN));
    mouse_wheel_step(1000000,0,2);
    assert(wheel_phase[1]==1 && mouse_key_down(MOUSE_WHEEL_DOWN));
    mouse_wheel_step(1070000,0,0);
    assert(!mouse_key_down(MOUSE_WHEEL_DOWN));
    mouse_wheel_step(1140000,0,0);
    assert(mouse_key_down(MOUSE_WHEEL_DOWN) && wheel_pending[1]==0);
    mouse_wheel_reset();
    inject(path,"mouse_right mouse=3000,0");
    usleep(5000);
    assert(pad_read_state(1,&data)==0);
    usleep(10000);
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_L2) && data.l2==255 && data.right_x>200);
    inject(path,"");
    for (int i=0;i<30;++i) { usleep(5000); pad_read_state(1,&data); }
    assert(!(data.buttons & BTN_L2) && data.right_x==128);
    /* The in-game menu's key rows (90008b39 by Mrsuss60): new lines are read again as soon as
     * the generation changes, without a layout switch; a mouse button named there leaves the
     * built-in scheme (left now Cross, not R1). */
    const char rebound[]="upscaler=fsr3\nkey.cross=X, Space\nmkkey.triangle=Y\nmkkey.cross=Mouse Left\n";
    assert(bb_test_write_file(config,rebound,sizeof(rebound)-1)==0);
    assert(pad_read_state(1,&data)==0 && bindings[IN_TRIANGLE].keys[0]==SDL_SCANCODE_T);
    ++bind_generation;
    mk_buttons=1;
    assert(pad_read_state(1,&data)==0 && bindings[IN_TRIANGLE].keys[0]==SDL_SCANCODE_Y);
    assert((data.buttons & BTN_CROSS) && !(data.buttons & BTN_R1));
    mk_buttons=0;
    assert(bb_test_write_file(config,controls,sizeof(controls)-1)==0);
    ++bind_generation;
    assert(pad_read_state(1,&data)==0 && bindings[IN_TRIANGLE].keys[0]==SDL_SCANCODE_T);
    /* src/pad_bindings.h names: a binding line's name for each code. */
    assert(!strcmp(key_name(MOUSE_KEY+SDL_BUTTON_X2),"Mouse X2") && !strcmp(key_name(SDL_SCANCODE_Q),"Q") &&
           key_from_name(key_name(MOUSE_WHEEL_UP))==MOUSE_WHEEL_UP && !*key_name(0));
    mk_mode=0; mk_buttons=0;
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_R1) && data.right_x==128 &&
           bindings[IN_CROSS].keys[0]==SDL_SCANCODE_X);
    /* I2 pings: touchpad + R3 is a ping and reaches the game as neither; the touchpad alone is
     * held back while down and given to the game as a short click when released. */
    ping_on=1;
    inject(path,"touchpad");
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_TOUCHPAD) && data.touch_count==0);
    inject(path,"touchpad r3");
    assert(pad_read_state(1,&data)==0 && !(data.buttons & (BTN_TOUCHPAD|BTN_R3)) && data.touch_count==0 && pings==1);
    inject(path,"r3");
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_R3) && pings==1); /* held back until released */
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    inject(path,"r3");
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_R3) && pings==1); /* R3 alone: the game's */
    inject(path,"touchpad r3");
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_TOUCHPAD) && (data.buttons & BTN_R3) &&
           pings==1); /* R3 held before the touchpad: the game's lock-on, no ping */
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_TOUCHPAD) && data.touch_count==1 && pings==1);
    usleep(150000);
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_TOUCHPAD) && data.touch_count==0);
    ping_on=0;
    inject(path,"touchpad r3");
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_TOUCHPAD) && (data.buttons & BTN_R3) && pings==1);
    inject(path,"");
    pad_read_state(1,&data);
    /* Button prompts in the runtime: the device used last (a scripted keyboard key, a scripted pad
     * button), the set (the virtual pad's type, prompt_style), labels from the bindings
     * (key.cross=X, Space; pad.cross=b; pad.r1=righttrigger), a redraw after a rebinding. */
    {
        char l[32];
        test_prompt_logic();
        const SDL_GamepadType pad_type=SDL_GetGamepadType(gamepad);
        const int pad_set=prompt_set_for_type(pad_type);
        assert(pad_set!=PROMPT_SET_PLAYSTATION);
        inject(path,"keyhold=Space");
        assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS));
        assert(runtime_pad_prompt_set()==PROMPT_SET_KEYBOARD);
        unsigned gen=runtime_pad_prompt_generation();
        assert(gen!=0);
        assert(runtime_pad_prompt("cross",1,l,sizeof l)=='K' && !strcmp(l,"X"));
        assert(runtime_pad_prompt("options",1,l,sizeof l)=='K' && !strcmp(l,"F1"));
        assert(runtime_pad_prompt("up",0,l,sizeof l)=='K' && !strcmp(l,"\xe2\x86\x91"));
        assert(runtime_pad_prompt("touchpad_right",1,l,sizeof l)=='K' && !strcmp(l,"Bksp"));
        assert(runtime_pad_prompt("nothing",1,l,sizeof l)==0);
        inject(path,"cross");
        assert(pad_read_state(1,&data)==0 && runtime_pad_prompt_set()==pad_set);
        assert(runtime_pad_prompt_generation()!=gen);
        gen=runtime_pad_prompt_generation();
        assert(runtime_pad_prompt("cross",1,l,sizeof l)=='F' && !strcmp(l,pad_set==PROMPT_SET_NINTENDO ? "A" : "B"));
        assert(runtime_pad_prompt("r1",1,l,sizeof l)=='S' && !strcmp(l,pad_set==PROMPT_SET_NINTENDO ? "ZR" : "RT"));
        assert(runtime_pad_prompt("move_up",0,l,sizeof l)=='C' && !strcmp(l,"LS"));
        assert(runtime_pad_prompt("up",0,l,sizeof l)==0); /* the d-pad: the game's icon */
        assert(pad_read_state(1,&data)==0 && runtime_pad_prompt_generation()==gen); /* no change: no redraw */
        prompt_style_setting=PROMPT_STYLE_NINTENDO;
        inject(path,"");
        assert(pad_read_state(1,&data)==0 && runtime_pad_prompt_set()==PROMPT_SET_NINTENDO);
        assert(runtime_pad_prompt("cross",1,l,sizeof l)=='F' && !strcmp(l,"A")); /* pad.cross=b: East */
        assert(runtime_pad_prompt("circle",1,l,sizeof l)=='F' && !strcmp(l,"B")); /* pad.circle=a: South */
        prompt_style_setting=PROMPT_STYLE_PLAYSTATION;
        assert(pad_read_state(1,&data)==0 && runtime_pad_prompt_set()==PROMPT_SET_PLAYSTATION &&
               runtime_pad_prompt("cross",1,l,sizeof l)==0);
        prompt_style_setting=PROMPT_STYLE_KEYBOARD;
        inject(path,"cross");
        assert(pad_read_state(1,&data)==0 && runtime_pad_prompt_set()==PROMPT_SET_KEYBOARD);
        prompt_style_setting=PROMPT_STYLE_AUTO;
        assert(pad_read_state(1,&data)==0 && runtime_pad_prompt_set()==pad_set);
        /* A key rebound (the in-game menu writes the line, bumps the generation): drawn again. */
        const char rebound2[]="upscaler=fsr3\nkey.cross=Return\nmkkey.r1=Mouse Right\n";
        assert(bb_test_write_file(config,rebound2,sizeof(rebound2)-1)==0);
        ++bind_generation;
        gen=runtime_pad_prompt_generation();
        inject(path,"keyhold=Return");
        assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS) && runtime_pad_prompt_set()==PROMPT_SET_KEYBOARD);
        assert(runtime_pad_prompt_generation()!=gen);
        assert(runtime_pad_prompt("cross",1,l,sizeof l)=='K' && !strcmp(l,"\xe2\x86\xb5"));
        /* Mouse & keyboard mode: the mouse scheme's buttons, a bound mouse button, mouse look. */
        mk_mode=1;
        inject(path,"keyhold=E");
        assert(pad_read_state(1,&data)==0 && runtime_pad_mouse_look());
        assert(runtime_pad_prompt("r1",1,l,sizeof l)=='M' && !strcmp(l,"Right"));
        assert(runtime_pad_prompt("r2",1,l,sizeof l)=='M' && !strcmp(l,"\xe2\x87\xa7Left"));
        assert(runtime_pad_prompt("l2",1,l,sizeof l)==0); /* Mouse Right is R1 now, no L2 key */
        assert(runtime_pad_prompt("r3",1,l,sizeof l)=='M' && !strcmp(l,"Middle"));
        assert(runtime_pad_prompt("cross",1,l,sizeof l)=='K' && !strcmp(l,"E"));
        assert(runtime_pad_prompt("up",0,l,sizeof l)=='K' && !strcmp(l,"\xe2\x86\x91"));
        mk_mode=0;
        assert(bb_test_write_file(config,controls,sizeof(controls)-1)==0);
        ++bind_generation;
        inject(path,"");
        pad_read_state(1,&data);
        assert(!runtime_pad_mouse_look());
    }
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    unlink(config);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, controls, mouse & keyboard mode, mouse bindings and wheel");
}
