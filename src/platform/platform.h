#ifndef PLATFORM_H
#define PLATFORM_H

#include "core/types.h"

/*
 * Porting Guide: Input Mapping
 *
 * EarthBound uses 8 SNES action buttons, but most have redundant functions.
 * The game checks button *groups* for most actions (see PAD_CONFIRM, PAD_CANCEL,
 * etc. in pad.h). Ports with fewer buttons should prioritize mapping:
 *
 * REQUIRED (minimum playable):
 *   D-pad       — Movement and menu navigation
 *   PAD_A       — Confirm selection, open command window, interact
 *   PAD_B       — Cancel, back, show HP/PP window on overworld
 *
 * RECOMMENDED:
 *   PAD_X       — Toggle town map (unique function, no equivalent)
 *   PAD_START   — Start game from title screen (only use; could be combined)
 *   PAD_L       — "Check/talk to" shortcut on overworld (like A but tries talk first)
 *
 * OPTIONAL (cosmetic or redundant):
 *   PAD_SELECT  — Duplicate of B (for one-handed play)
 *   PAD_R       — Rings bicycle bell (cosmetic sound effect only)
 *   PAD_Y       — No function in normal gameplay
 *
 * For platforms with limited buttons, consider mapping unused physical buttons
 * to PAD_L (for check/talk) rather than PAD_Y or PAD_R.
 */

/* Global mode flags (set before init, read by platform backends) */
extern bool platform_headless;    /* --headless: no window, no frame timing */
extern bool platform_skip_intro;  /* --skip-intro: skip to overworld for debugging */
extern int platform_max_frames;   /* --frames N: quit after N frames (0=unlimited) */

/*
 * Video — scanline-based rendering
 *
 * The PPU renders one scanline at a time. Each frame follows this sequence:
 *   1. platform_video_begin_frame()       — prepare for a new frame
 *   2. platform_video_send_scanline(y, p) — called for y=0..height-1 with RGBA pixels
 *   3. platform_video_end_frame()         — present the completed frame
 *
 * platform_video_get_framebuffer() returns a pointer to the full frame pixel
 * buffer (for desktop backends that texture-upload), or NULL on embedded
 * targets that stream scanlines directly to the display.
 *
 * platform_render_frame() encapsulates the full begin/render/end sequence.
 * On single-core platforms it calls begin_frame, ppu_render_frame, end_frame.
 * Dual-core platforms override this to distribute scanline rendering across
 * cores (e.g., even/odd scanline interleaving with a ring buffer).
 * The fps_overlay_cb, if non-NULL, is called for each scanline to stamp
 * the FPS overlay text before output.
 */
typedef void (*scanline_stamp_cb_t)(int y, pixel_t *pixels);
bool platform_video_init(void);
void platform_video_shutdown(void);
void platform_video_begin_frame(void);
void platform_video_send_scanline(int y, const pixel_t *pixels);
pixel_t *platform_video_get_framebuffer(void);
void platform_video_end_frame(void);
void platform_video_set_vsync(bool enabled);
void platform_render_frame(scanline_stamp_cb_t fps_overlay_cb);

/*
 * Input
 *
 * Pad state is a uint16_t bitmask of SNES joypad buttons (see pad.h).
 * Each bit corresponds to one button — e.g. PAD_A (0x0080), PAD_B (0x8000).
 * Game logic tests buttons via groups like PAD_CONFIRM (A|L) and PAD_CANCEL
 * (B|SELECT), so ports can map physical buttons to whichever SNES buttons
 * make sense for the hardware.
 *
 * Aux state is a uint16_t bitmask of non-game buttons (debug, turbo, etc.).
 * Ports report raw level state (bit set = button currently held). Edge
 * detection and toggle logic are handled centrally in game_main.c, so
 * ports never need to debounce or track previous state for aux buttons.
 *
 * platform_input_poll() is called once per frame before any pad/aux reads.
 * It must sample hardware and update internal state so that get_pad(),
 * get_pad_new(), and get_aux() return consistent values for the frame.
 */

