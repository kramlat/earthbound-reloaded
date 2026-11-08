#include "platform/platform.h"
#include "sdl2_touch.h"
#include "include/pad.h"
#include <SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;

/* ---- Input settings: keyboard bindings, gamepad choice, stick deadzone ----
 * Persisted in $XDG_CONFIG_HOME/earthbound/input.cfg (default ~/.config/...). */
static const struct { uint16_t bit; const char *name; const char *cfg; SDL_Scancode def; } slots[INPUT_SLOT_COUNT] = {
    { PAD_UP,     "D-Pad Up",    "up",     SDL_SCANCODE_UP     },
    { PAD_DOWN,   "D-Pad Down",  "down",   SDL_SCANCODE_DOWN   },
    { PAD_LEFT,   "D-Pad Left",  "left",   SDL_SCANCODE_LEFT   },
    { PAD_RIGHT,  "D-Pad Right", "right",  SDL_SCANCODE_RIGHT  },
    { PAD_A,      "B",      "a",      SDL_SCANCODE_X      },
    { PAD_B,      "A",      "b",      SDL_SCANCODE_Z      },
    { PAD_X,      "Y",      "x",      SDL_SCANCODE_S      },
    { PAD_Y,      "X",      "y",      SDL_SCANCODE_A      },
    { PAD_L,      "LB",     "l",      SDL_SCANCODE_Q      },
    { PAD_R,      "RB",     "r",      SDL_SCANCODE_W      },
    { PAD_START,  "Menu",   "start",  SDL_SCANCODE_RETURN },
    { PAD_SELECT, "View",   "select", SDL_SCANCODE_RSHIFT },
    { PAD_LT,     "LT",     "lt",     SDL_SCANCODE_E      },   /* the two triggers come last: slot numbers are */
    { PAD_RT,     "RT",     "rt",     SDL_SCANCODE_R      },   /* what the settings pages refer to */
};
static SDL_Scancode key_bind[INPUT_SLOT_COUNT];
static int deadzone_pct = INPUT_DEADZONE_DEFAULT;
static char wanted_pad_name[128];       /* "" = first available */
static SDL_GameController *pad_dev;
static int display_aspect = DISPLAY_ASPECT_4_3;
static bool display_fullscreen;
static bool settings_loaded;
void platform_video_set_fullscreen(bool fullscreen);   /* sdl2_video.c */
static bool text_mode;                  /* typed-text capture on (naming screen) */
static uint8_t text_queue[32];
static unsigned text_head, text_tail;
/* Key repeat for typed text, done here so it behaves the same on every desktop: SDL's own repeat is
 * ignored, a held key repeats after a delay and then at a steady rate until it is released. */
#define TEXT_REPEAT_DELAY_MS 450
#define TEXT_REPEAT_RATE_MS  40
static int text_repeat_char;            /* character (or 8 = Backspace) being repeated, 0 = none */
static SDL_Scancode text_repeat_key;    /* the key being held */
static SDL_Scancode text_last_down;
static bool text_swallow_next;          /* the next SDL_TEXTINPUT came from SDL's repeat of a held key */
static uint32_t text_repeat_next;
static int rebind_slot = -1;            /* slot waiting for a key press, or -1 */
static uint16_t key_held;               /* pad bits held through the keyboard */
static uint16_t ctl_held;               /* pad bits held through the gamepad */

static void config_path(char *out, size_t n) {
    snprintf(out, n, "%s/input.cfg", platform_data_dir());
}

/* Settings used to live in ~/.config/earthbound/input.cfg: carry them over the first time. */
static void migrate_old_config(const char *new_path) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char old_path[512];
    if (xdg && *xdg) snprintf(old_path, sizeof(old_path), "%s/earthbound/input.cfg", xdg);
    else if (home && *home) snprintf(old_path, sizeof(old_path), "%s/.config/earthbound/input.cfg", home);
    else return;
    FILE *in = fopen(old_path, "r");
    if (!in) return;
    FILE *out = fopen(new_path, "w");
    if (out) {
        char buf[512];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, got, out);
        fclose(out);
    }
    fclose(in);
}

