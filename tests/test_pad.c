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
void bbgpu_reflex_pre_input_sample(void) {}
void bbgpu_reflex_on_input_sampled(void) {}
/* Mouse & keyboard mode (after Mrsuss60/bloodborne_pc_windows_port's test stubs). */
static int mk_mode;
static float mk_dx;
static uint32_t mk_buttons;
static int mk_wheel_down;
int bbgpu_mouse_keyboard(BbMouseState *state, int consume_motion) {
    memset(state,0,sizeof(*state));
    state->dx=mk_dx; state->buttons=mk_buttons;
    state->sens_x=state->sens_y=1.0f; state->deadzone=0.05f; state->smoothing=0.0f;
    state->wheel=1;
    if (consume_motion) { mk_dx=0.0f; state->wheel_down=mk_wheel_down; mk_wheel_down=0; }
    return mk_mode;
}
int bbgpu_test_key(const char *name) { (void)name; return 1; }
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

/* Keyboard sticks (after Supermedo/bloodborne_pc a147eb8, 576514c): last-wins opposite keys and
 * a round gate for diagonals. Classic layout: W/A/S/D move. */
static void test_keyboard_sticks(void) {
    static bool k[SDL_SCANCODE_COUNT];
    PadData d;
    memset(&d,0,sizeof(d)); d.left_x=d.left_y=d.right_x=d.right_y=128;
    k[SDL_SCANCODE_A]=true; apply_keyboard(&d,k); assert(d.left_x==0);
    k[SDL_SCANCODE_D]=true; apply_keyboard(&d,k); assert(d.left_x==255); /* D pressed last */
    k[SDL_SCANCODE_A]=false; apply_keyboard(&d,k); k[SDL_SCANCODE_A]=true; apply_keyboard(&d,k);
    assert(d.left_x==0); /* A pressed again last */
    k[SDL_SCANCODE_A]=k[SDL_SCANCODE_D]=false;
    memset(&d,0,sizeof(d)); d.left_x=d.left_y=d.right_x=d.right_y=128;
    k[SDL_SCANCODE_W]=k[SDL_SCANCODE_D]=true; apply_keyboard(&d,k);
    assert(d.left_x==218 && d.left_y==38); /* full deflection on the circle, not the corner */
    k[SDL_SCANCODE_W]=k[SDL_SCANCODE_D]=false;
}

/* Xbox pads: Back + d-pad left/right clicks the touchpad halves (after Supermedo/bloodborne_pc
 * a04e1e95, parsifal295's PR #39); Back alone clicks the left half for 150 ms on release. */
static void test_back_combo(void) {
    uint64_t t=1000000;
    assert(back_combo_step(0,0,0,t)==-1);
    assert(back_combo_step(1,0,0,t+=10000)==-1);       /* Back held alone: nothing yet */
    assert(back_combo_step(0,0,0,t+=10000)==0);        /* released: a left-half click */
    assert(back_combo_step(0,0,0,t+=100000)==0);       /* ... for 150 ms */
    assert(back_combo_step(0,0,0,t+=60000)==-1);
    assert(back_combo_step(1,0,0,t+=10000)==-1);
    assert(back_combo_step(1,0,1,t+=10000)==1);        /* Back + right: the right half */
    assert(back_combo_step(1,0,1,t+=10000)==1);        /* while the d-pad is held */
    assert(back_combo_step(1,0,0,t+=10000)==-1);
    assert(back_combo_step(0,0,0,t+=10000)==-1);       /* no left click after a combo */
    assert(back_combo_step(1,1,0,t+=10000)==0);        /* Back + left: the left half */
    assert(back_combo_step(0,0,0,t+=10000)==-1);
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
    mk_mode=0; mk_buttons=0;
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_R1) && data.right_x==128 &&
           bindings[IN_CROSS].keys[0]==SDL_SCANCODE_X);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    unlink(config);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, controls, mouse & keyboard mode, mouse bindings and wheel");
}