/* Aux button bitmask — non-game actions (debug, turbo, etc.) */
#define AUX_DEBUG_DUMP   (1 << 0)   /* F1: dump PPU state */
#define AUX_VRAM_DUMP    (1 << 1)   /* F2: dump VRAM as image */
#define AUX_FPS_TOGGLE   (1 << 2)   /* F3: toggle FPS overlay */
#define AUX_FAST_FORWARD (1 << 4)   /* Tab: toggle 4x speed */
#define AUX_DEBUG_TOGGLE (1 << 5)   /* `: toggle debug mode */
#define AUX_SAVESTATE    (1 << 6)   /* F6: request a torn-safe savestate capture */
#define AUX_LOAD_STATE   (1 << 7)   /* F7: restore the last savestate */

bool platform_input_init(void);
void platform_input_shutdown(void);
void platform_input_poll(void);                       /* sample hardware — call once per frame */
uint16_t platform_input_get_pad(void);                /* bitmask of currently held buttons (pad.h PAD_* flags) */
uint16_t platform_input_get_pad_new(void);            /* bitmask of buttons pressed this frame (not held last frame) */
uint16_t platform_input_get_aux(void);                /* bitmask of currently held aux buttons (AUX_* flags) */
bool platform_input_quit_requested(void);             /* user wants to exit (e.g. window close, Escape) */
void platform_request_quit(void);                     /* programmatic quit (e.g. from game menu) */

/*
 * Mouse (desktop only; EB_HAS_INPUT_SETTINGS): the pointer in picture pixels (0,0 = top-left of
 * the EB_VIEWPORT_WIDTH x EB_VIEWPORT_HEIGHT picture, gutters included) and this frame's edges.
 * `active` is false until the pointer has been over the window; ports without a mouse report a
 * mouse that never moves or clicks.
 */
typedef struct {
    int x, y;
    bool active;
    bool moved;            /* the pointer moved since the last poll */
    bool left_pressed;     /* a button went down since the last poll */
    bool right_pressed;
} PlatformMouse;
#ifdef EB_HAS_INPUT_SETTINGS
const PlatformMouse *platform_mouse(void);
void platform_mouse_script(int x, int y, bool left, bool right);   /* input scripts: put the pointer at x,y and click */
void platform_text_input_script(int c);                            /* input scripts: queue a typed character (13 = Enter) */
#else
static inline const PlatformMouse *platform_mouse(void) { static const PlatformMouse none; return &none; }
#endif

/*
 * Input settings (desktop only; defined when EB_HAS_INPUT_SETTINGS is set)
 *
 * Remappable keyboard bindings, gamepad choice and stick deadzone, saved by the
 * platform in its own config file. Slots are the 12 SNES buttons in menu order; names are the Xbox controller names
 * for the pad button each one is driven by (SNES A is the Xbox B button, and so on).
 */
#define INPUT_SLOT_COUNT        14
#define INPUT_DEADZONE_MIN      5     /* percent of full stick travel */
#define INPUT_DEADZONE_MAX      60
#define INPUT_DEADZONE_DEFAULT  25
int platform_input_slot_count(void);
const char *platform_input_slot_name(int slot);       /* Xbox controller names: "A", "LB", "Menu", ... */
const char *platform_input_slot_key_name(int slot);   /* bound key's name, "..." while waiting for a key */
void platform_input_rebind_begin(int slot);           /* next key press becomes this slot's binding (Escape cancels) */
bool platform_input_rebind_active(void);              /* true until the key was captured or cancelled */
int platform_input_gamepad_count(void);
const char *platform_input_gamepad_name(int index);
int platform_input_gamepad_selected(void);            /* index of the pad in use, or -1 */
void platform_input_gamepad_select(int index);
int platform_input_deadzone(void);                    /* percent */
void platform_input_set_deadzone(int percent);
void platform_input_reset_defaults(void);             /* default keys, deadzone and first gamepad */

/*
 * Typed text (desktop only; EB_HAS_INPUT_SETTINGS): lets the naming screen take the PC keyboard.
 * While enabled, letters and digits stop driving the pad bindings and are queued as characters.
 * platform_text_input_get() returns the next queued character (printable ASCII), 8 for Backspace,
 * 8 for Backspace, 13 for Enter/Return, 9 for Tab, or 0 when the queue is empty.
 */
void platform_text_input_enable(bool enable);
int platform_text_input_get(void);
void platform_text_input_request_osk(void);          /* ask the system for its on-screen keyboard (handhelds, consoles, touch) */

/*
 * Files and folders (desktop only; EB_HAS_FILE_BROWSER): the save backup / restore browser.
 */