static void settings_defaults(void) {
    for (int i = 0; i < INPUT_SLOT_COUNT; i++) key_bind[i] = slots[i].def;
    deadzone_pct = INPUT_DEADZONE_DEFAULT;
    wanted_pad_name[0] = '\0';
}

static void settings_ensure_loaded(void);

static void settings_save(void) {
    char path[512];
    config_path(path, sizeof(path));                  /* the folder exists: platform_data_dir() made it */
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < INPUT_SLOT_COUNT; i++)
        fprintf(f, "key.%s=%d\n", slots[i].cfg, (int)key_bind[i]);
    fprintf(f, "deadzone=%d\n", deadzone_pct);
    fprintf(f, "gamepad=%s\n", wanted_pad_name);
    fprintf(f, "aspect=%d\n", display_aspect);
    fprintf(f, "fullscreen=%d\n", display_fullscreen ? 1 : 0);
    fclose(f);
}

static void settings_load(void) {
    settings_loaded = true;
    settings_defaults();
    display_aspect = DISPLAY_ASPECT_4_3;
    display_fullscreen = false;
    char path[512];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        migrate_old_config(path);
        f = fopen(path, "r");
    }
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        if (!strncmp(line, "key.", 4)) {
            for (int i = 0; i < INPUT_SLOT_COUNT; i++)
                if (!strcmp(line + 4, slots[i].cfg)) {
                    int v = atoi(eq);
                    if (v > 0 && v < SDL_NUM_SCANCODES) key_bind[i] = (SDL_Scancode)v;
                }
        } else if (!strcmp(line, "deadzone")) {
            int v = atoi(eq);
            if (v >= INPUT_DEADZONE_MIN && v <= INPUT_DEADZONE_MAX) deadzone_pct = v;
        } else if (!strcmp(line, "aspect")) {
            int v = atoi(eq);
            if (v >= DISPLAY_ASPECT_4_3 && v <= DISPLAY_ASPECT_FIT) display_aspect = v;
        } else if (!strcmp(line, "fullscreen")) {
            display_fullscreen = atoi(eq) != 0;
        } else if (!strcmp(line, "gamepad")) {
            snprintf(wanted_pad_name, sizeof(wanted_pad_name), "%s", eq);
        }
    }
    fclose(f);
}

static void settings_ensure_loaded(void) {
    if (!settings_loaded) settings_load();
}

/* Index into the controllers SDL currently reports (joysticks that are game controllers). */
static int controller_count(void) {
    int n = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) n++;
    return n;
}

static int controller_device_index(int nth) {
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && nth-- == 0) return i;
    return -1;
}

static void open_wanted_gamepad(void) {
    if (pad_dev) { SDL_GameControllerClose(pad_dev); pad_dev = NULL; }
    ctl_held = 0;
    int n = controller_count();
    for (int i = 0; i < n; i++) {
        const char *name = SDL_GameControllerNameForIndex(controller_device_index(i));
        if (wanted_pad_name[0] && (!name || strcmp(name, wanted_pad_name))) continue;
        pad_dev = SDL_GameControllerOpen(controller_device_index(i));
        if (pad_dev) return;
    }
    /* The remembered pad isn't plugged in: fall back to the first one. */
    if (wanted_pad_name[0]) {
        for (int i = 0; i < n && !pad_dev; i++)
            pad_dev = SDL_GameControllerOpen(controller_device_index(i));
    } else if (n > 0) {
        pad_dev = SDL_GameControllerOpen(controller_device_index(0));
    }
}

/* Sample the open controller: buttons use SNES positions (SDL's south/east face
 * buttons are the SNES B/A), the d-pad and left stick both drive the d-pad. */
