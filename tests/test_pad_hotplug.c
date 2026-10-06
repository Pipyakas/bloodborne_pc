/* Standalone regression test: no game window or physical controller required. */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"

/* Avoid the Windows CRT's modal assertion dialog in unattended tests. */
#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr,"FAIL %s:%d: %s (SDL: %s)\n",__FILE__,__LINE__,#condition,SDL_GetError()); \
    exit(1); \
} } while (0)

int bbgpu_overlay_captures_input(void) { return 0; }
int bbgpu_keyboard_controls(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static SDL_JoystickID attach(void) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.name="bbport hotplug test controller";
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id);
    return id;
}

int main(void) {
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "BB_HIDDEN", "0", true);
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "BB_MINIMIZED", "0", true);
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x1d50/0x6189");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Sampling must refresh discovery/state independently of the window pump. */
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==PAD_HANDLE);
    PadData data;
    assert(pad_read_state(PAD_HANDLE,&data)==0 && !gamepad && data.buttons==0);
    assert(data.connected && data.left_x==128);

    for (int connection=0;connection<2;++connection) {
        SDL_JoystickID id=attach();
        SDL_Joystick *joystick=SDL_OpenJoystick(id);
        assert(joystick);
        /* Open the newly attached pad through the existing guest handle. */
        assert(pad_read_state(PAD_HANDLE,&data)==0 && gamepad);
        assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
        assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,32767));
        assert(pad_read_state(PAD_HANDLE,&data)==0);
        assert((data.buttons & BTN_CROSS) && data.left_x==255);
        assert(data.connected_count==connected_count);

        SDL_CloseJoystick(joystick);
        assert(SDL_DetachVirtualJoystick(id));
        assert(pad_read_state(PAD_HANDLE,&data)==0 && !gamepad);
        assert(data.connected && data.buttons==0 && data.left_x==128);
    }
    assert(pad_close(PAD_HANDLE)==0);
    SDL_Quit();
    puts("PASS: late controller connection, live buttons/sticks, disconnect and reconnect");
    return 0;
}