typedef struct {
    char name[256];
    bool is_dir;
} PlatformDirEntry;
const char *platform_home_dir(void);
bool platform_dir_parent(char *path);                 /* cut path to its parent; false at the root */
int platform_dir_list(const char *dir, const char *ext, PlatformDirEntry *out, int max);
                                                      /* folders first, then files ending in ext (NULL: no files); -1 on error */
bool platform_file_exists(const char *path);
size_t platform_file_read(const char *path, void *dst, size_t max);
bool platform_file_write(const char *path, const void *data, size_t size);   /* via a temp file */

/* Display settings (same config file; EB_HAS_INPUT_SETTINGS) */
#define DISPLAY_ASPECT_4_3    0
#define DISPLAY_ASPECT_16_10  1
#define DISPLAY_ASPECT_16_9   2
#define DISPLAY_ASPECT_FIT    3   /* the picture follows the window's shape (between 4:3 and 9:5) */
int platform_display_aspect(void);                    /* DISPLAY_ASPECT_*; widescreen rendering reads this */
void platform_display_set_aspect(int aspect);
bool platform_display_fullscreen(void);
void platform_display_apply_aspect(void);             /* resize the picture to the current aspect ratio setting */
void platform_display_set_fullscreen(bool fullscreen); /* applies at once */

/*
 * Audio
 *
 * The audio subsystem runs the SPC700/DSP emulator on a separate thread
 * (desktop) or is disabled entirely (embedded without audio hardware).
 * platform_audio_lock/unlock() guard shared state between the audio
 * callback thread and the main game thread (e.g. when writing APU ports).
 */
bool platform_audio_init(void);
void platform_audio_shutdown(void);
void platform_audio_lock(void);                       /* acquire audio mutex (pairs with unlock) */
void platform_audio_unlock(void);
/* Desktop: the game loop runs the sound chip one frame per video frame into a ring buffer and trims the
 * frame time by up to half a percent to keep the buffer level, so audio cannot drift from video. */
#ifdef EB_HAS_INPUT_SETTINGS
void platform_audio_produce_frame(void);
uint64_t platform_audio_adjust_frame_period(uint64_t period);
#else
static inline void platform_audio_produce_frame(void) {}
static inline uint64_t platform_audio_adjust_frame_period(uint64_t period) { return period; }
#endif

/*
 * Save data — persistent storage
 *
 * The game calls platform_save_read/write with byte offsets and sizes
 * into a flat SAVE_FILE_SIZE byte buffer (the first 3 slots × 2 copies
 * × 1280 bytes fill the 8 KB SRAM image; slots 4-99 follow it). On desktop this is a file; on SNES it's SRAM; on
 * embedded it might be flash.
 *
 * platform_save_init() is called once at startup. Returns false if the
 * storage backend can't be initialized.
 *
 * platform_save_read() returns the number of bytes actually read (0 if
 * no save exists). platform_save_write() returns true on success.
 */
#define PLATFORM_SAVE_IMAGE_SIZE 0x2000   /* one SRAM image; with SAVE_COUNT > 3 each slot has its own */
/* The folder for the game's own files (saves, settings), created on first use. Linux/macOS:
 * ~/.earthbound; Windows: %LOCALAPPDATA%\earthbound. EARTHBOUND_HOME overrides it. No trailing slash. */
const char *platform_data_dir(void);

bool platform_save_init(void);
size_t platform_save_read(void *dst, size_t offset, size_t size);
bool platform_save_write(const void *src, size_t offset, size_t size);

/*
 * Savestate storage — large suspend/resume snapshots (build-order item #5).
 *
 * Distinct from platform_save_* (the 7680-byte battery SRAM). A savestate is the
 * full run-to-completion snapshot (~139 KiB) used for power-off suspend/resume.
 * Two ping-pong SLOTS provide crash-safety: a write targets the inactive slot and
 * is durable only after _commit(), so a power loss mid-write leaves the prior slot
 * intact. The embedded firmware power-off handler requests a capture
 * (host_request_capture) and holds the power rail until the root boundary's
 * state_dump_save_slots() — which calls _begin/_write/_commit — returns. On desktop
 * the slots are two files; on embedded they are two flash regions.
 *
 * Offset/slot-addressed, caller-owned buffers, no malloc (same contract as
 * platform_save_*):
 *   _begin(slot)              start a fresh, truncating write to `slot`
 *   _write(slot,off,src,size) write `size` bytes at `off` within the slot
 *   _commit(slot)             flush the slot durably (fsync / flash commit)
 *   _read(slot,off,dst,size)  random read; returns bytes actually read (0 = absent)
 */