static uint16_t gamepad_sample(void) {
    if (!pad_dev) return 0;
    static const struct { SDL_GameControllerButton b; uint16_t bit; } map[] = {
        { SDL_CONTROLLER_BUTTON_A, PAD_B },        { SDL_CONTROLLER_BUTTON_B, PAD_A },
        { SDL_CONTROLLER_BUTTON_X, PAD_Y },        { SDL_CONTROLLER_BUTTON_Y, PAD_X },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_L }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_R },
        { SDL_CONTROLLER_BUTTON_START, PAD_START },    { SDL_CONTROLLER_BUTTON_BACK, PAD_SELECT },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP },     { SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT },
    };
    uint16_t bits = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (SDL_GameControllerGetButton(pad_dev, map[i].b)) bits |= map[i].bit;
    if (SDL_GameControllerGetAxis(pad_dev, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)  bits |= PAD_LT;
    if (SDL_GameControllerGetAxis(pad_dev, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) bits |= PAD_RT;
    int dz = deadzone_pct * 32767 / 100;
    int lx = SDL_GameControllerGetAxis(pad_dev, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(pad_dev, SDL_CONTROLLER_AXIS_LEFTY);
    if (lx < -dz) bits |= PAD_LEFT;
    if (lx >  dz) bits |= PAD_RIGHT;
    if (ly < -dz) bits |= PAD_UP;
    if (ly >  dz) bits |= PAD_DOWN;
    return bits;
}
static volatile sig_atomic_t quit_requested;

/* Scripted playback (sdl2_input_script.c) */
bool input_script_active(void);
uint16_t input_script_next(uint16_t *aux);
/* Headless recording (sdl2_record.c) */
void record_frame(void);

/* Global mode flags */
bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static void signal_handler(int sig) {
    (void)sig;
    quit_requested = 1;
}

bool platform_input_init(void) {
    pad_state = 0;
    pad_prev = 0;
    aux_state = 0;
    quit_requested = 0;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    touch_init();
    settings_load();
    open_wanted_gamepad();
    key_held = ctl_held = 0;
    rebind_slot = -1;
    return true;
}

void platform_input_shutdown(void) {
    if (pad_dev) { SDL_GameControllerClose(pad_dev); pad_dev = NULL; }
}

void platform_text_input_script(int c) {
    if (text_tail - text_head < sizeof(text_queue))
        text_queue[text_tail++ % sizeof(text_queue)] = (uint8_t)c;
}

static PlatformMouse mouse;
const PlatformMouse *platform_mouse(void) { return &mouse; }
void platform_mouse_script(int x, int y, bool left, bool right) {
    mouse.active = true;
    if (x != mouse.x || y != mouse.y) mouse.moved = true;
    mouse.x = x;
    mouse.y = y;
    mouse.left_pressed |= left;
    mouse.right_pressed |= right;
}

void platform_input_poll(void) {
    SDL_Event event;

    mouse.moved = mouse.left_pressed = mouse.right_pressed = false;

    record_frame(); /* --record-video/--record-audio: this frame is rendered */

    pad_prev = pad_state;

    while (SDL_PollEvent(&event)) {
        if (touch_handle_event(&event))
            continue;
        switch (event.type) {
        case SDL_QUIT:
            quit_requested = 1;
            break;
        case SDL_TEXTINPUT:
            if (text_mode) {
                if (text_swallow_next) { text_swallow_next = false; break; }
                for (const char *c = event.text.text; *c; c++)
                    if ((unsigned char)*c >= 32 && (unsigned char)*c <= 126 && text_tail - text_head < sizeof(text_queue)) {
                        text_queue[text_tail++ % sizeof(text_queue)] = (uint8_t)*c;
                        text_repeat_char = (unsigned char)*c;
                        text_repeat_key = text_last_down;
                        text_repeat_next = SDL_GetTicks() + TEXT_REPEAT_DELAY_MS;
                    }
            }
            break;
        case SDL_MOUSEMOTION:
            touch_not_in_use();
            mouse.active = true;
            mouse.moved = true;
            mouse.x = event.motion.x;      /* picture pixels: the renderer maps window to logical size */
            mouse.y = event.motion.y;
            break;
        case SDL_MOUSEBUTTONDOWN:
            touch_not_in_use();
            mouse.active = true;
            mouse.x = event.button.x;
            mouse.y = event.button.y;
            if (event.button.button == SDL_BUTTON_LEFT) mouse.left_pressed = true;
            else if (event.button.button == SDL_BUTTON_RIGHT) mouse.right_pressed = true;
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                platform_display_apply_aspect();       /* "fit window" follows the window as it is resized */
            break;
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            open_wanted_gamepad();
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            SDL_Scancode sc = event.key.keysym.scancode;
            if (event.type == SDL_KEYDOWN) touch_not_in_use();
            if (rebind_slot >= 0) {
                /* Capture mode: the next key press becomes the binding (Escape cancels). */
                if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                    if (sc != SDL_SCANCODE_ESCAPE) {
                        for (int i = 0; i < INPUT_SLOT_COUNT; i++)   /* a key drives one button: swap */
                            if (i != rebind_slot && key_bind[i] == sc) key_bind[i] = key_bind[rebind_slot];
                        key_bind[rebind_slot] = sc;
                        settings_save();
                    }
                    rebind_slot = -1;
                    key_held = 0;
                }
                break;
            }
            if (text_mode) {
                if (event.type == SDL_KEYUP && sc == text_repeat_key) text_repeat_char = 0;
                if (event.type == SDL_KEYDOWN) {
                    text_last_down = sc;
                    text_swallow_next = event.key.repeat != 0;     /* a repeat of a held key: ours replaces it */
                }
                /* Typing a name: Backspace deletes, Enter and Tab are queued, everything else is ignored. */
                if (event.type == SDL_KEYDOWN && !event.key.repeat && text_tail - text_head < sizeof(text_queue) &&
                    (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER))
                    text_queue[text_tail++ % sizeof(text_queue)] = 13;     /* Enter ends the typing */
                if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER)
                    break;                                                  /* ...and is not also a button press */
                if (sc == SDL_SCANCODE_TAB) {                               /* Tab = Don't Care (not fast-forward here) */
                    if (event.type == SDL_KEYDOWN && !event.key.repeat && text_tail - text_head < sizeof(text_queue))
                        text_queue[text_tail++ % sizeof(text_queue)] = 9;
                    break;
                }
                if (event.type == SDL_KEYDOWN && !event.key.repeat && sc == SDL_SCANCODE_BACKSPACE &&
                    text_tail - text_head < sizeof(text_queue)) {
                    text_queue[text_tail++ % sizeof(text_queue)] = 8;
                    text_repeat_char = 8;
                    text_repeat_key = sc;
                    text_repeat_next = SDL_GetTicks() + TEXT_REPEAT_DELAY_MS;
                }
                /* While typing the keyboard is for text only: no button bindings and no function keys
                 * (fast-forward, savestates, debug dumps). The gamepad is unaffected. */
                break;
            }
            uint16_t bit = 0;
            uint16_t aux_bit = 0;
            for (int i = 0; i < INPUT_SLOT_COUNT; i++)
                if (key_bind[i] == sc) bit |= slots[i].bit;
            switch (sc) {
            case SDL_SCANCODE_F1:     aux_bit = AUX_DEBUG_DUMP;   break;
            case SDL_SCANCODE_F2:     aux_bit = AUX_VRAM_DUMP;    break;
            case SDL_SCANCODE_F3:     aux_bit = AUX_FPS_TOGGLE;   break;
            case SDL_SCANCODE_F6:     aux_bit = AUX_SAVESTATE;    break;
            case SDL_SCANCODE_F7:     aux_bit = AUX_LOAD_STATE;   break;
            case SDL_SCANCODE_TAB:    aux_bit = AUX_FAST_FORWARD; break;
            case SDL_SCANCODE_F5:     aux_bit = AUX_DEBUG_TOGGLE; break;
            default: break;
            }
            if (event.type == SDL_KEYDOWN) {
                key_held |= bit;
                aux_state |= aux_bit;
            } else {
                key_held &= ~bit;
                aux_state &= ~aux_bit;
            }
            break;
        }
        }
    }

    if (text_mode && text_repeat_char) {                 /* a held key: repeat after the delay */
        uint32_t now = SDL_GetTicks();
        if ((int32_t)(now - text_repeat_next) >= 0) {
            if (text_tail - text_head < sizeof(text_queue))
                text_queue[text_tail++ % sizeof(text_queue)] = (uint8_t)text_repeat_char;
            text_repeat_next = now + TEXT_REPEAT_RATE_MS;
        }
    }

    ctl_held = gamepad_sample();
    if (ctl_held) touch_not_in_use();
    pad_state = key_held | ctl_held | touch_pad();

    if (input_script_active()) {
        uint16_t aux = 0;
        pad_state = input_script_next(&aux) | touch_pad();
        aux_state = aux;
    }
}

