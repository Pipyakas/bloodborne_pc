/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled.
 * The context is the handler's ucontext_t (Linux) or the vectored exception handler's
 * EXCEPTION_POINTERS (Windows). */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault
 * (same context as bbgpu_handle_fault). */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* Control channel (src/runtime_control.c): completes an open text entry as if typed and
 * confirmed (0 when none is open); 1 while one is open. */
int bbgpu_text_input_submit(const char *text_utf8);
int bbgpu_text_input_active(void);
/* bb-probe --first-run <result file>: the first-launch screen (gpu/shim/bbport_first_run.cpp)
 * that asks for the game folder or installs the game's .pkg files; 0 with the folder written
 * to result_path, 1 when the user quit. */
int bbgpu_first_run(const char *result_path);
/* Frames presented since start. */
uint64_t bbgpu_present_count(void);
/* Writes the next presented frame (the game's picture with its HUD, without the settings menu)
 * to a PNG, scaled down to at most max_width pixels wide (0: full size). 0 on success, -1 when
 * no frame was presented within timeout_ms or the display format is not supported. */
int bbgpu_capture_png(const char *path, int max_width, int timeout_ms, int *width, int *height);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* Launch shortcut for this start (bbport.ini "launch", BB_LAUNCH): BbSettings::Launch,
 * 0 title screen, 1 Play Offline menu, 2 continue, 3 load game, 4 new game, 5 system. */
int bbgpu_launch_destination(void);
/* 1 while the keyboard plays the game ("keyboard_controls"). */
int bbgpu_keyboard_controls(void);
/* 1 while audio output should be silent: "mute", or "mute_background" while unfocused. */
int bbgpu_audio_muted(void);
/* 1 while the effect switch `key` (bbport.ini effect_*) is on. */
int bbgpu_effect_enabled(const char *key);
/* runtime_effects.c: the game now runs with effect `key` on (1) or off (0), so changing it
 * needs no restart; -1: it cannot be switched while running (applies after a restart). */
void bbgpu_effect_live(const char *key, int state);
/* The port's settings as rows of the game's options screens (runtime_menu.c). The game's
 * widgets edit *value, an int32 (on/off 1/0, a choice index, a slider 0..10; choices are
 * written as int32, the others as its low byte); the GPU library applies and saves changes.
 * Strings are UTF-16, static. A screen shows five rows (the movie's row sprites). */
enum { BB_NATIVE_TOGGLE = 0, BB_NATIVE_CHOICE = 1, BB_NATIVE_SLIDER = 2 };
typedef struct {
    const uint16_t *label, *help;
    int32_t kind, choice_count;
    const uint16_t *const *choices;
    int32_t *value;
    const int32_t *default_value;
} BbNativeSetting;
/* The Graphics list (options screen) opens three screens; BB_NATIVE_GRAPHICS is the list
 * itself (bbgpu_native_screen_text only). */
enum { BB_NATIVE_SCREEN = 0, BB_NATIVE_ADVANCED = 1, BB_NATIVE_UPSCALING = 2, BB_NATIVE_SCREENS = 3,
       BB_NATIVE_GRAPHICS = 3 };
/* Screen `screen`'s rows, their values read from the settings (call when it opens; choices
 * still pending from a previous opening are applied first). */
int bbgpu_native_settings(int32_t screen, const BbNativeSetting **rows);
/* Screen titles and the options-list rows that open them (label, one-line help). */
const uint16_t *bbgpu_native_screen_text(int32_t screen, int32_t which);
/* The screen closed: applies the choice rows (on/off and sliders apply at once). */
void bbgpu_native_settings_commit(void);
/* A choice row's "dropdown open" byte in the game's row widget: its choice applies as soon
 * as the dropdown closes (it reads 0); without one, when the screen closes. */
void bbgpu_native_settings_dropdown(const BbNativeSetting *row, const volatile uint8_t *open);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