#define SAVESTATE_SLOTS 2
bool   platform_savestate_begin(int slot);
bool   platform_savestate_write(int slot, size_t offset, const void *src, size_t size);
bool   platform_savestate_commit(int slot);
size_t platform_savestate_read(int slot, size_t offset, void *dst, size_t size);

/*
 * Suspend/resume audio *output* around the root-boundary save/load I/O (the
 * ~0.6-1 s of blocking slot reads/writes during which the game loop, and thus
 * the port's audio producer, is stalled). A port whose audio is pulled by a
 * DMA/callback that keeps running while the loop is blocked (e.g. the G&W SAI
 * ring) will otherwise drain its buffer and then repeat/underrun for the rest
 * of the stall. freeze(true) is called once immediately before the blocking
 * state_dump, freeze(false) once after it returns (both branches). It is only
 * ever called at the root boundary, where the producer is quiescent, so a port
 * may safely gate its output callback here without deadlocking the producer.
 * Optional: the default (weak) implementation is a no-op for ports that don't
 * need it. Must be balanced and reentrancy-free (never nested).
 */
void platform_savestate_freeze_audio(bool freeze);

/*
 * Transient scratch RAM for the savestate (de)compressor — the LZ window, the
 * tamp working struct, and the compressed-byte I/O staging buffer. The payload
 * is stored as a tamp stream (see state_dump.c); compress (save) and decompress
 * (load) both run only at the root boundary, where the port can lend a large
 * already-allocated buffer it isn't using (the G&W lends its idle third
 * framebuffer; desktop returns a static buffer). Returns the buffer and writes
 * its byte size to *out_bytes. The caller treats it as volatile scratch and owns
 * no state across calls. Must hold the 256-byte LZ window, the tamp struct, a
 * 256-byte discard buffer and at least 512 bytes of I/O staging (about 1.3 KB in
 * all; see SS_* in state_dump.c); a bigger region just enables chunkier staging. Never NULL for a backend that can store a slot; the no-op
 * stub backend (src/platform/savestate_backend_stub.c), whose slots are always
 * absent, returns NULL with *out_bytes = 0.
 */
void  *platform_savestate_scratch(size_t *out_bytes);

/* Debug dumps (desktop: write files to debug/; embedded: no-op) */
void platform_debug_dump_ppu(const pixel_t *framebuffer);
void platform_debug_dump_vram_image(void);

/*
 * Timer — frame pacing
 *
 * Call platform_timer_frame_start() at the top of the game loop and
 * platform_timer_frame_end() at the bottom. frame_end() sleeps or
 * busy-waits to maintain ~60 fps (NTSC timing).
 *
 * platform_timer_ticks() / ticks_per_sec() provide a monotonic clock
 * for measuring elapsed time. get_fps_tenths() returns a smoothed FPS
 * value multiplied by 10 (e.g. 600 = 60.0 fps) for the FPS overlay.
 */
bool platform_timer_init(void);
void platform_timer_shutdown(void);
void platform_timer_frame_start(void);
void platform_timer_frame_end(void);
void platform_timer_update_fps(void);               /* update IIR FPS filter (call every frame) */
void platform_timer_sleep_until(uint64_t deadline);  /* sleep until the given tick value */
uint64_t platform_timer_ticks(void);
uint64_t platform_timer_ticks_per_sec(void);
uint32_t platform_timer_get_fps_tenths(void);

/*
 * Optional host-paced frame-skip hook.
 *
 * When EB_HOST_PACED_FRAMESKIP is defined, host_process_frame() delegates
 * the per-frame render/skip decision to the platform instead of using its own
 * deadline-based dynamic frame-skip. Platforms that pace the game loop against
 * an external clock implement this — e.g. the Game & Watch port locks the loop
 * to its audio DMA: platform_timer_should_render() runs the shared retro-go
 * frame-loop controller (which records this frame's skip state so the matching
 * platform_timer_sleep_until() can rate-match and phase-lock audio), and
 * returns whether this frame should be rendered. Called once per frame, at the
 * top of host_process_frame(), in place of the built-in skip logic.
 */
#ifdef EB_HOST_PACED_FRAMESKIP
bool platform_timer_should_render(void);
#endif

#endif /* PLATFORM_H */