uint16_t platform_input_get_pad(void) {
    return pad_state;
}

uint16_t platform_input_get_pad_new(void) {
    return pad_state & ~pad_prev;
}

uint16_t platform_input_get_aux(void) {
    return aux_state;
}

bool platform_input_quit_requested(void) {
    return quit_requested != 0;
}

void platform_request_quit(void) {
    quit_requested = 1;
}

/* ---- Input settings API (see platform.h) ---- */
int platform_input_slot_count(void) { return INPUT_SLOT_COUNT; }
const char *platform_input_slot_name(int slot) { return slots[slot].name; }
const char *platform_input_slot_key_name(int slot) {
    if (rebind_slot == slot) return "...";
    const char *n = SDL_GetScancodeName(key_bind[slot]);
    return (n && *n) ? n : "None";
}
void platform_input_rebind_begin(int slot) { rebind_slot = slot; key_held = 0; }
bool platform_input_rebind_active(void) { return rebind_slot >= 0; }

int platform_input_gamepad_count(void) { return controller_count(); }
const char *platform_input_gamepad_name(int i) {
    int d = controller_device_index(i);
    const char *n = d >= 0 ? SDL_GameControllerNameForIndex(d) : NULL;
    return n ? n : "Unknown";
}
int platform_input_gamepad_selected(void) {
    if (!pad_dev) return -1;
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad_dev));
    for (int i = 0; i < controller_count(); i++)
        if (SDL_JoystickGetDeviceInstanceID(controller_device_index(i)) == id) return i;
    return -1;
}
void platform_input_gamepad_select(int i) {
    int d = controller_device_index(i);
    if (d < 0) return;
    const char *n = SDL_GameControllerNameForIndex(d);
    snprintf(wanted_pad_name, sizeof(wanted_pad_name), "%s", n ? n : "");
    open_wanted_gamepad();
    settings_save();
}
int platform_input_deadzone(void) { return deadzone_pct; }
void platform_input_set_deadzone(int pct) {
    if (pct < INPUT_DEADZONE_MIN) pct = INPUT_DEADZONE_MIN;
    if (pct > INPUT_DEADZONE_MAX) pct = INPUT_DEADZONE_MAX;
    deadzone_pct = pct;
    settings_save();
}
void platform_input_reset_defaults(void) {
    settings_defaults();
    open_wanted_gamepad();
    settings_save();
}

/* ---- Display settings API ---- */
int platform_display_aspect(void) { settings_ensure_loaded(); return display_aspect; }
void platform_display_set_aspect(int aspect) {
    settings_ensure_loaded();
    if (aspect < DISPLAY_ASPECT_4_3 || aspect > DISPLAY_ASPECT_FIT) return;
    display_aspect = aspect;
    settings_save();
    platform_display_apply_aspect();
}
bool platform_display_fullscreen(void) { settings_ensure_loaded(); return display_fullscreen; }
void platform_display_set_fullscreen(bool fullscreen) {
    settings_ensure_loaded();
    display_fullscreen = fullscreen;
    platform_video_set_fullscreen(fullscreen);
    settings_save();
}

/* ---- Typed text API ---- */
void platform_text_input_enable(bool enable) {
    if (enable == text_mode) return;
    text_mode = enable;
    text_head = text_tail = 0;
    text_repeat_char = 0;
    text_swallow_next = false;
    key_held = 0;                      /* a key held when the mode switched must not stick... */
    aux_state = 0;                     /* ...and its release is ignored while typing, so clear the function keys too */
    if (enable) SDL_StartTextInput(); else SDL_StopTextInput();
}

int platform_text_input_get(void) {
    if (text_head == text_tail) return 0;
    return text_queue[text_head++ % sizeof(text_queue)];
}

void platform_text_input_request_osk(void) {
    if (!text_mode) return;
    SDL_StopTextInput();               /* restarting text input is what makes a system keyboard appear */
    SDL_StartTextInput();
}
