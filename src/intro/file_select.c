#include "intro/file_select.h"
#include "game/window.h"
#include "game/ng_plus.h"
#include "game/text.h"
#include "game/game_state.h"
#include "game/display_text.h"
#include "game/audio.h"
#include "game/fade.h"
#include "game/battle_bg.h"
#include "entity/entity.h"
#include "entity/sprite.h"
#include "data/event_script_data.h"
#include "include/binary.h"
#include "snes/ppu.h"
#include "core/math.h"
#include "core/memory.h"
#include "core/embedded.h"
#include "data/assets.h"
#include "platform/platform.h"
#include "include/pad.h"
#include "game/overworld.h"
#include "game/inventory.h"
#include "game/battle.h"
#include "include/constants.h"
#include <string.h>
#include <stdio.h>
#include "game_main.h"
#include "data/text_refs.h"
#include "core/mode_stack.h"

/* Whether each save slot has data */
static uint8_t save_files_present[SAVE_COUNT];



/* current_save_slot is now a global in game_state.c (port of CURRENT_SAVE_SLOT BSS) */

/* Don't Care names — loaded from ROM via asset pipeline.
 * 49 entries (7 categories × 7 names), 6 bytes each, EB-encoded and null-padded. */
#define DONT_CARE_COUNT 7
#define DONT_CARE_NAME_SIZE 6
static const uint8_t *dont_care_names_data;

/* HP_METER_SPEEDS — loaded from ROM via asset pipeline.
 * 3 entries × 4 bytes (32-bit LE fixed-point rolling speeds). */
static const uint8_t *hp_meter_speeds_data;

/* FILE_SELECT_TEXT_PLEASE_NAME_THEM_STRINGS — loaded from ROM via asset pipeline.
 * 7 prompts × NAME_THEM_STRING_LENGTH bytes, EB-encoded and null-padded. */
#define NAMING_PROMPT_ENTRY_SIZE 40  /* NAME_THEM_STRING_LENGTH (US) */
static const uint8_t *naming_prompts_data;

/* INITIAL_STATS — loaded from ROM via asset pipeline.
 * 4 characters × 20 bytes (coords, money, level, EXP, items). */
#define INITIAL_STATS_ENTRY_SIZE 20
static const uint8_t *initial_stats_data;

/* Resolve the file-select asset pointers (don't-care names, HP-meter speeds,
 * naming prompts, initial stats). These are link-time-constant ASSET_DATA bases,
 * not runtime loads — but they must be resolved before any consumer reads them.
 *
 * file_menu_setup() resolves them on the normal path, but a savestate taken on
 * the file-select/naming flow can be restored into a process where that setup
 * never ran (cross-process / cold load). Every consumer therefore calls this
 * idempotent guard first, matching the house pattern (inventory.c
 * ensure_item_config, map_loader.c map_loader_init). Without it, the post-naming
 * new-game init (NGN_FINALIZE) dereferenced a NULL initial_stats_data and
 * faulted as the meteorite cutscene started. */
static void ensure_file_select_assets(void) {
    if (initial_stats_data) return; /* all four are resolved together */
    dont_care_names_data = ASSET_DATA(ASSET_US_DATA_DONT_CARE_NAMES_BIN);
    hp_meter_speeds_data = ASSET_DATA(ASSET_DATA_HP_METER_SPEEDS_BIN);
    naming_prompts_data  = ASSET_DATA(ASSET_US_DATA_NAMING_PROMPTS_BIN);
    initial_stats_data   = ASSET_DATA(ASSET_DATA_INITIAL_STATS_BIN);
}

/* Helper: get EB-encoded don't care name at [category][index] */
static const uint8_t *get_dont_care_name(int category, int index) {
    ensure_file_select_assets();
    return dont_care_names_data + (category * DONT_CARE_COUNT + index) * DONT_CARE_NAME_SIZE;
}

/* See file_select.h. Mirrors a cold load: null the (un-sectioned) asset caches,
 * then confirm a consumer entry (get_dont_care_name) and the resolver re-resolve
 * all four. */
bool file_select_asset_selfheal_test(void) {
    initial_stats_data   = NULL;
    dont_care_names_data = NULL;
    hp_meter_speeds_data = NULL;
    naming_prompts_data  = NULL;
    (void)get_dont_care_name(0, 0);   /* a consumer must self-heal its own ptr */
    ensure_file_select_assets();      /* and the shared resolver */
    return initial_stats_data && dont_care_names_data &&
           hp_meter_speeds_data && naming_prompts_data;
}

/*
 * FILE_SELECT_INIT - Faithful port of asm/system/file_select_init.asm
 *
 * Sets up PPU, loads graphics, and prepares the file select screen.
 */
static void file_select_init(void) {
    /* Step 1: Force blank and wait vblank */
    ppu.inidisp = 0x80;

    /* Step 2: Initialize entity/graphics systems (simplified) */
    /* In the ROM: INIT_ENTITY_SYSTEM, OAM_CLEAR, UPDATE_SCREEN, etc. */
    for (int i = 0; i < 128; i++) {
        ppu.oam[i].y = EB_VIEWPORT_HEIGHT;
        ppu.oam_full_y[i] = EB_VIEWPORT_HEIGHT;
    }

    /* Step 3: Set BG mode $09 (Mode 1 + BG3 priority) */
    ppu.bgmode = 0x09;

    /* Step 4: Clear VRAM */
    memset(ppu.vram, 0, VRAM_SIZE);

    /* Step 5: Initialize text system (load fonts) */
    text_system_init();

    /* Step 6: Set up BG3 for text overlay */
    text_setup_bg3();

    /* Step 7: Upload tiny font tiles to VRAM as fallback fixed-width glyphs.
       Fills EB character positions 0x50-0xAF with font glyphs. */
    text_upload_font_tiles();

    /* Step 7b: Load window border graphics (TEXT_WINDOW_GFX).
       Must run AFTER font tile upload: the scattered mode-0 upload overwrites
       specific tile positions (including 0x51 = cursor lower-half) with the
       correct window graphics data. */
    text_load_window_gfx();

    /* Step 8: Initialize window system */
    window_system_init();

    /* Step 9: Clear BG3 tilemap
       Per assembly: writes 0 to VRAM at TEXT_LAYER_TILEMAP, $800 words */
    /* Clear BG3 tilemap: 32x32 entries = 1024 words = 2048 bytes */
    memset(win.bg2_buffer, 0, BG2_BUFFER_SIZE);
    uint32_t tilemap_offset = VRAM_TEXT_LAYER_TILEMAP * 2;
    uint32_t tilemap_size = 32 * 32 * 2;
    if (tilemap_offset + tilemap_size <= VRAM_SIZE) {
        memset(ppu.vram + tilemap_offset, 0, tilemap_size);
    }

    /* Step 10: Load flavour palette (default = plain/0) */
    text_load_flavour_palette(0);

    /* Step 11: Load battle background #230 (file select BG) */
    battle_bg_load(BATTLEBG_FILE_SELECT);

    /* Starfield BG2 fills viewport; BG3 text stays centered.
     * Clear fill[2] in case a previous screen left it set (e.g. logos). */
    ppu.bg_viewport_fill[1] = BG_VIEWPORT_FILL;
    ppu.bg_viewport_fill[2] = BG_VIEWPORT_CENTER;
    window_anchors_enabled = false;   /* the menus stay centred, not pinned to the corners */

    /* Step 11b: Set OBSEL = $62 (OBJ tile base at VRAM word $4000, 8x8/16x16 sizes)
       From OVERWORLD_SETUP_VRAM → SET_OAM_SIZE #$0062 */
    ppu.obsel = 0x62;

    /* Step 11c: Load sprite group ert.palettes (8 × 32 bytes = 256 bytes) into OBJ palette area.
       Assembly: MEMCPY16 from SPRITE_GROUP_PALETTES to PALETTES + BPP4PALETTE_SIZE*8.
       PALETTES maps to ppu.cgram[]; BPP4PALETTE_SIZE*8 = 256 bytes = 128 colors.
       OBJ ert.palettes occupy CGRAM entries 128-255. */
    {
        for (int p = 0; p < 8; p++) {
            size_t pal_size;
            pal_size = ASSET_SIZE(ASSET_OVERWORLD_SPRITES_PALETTES(p));
            const uint8_t *pal_data = ASSET_DATA(ASSET_OVERWORLD_SPRITES_PALETTES(p));
            if (pal_data && pal_size >= 32) {
                memcpy(&ppu.cgram[128 + p * 16], pal_data, 32);
                memcpy(&ert.palettes[128 + p * 16], pal_data, 32);
            }
        }
    }

    /* Step 12: Set TM = $16 (BG2 + BG3 + OBJ)
       This is the correct setting - BG2 for battle BG, BG3 for text, OBJ for cursor */
    ppu.tm = 0x16;
    ppu.ts = 0x00;

    /* Sprites use SNES-native coordinates; offset them to match centered BG3 */
    ppu.sprite_x_offset = EB_VIEWPORT_PAD_LEFT;
    ppu.sprite_y_offset = EB_VIEWPORT_PAD_TOP;

    /* Step 13: Clear scroll positions */
    ppu.bg_hofs[0] = 0; ppu.bg_vofs[0] = 0;
    ppu.bg_hofs[1] = 0; ppu.bg_vofs[1] = 0;
    /* BG3 scroll already set in text_setup_bg3 */

    /* Step 14: Set background color */
    ppu.cgram[0] = BGR555(0, 0, 0); /* black backdrop */
}

/*
 * Display the file select menu showing 3 save slots.
 * Returns selected slot (1-3).
 *
 * Faithful port of FILE_SELECT_MENU (file_select_menu.asm):
 *   Pass 1: build menu item labels ("N: CharName" or "N: Start New Game")
 *   Pass 2: for occupied slots, print "Level:" and "Text Speed:" info
 */
/* ---- File-select screen (port rework) ----------------------------------------
 * Left: the options (Settings, Exit Game). Right: a scrolling list of all
 * SAVE_COUNT slots, BROWSE_ROWS at a time. Left/Right switch panes, Up/Down
 * move (held = repeat), L/R page through the slots, A confirms. The same list
 * is the "Copy to where?" picker (copy mode: only empty slots can be chosen). */
#define BROWSE_ROWS 9
#define BROWSE_RESULT_SETTINGS 1000   /* results above SAVE_COUNT are the option rows */
#define BROWSE_RESULT_EXIT     1001
#define BROWSE_RESULT_RESTORE  1002
#define BROWSE_RESULT_COPY     1003   /* these three act on the highlighted save */
#define BROWSE_RESULT_DELETE   1004
#define BROWSE_RESULT_BACKUP   1005
#define BROWSE_RESULT_NGPLUS   1006   /* New Game+ (only listed when a cleared save exists) */
#define BROWSE_RESULT_CRAZY    1007   /* Crazy Mode (only listed once a New Game+ has been finished) */
enum { BROWSE_NORMAL = 0, BROWSE_COPY_DEST = 1, BROWSE_RESTORE_DEST = 2, BROWSE_NG_SOURCE = 3, BROWSE_NG_DEST = 4, BROWSE_CRAZY_DEST = 5 };

static char     slot_names[SAVE_COUNT][NAME_MAX_CHARS + 1];
static uint8_t  slot_levels[SAVE_COUNT];
static bool     slot_cleared[SAVE_COUNT];   /* beaten: New Game+ unlocked */
static bool     any_cleared;
static bool     any_crazy_unlocked;         /* a New Game+ has been finished */
static uint16_t browse_top;      /* first slot shown (0-based) */
static uint16_t browse_cursor;   /* highlighted slot (0-based) */
static uint8_t  browse_pane;     /* 0 = options, 1 = saves */
static uint8_t  browse_option;   /* highlighted option row */
static uint8_t  browse_mode;
static uint8_t  browse_dirty;
static uint8_t  input_guard;     /* frames to ignore buttons after a screen opens (the press that opened it must not act twice) */
#define INPUT_GUARD_FRAMES 10

/* Read every slot once: occupancy, first character's name and level. Assembly
 * (file_select_menu.asm:20-25) treats favourite_thing[1] != 0 as "in use". */
static void file_select_scan_saves(void) {
    any_cleared = false;
    any_crazy_unlocked = false;
    for (int slot = 0; slot < SAVE_COUNT; slot++) {
        bool occupied = load_game(slot) && game_state.favourite_thing[1] != 0;
        save_files_present[slot] = occupied;
        slot_names[slot][0] = '\0';
        slot_levels[slot] = 0;
        slot_cleared[slot] = occupied && ng_plus_slot_cleared(slot);
        any_cleared |= slot_cleared[slot];
        any_crazy_unlocked |= slot_cleared[slot] && ng_plus_crazy_unlocked(slot);
        if (!occupied) continue;
        for (int i = 0; i < NAME_MAX_CHARS && party_characters[0].name[i]; i++)
            slot_names[slot][i] = eb_char_to_ascii(party_characters[0].name[i]);
        slot_names[slot][NAME_MAX_CHARS] = '\0';
        slot_levels[slot] = party_characters[0].level;
    }
}

/* The left column, top to bottom; New Game+ is listed only while a cleared save exists. */
static uint16_t browse_options[8];
static const char *browse_option_labels[8];
static int browse_option_n;
static void browse_build_options(void) {
    int n = 0;
    if (any_cleared) { browse_options[n] = BROWSE_RESULT_NGPLUS; browse_option_labels[n++] = "New Game+"; }
    if (any_crazy_unlocked) { browse_options[n] = BROWSE_RESULT_CRAZY; browse_option_labels[n++] = "Crazy Mode"; }
    browse_options[n] = BROWSE_RESULT_COPY;   browse_option_labels[n++] = "Copy Save";
    browse_options[n] = BROWSE_RESULT_DELETE; browse_option_labels[n++] = "Delete Save";
#ifdef EB_HAS_FILE_BROWSER
    browse_options[n] = BROWSE_RESULT_BACKUP;  browse_option_labels[n++] = "Backup Save";
    browse_options[n] = BROWSE_RESULT_RESTORE; browse_option_labels[n++] = "Restore Save";
#endif
#ifdef EB_HAS_INPUT_SETTINGS
    browse_options[n] = BROWSE_RESULT_SETTINGS; browse_option_labels[n++] = "Settings";
#endif
    browse_options[n] = BROWSE_RESULT_EXIT;   browse_option_labels[n++] = "Exit Game";
    browse_option_n = n;
    if (browse_option >= n) browse_option = 0;
}
#define BROWSE_OPTION_COUNT browse_option_n

static void browse_show_cursor(uint16_t slot) {
    browse_cursor = slot;
    if (browse_cursor < browse_top) browse_top = browse_cursor;
    if (browse_cursor >= browse_top + BROWSE_ROWS) browse_top = (uint16_t)(browse_cursor - BROWSE_ROWS + 1);
    if (browse_top > SAVE_COUNT - BROWSE_ROWS) browse_top = SAVE_COUNT - BROWSE_ROWS;
}

/* Draw both panes. The scrollbar is the stand-in track at the list's right edge
 * (see browse_draw_scrollbar); the window title says which slots are showing. */
static void browse_draw_scrollbar(WindowInfo *w) {
    /* Track: one cell per row; the thumb row is the one the cursor's position maps to. */
    int thumb = (int)((uint32_t)browse_cursor * (BROWSE_ROWS - 1) / (SAVE_COUNT - 1));
    for (int r = 0; r < BROWSE_ROWS; r++) {
        set_focus_text_cursor((uint16_t)(w->width - 3), (uint16_t)r);
        print_string(r == thumb ? "*" : ".");
    }
}

static void browse_draw(bool show_cursor) {
    close_window(WINDOW_FILE_SELECT_MAIN);      /* a fresh window: no highlight left from the last cursor position */
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_MAIN);
    if (!w) return;
    w->menu_count = 0;
    char title[24];
    snprintf(title, sizeof(title), browse_mode == BROWSE_COPY_DEST ? "Copy to %d/%d" :
                                   browse_mode == BROWSE_RESTORE_DEST ? "Restore to %d/%d" :
                                   browse_mode == BROWSE_NG_SOURCE ? "New Game+ from %d/%d" :
                                   browse_mode == BROWSE_NG_DEST ? "New Game+ in %d/%d" :
                                   browse_mode == BROWSE_CRAZY_DEST ? "Crazy Mode in %d/%d" : "Saves %d/%d",
             browse_cursor + 1, SAVE_COUNT);
    set_window_title(WINDOW_FILE_SELECT_MAIN, title, -1);

    static char labels[BROWSE_ROWS][32];
    for (int r = 0; r < BROWSE_ROWS; r++) {
        int slot = browse_top + r;
        if (slot >= SAVE_COUNT) break;
        if (save_files_present[slot])
            snprintf(labels[r], sizeof(labels[r]), "%d: %s%s", slot + 1, slot_names[slot],
                     slot_cleared[slot] ? " +" : "");   /* + marks a beaten game: New Game+ unlocked */
        else
            snprintf(labels[r], sizeof(labels[r]), "%d: %s", slot + 1,
                     browse_mode != BROWSE_NORMAL ? "Empty" : "Start New Game");
        add_menu_item(labels[r], (uint16_t)(slot + 1), 0, (uint16_t)r);
    }
    open_window_and_print_menu(1, 0);
    for (int r = 0; r < BROWSE_ROWS; r++) {
        int slot = browse_top + r;
        if (slot >= SAVE_COUNT || !save_files_present[slot]) continue;
        set_focus_text_cursor(12, (uint16_t)r);
        print_string("Lv");
        set_focus_text_cursor(14, (uint16_t)r);
        print_number(slot_levels[slot], 2);
    }
    browse_draw_scrollbar(w);
    if (show_cursor && browse_pane == 1 && browse_cursor >= browse_top &&
        browse_cursor < browse_top + BROWSE_ROWS)
        highlight_menu_item(w, (uint16_t)(browse_cursor - browse_top), 6, true);

    if (browse_mode == BROWSE_NORMAL) {
        close_window(WINDOW_FILE_SELECT_OPTIONS);
        WindowInfo *opts = create_window(WINDOW_FILE_SELECT_OPTIONS);
        if (opts) {
            opts->menu_count = 0;
            browse_build_options();
            window_set_height(opts, (uint16_t)(2 * BROWSE_OPTION_COUNT + 2));   /* tall enough for every row */
            for (int row = 0; row < BROWSE_OPTION_COUNT; row++)
                add_menu_item(browse_option_labels[row], browse_options[row], 0, (uint16_t)row);
            print_menu_items();
            if (show_cursor && browse_pane == 0) highlight_menu_item(opts, browse_option, 6, true);
        }
    }
}

static int browse_option_count(void) { return BROWSE_OPTION_COUNT; }
static uint16_t browse_option_result(int row) { return browse_options[row]; }

/* One frame of input for the browse screen. Returns the chosen result (slot 1..N or an
 * option code), 0xFFFF for "cancelled" (copy mode, B), or 0 while nothing was chosen. */
static uint16_t browse_step_input(void) {
    if (input_guard) { input_guard--; return 0; }
    uint16_t pressed = platform_input_get_pad_new();
    uint16_t held = pressed ? 0 : (uint16_t)core.pad1_autorepeat;   /* repeat only once it kicks in */
    uint16_t dir = pressed | held;
    bool in_list = browse_pane == 1 || browse_mode != BROWSE_NORMAL;

    if (in_list) {
        if (dir & PAD_UP) {
            if (browse_cursor > 0) { browse_show_cursor((uint16_t)(browse_cursor - 1)); browse_dirty = 1; play_sfx(3); }
            else if (pressed) { browse_show_cursor(SAVE_COUNT - 1); browse_dirty = 1; play_sfx(3); }   /* wrap */
        } else if (dir & PAD_DOWN) {
            if (browse_cursor < SAVE_COUNT - 1) { browse_show_cursor((uint16_t)(browse_cursor + 1)); browse_dirty = 1; play_sfx(3); }
            else if (pressed) { browse_show_cursor(0); browse_dirty = 1; play_sfx(3); }
        } else if (pressed & PAD_L) {
            browse_show_cursor(browse_cursor > BROWSE_ROWS ? (uint16_t)(browse_cursor - BROWSE_ROWS) : 0);
            browse_dirty = 1; play_sfx(2);
        } else if (pressed & PAD_R) {
            browse_show_cursor(browse_cursor + BROWSE_ROWS < SAVE_COUNT ? (uint16_t)(browse_cursor + BROWSE_ROWS) : SAVE_COUNT - 1);
            browse_dirty = 1; play_sfx(2);
        } else if ((pressed & PAD_LEFT) && browse_mode == BROWSE_NORMAL) {
            browse_pane = 0; browse_dirty = 1; play_sfx(2);
        }
    } else {
        int n = browse_option_count();
        if (dir & PAD_UP)        { browse_option = (uint8_t)((browse_option + n - 1) % n); browse_dirty = 1; play_sfx(3); }
        else if (dir & PAD_DOWN) { browse_option = (uint8_t)((browse_option + 1) % n);     browse_dirty = 1; play_sfx(3); }
        else if (pressed & PAD_RIGHT) { browse_pane = 1; browse_dirty = 1; play_sfx(2); }
    }

    if (pressed & PAD_CONFIRM) {
        if (in_list) {
            int slot = browse_cursor;
            if (browse_mode == BROWSE_COPY_DEST && (save_files_present[slot] || slot == current_save_slot - 1)) {
                play_sfx(4);   /* can't copy onto a used slot */
                return 0;
            }
            if ((browse_mode == BROWSE_NG_SOURCE && !slot_cleared[slot]) ||
                ((browse_mode == BROWSE_NG_DEST || browse_mode == BROWSE_CRAZY_DEST) && save_files_present[slot])) {
                play_sfx(4);   /* New Game+ continues a cleared save, in a free slot */
                return 0;
            }
            return (uint16_t)(slot + 1);
        }
        return browse_option_result(browse_option);
    }
    if ((pressed & PAD_CANCEL) && browse_mode != BROWSE_NORMAL) return 0xFFFF;
    return 0;
}

#ifdef EB_HAS_FILE_BROWSER
/* ---- Save backup / restore (port addition) ----------------------------------------
 * Backup (in the existing-save submenu) copies the slot's save image into a folder the
 * player picks; Restore (left column) picks a .srm file, then the slot to put it in.
 * The folder browser is drawn with the game's own windows, like the 99-slot list. */
enum { FB_BACKUP = 0, FB_RESTORE = 1 };
enum { FBROW_HERE, FBROW_PARENT, FBROW_DIR, FBROW_FILE };
#define FB_MAX_ENTRIES 512
#define FB_ROWS 8                         /* entries visible (the window's first row is the prompt) */
static PlatformDirEntry fb_list[FB_MAX_ENTRIES];
static int      fb_list_count;
static uint16_t fb_row_kind[FB_MAX_ENTRIES + 2];
static uint16_t fb_row_index[FB_MAX_ENTRIES + 2];
static int      fb_row_count;
static char     fb_dir[512];
static uint8_t  fb_purpose;
static uint16_t fb_top, fb_cursor;
static uint8_t  fb_dirty;
static char     fb_chosen[1100];          /* the file picked (restore) or written (backup) */
static char     restore_path[1100];       /* save image waiting for a slot */
static char     fb_msg[2][48];
static uint8_t  fb_msg_next;              /* phase after the message */

static void fb_refresh(void) {
    fb_list_count = platform_dir_list(fb_dir, fb_purpose == FB_RESTORE ? ".srm" : NULL, fb_list, FB_MAX_ENTRIES);
    if (fb_list_count < 0) fb_list_count = 0;
    fb_row_count = 0;
    if (fb_purpose == FB_BACKUP) {
        fb_row_kind[fb_row_count] = FBROW_HERE;   fb_row_index[fb_row_count++] = 0;
    }
    char parent[512];
    snprintf(parent, sizeof(parent), "%s", fb_dir);
    if (platform_dir_parent(parent)) {
        fb_row_kind[fb_row_count] = FBROW_PARENT; fb_row_index[fb_row_count++] = 0;
    }
    for (int i = 0; i < fb_list_count && fb_row_count < FB_MAX_ENTRIES + 2; i++) {
        fb_row_kind[fb_row_count] = fb_list[i].is_dir ? FBROW_DIR : FBROW_FILE;
        fb_row_index[fb_row_count++] = (uint16_t)i;
    }
    fb_top = 0;
    fb_cursor = (fb_purpose == FB_BACKUP && fb_row_count > 1) ? 1 : 0;   /* backup starts on the first folder, not "save here" */
    if (fb_purpose == FB_BACKUP) fb_cursor = 0;
    fb_dirty = 1;
}

static void fb_scroll_to_cursor(void) {
    if (fb_cursor < fb_top) fb_top = fb_cursor;
    if (fb_cursor >= fb_top + FB_ROWS) fb_top = (uint16_t)(fb_cursor - FB_ROWS + 1);
}

/* Label for a row, cut to what the window can show. */
static void fb_label(int row, char *out, size_t n) {
    switch (fb_row_kind[row]) {
    case FBROW_HERE:   snprintf(out, n, "Save here"); break;
    case FBROW_PARENT: snprintf(out, n, "../  (up one folder)"); break;
    case FBROW_DIR:    snprintf(out, n, "%.22s/", fb_list[fb_row_index[row]].name); break;
    default:           snprintf(out, n, "%.24s", fb_list[fb_row_index[row]].name); break;
    }
}

static void fb_draw(void) {
    WindowInfo *w = create_window(WINDOW_FILE_BROWSER);
    if (!w) return;
    w->menu_count = 0;
    /* The title shows the tail of the current folder. */
    size_t len = strlen(fb_dir);
    set_window_title(WINDOW_FILE_BROWSER, len > 22 ? fb_dir + len - 22 : fb_dir, -1);
    set_focus_text_cursor(0, 0);
    print_string(fb_purpose == FB_BACKUP ? "Choose a folder for the backup."
                                         : "Choose a save file to restore.");
    static char labels[FB_ROWS][MENU_LABEL_SIZE];
    for (int r = 0; r < FB_ROWS; r++) {
        int row = fb_top + r;
        if (row >= fb_row_count) break;
        fb_label(row, labels[r], sizeof(labels[r]));
        add_menu_item(labels[r], (uint16_t)(row + 1), 0, (uint16_t)(r + 1));
    }
    if (fb_row_count == 0) {
        set_focus_text_cursor(0, 1);
        print_string("(no save files here)");
    }
    print_menu_items();
    if (fb_cursor >= fb_top && fb_cursor < fb_top + FB_ROWS && fb_cursor < fb_row_count)
        highlight_menu_item(w, (uint16_t)(fb_cursor - fb_top), 6, true);
}

/* Returns 0 while browsing, 0xFFFF once cancelled, 1 once a choice is in fb_chosen. */
static uint16_t fb_step_input(void) {
    if (input_guard) { input_guard--; return 0; }
    uint16_t pressed = platform_input_get_pad_new();
    uint16_t held = pressed ? 0 : (uint16_t)core.pad1_autorepeat;
    uint16_t dir = pressed | held;
    int last = fb_row_count - 1;

    if (last >= 0) {
        if (dir & PAD_UP) {
            if (fb_cursor > 0) { fb_cursor--; fb_dirty = 1; play_sfx(3); }
            else if (pressed)  { fb_cursor = (uint16_t)last; fb_dirty = 1; play_sfx(3); }
        } else if (dir & PAD_DOWN) {
            if (fb_cursor < last) { fb_cursor++; fb_dirty = 1; play_sfx(3); }
            else if (pressed)     { fb_cursor = 0; fb_dirty = 1; play_sfx(3); }
        } else if (pressed & PAD_L) {
            fb_cursor = fb_cursor > FB_ROWS ? (uint16_t)(fb_cursor - FB_ROWS) : 0; fb_dirty = 1; play_sfx(2);
        } else if (pressed & PAD_R) {
            fb_cursor = fb_cursor + FB_ROWS <= last ? (uint16_t)(fb_cursor + FB_ROWS) : (uint16_t)last; fb_dirty = 1; play_sfx(2);
        }
        fb_scroll_to_cursor();
    }
    if (pressed & PAD_CANCEL) return 0xFFFF;
    if ((pressed & PAD_CONFIRM) && last >= 0) {
        int row = fb_cursor;
        switch (fb_row_kind[row]) {
        case FBROW_HERE:
            return 1;                                   /* fb_dir is the destination */
        case FBROW_PARENT:
            platform_dir_parent(fb_dir);
            fb_refresh();
            break;
        case FBROW_DIR: {
            size_t n = strlen(fb_dir);
            snprintf(fb_dir + n, sizeof(fb_dir) - n, "%s%s", strcmp(fb_dir, "/") ? "/" : "",
                     fb_list[fb_row_index[row]].name);
            fb_refresh();
            break;
        }
        default:
            snprintf(fb_chosen, sizeof(fb_chosen), "%s/%s", fb_dir, fb_list[fb_row_index[row]].name);
            return 1;
        }
        play_sfx(2);
    }
    return 0;
}

static void fb_start(uint8_t purpose) {
    input_guard = INPUT_GUARD_FRAMES;
    fb_purpose = purpose;
    if (fb_dir[0] == '\0') snprintf(fb_dir, sizeof(fb_dir), "%s", platform_home_dir());
    fb_refresh();
}

/* One window with up to two lines, closed with A or B, then the cascade continues at `next`. */
static void fm_message_open(FileMenuState *st, uint8_t next, const char *line1, const char *line2) {
    snprintf(fb_msg[0], sizeof(fb_msg[0]), "%.40s", line1);          /* long file names are cut to fit the window */
    snprintf(fb_msg[1], sizeof(fb_msg[1]), "%.40s", line2 ? line2 : "");
    fb_msg_next = next;
    input_guard = INPUT_GUARD_FRAMES;
    close_all_windows();
    WindowInfo *w = create_window(WINDOW_SETTINGS_MESSAGE);
    if (w) {
        w->menu_count = 0;
        set_focus_text_cursor(0, 0);
        print_string(fb_msg[0]);
        if (fb_msg[1][0]) { set_focus_text_cursor(0, 1); print_string(fb_msg[1]); }
    }
    st->phase = FM_MSG;
}

/* "save005-Kramlat-L12.srm", numbered further if that name is taken. */
static void fb_backup_name(char *out, size_t n, int slot) {
    char who[NAME_MAX_CHARS + 1] = "";
    if (load_game(slot))
        for (int i = 0; i < NAME_MAX_CHARS && party_characters[0].name[i]; i++)
            who[i] = eb_char_to_ascii(party_characters[0].name[i]);
    for (char *c = who; *c; c++)                       /* keep it a safe file name */
        if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'))) *c = '_';
    for (int k = 0; k < 100; k++) {
        char suffix[8] = "";
        if (k) snprintf(suffix, sizeof(suffix), "-%d", k + 1);
        snprintf(out, n, "%s%s%s%03d%s%s%s-L%d%s.srm", fb_dir, strcmp(fb_dir, "/") ? "/" : "", "save", slot + 1,
                 who[0] ? "-" : "", who, "", party_characters[0].level, suffix);
        if (!platform_file_exists(out)) return;
    }
}
#endif /* EB_HAS_FILE_BROWSER */

/*
 * Show the sub-menu for a valid file (Continue/Copy/Delete/Set Up).
 */
/* Build the Continue/Copy/Delete/Set Up submenu (formerly show_file_select_
 * submenu() minus its blocking selection_menu(1)). Returns -1 when ready to push,
 * or 0 on create-window failure. */
static int fm_submenu_build(void) {
    WindowInfo *w = create_window(WINDOW_FILE_SELECT_MENU);
    if (!w) return 0;

    /* Copy, Backup and Delete are in the left column now; this menu keeps the other two. */
    add_menu_item("Continue", 1, 0, 0);

    add_menu_item("Set Up", 4, 10, 0);

    /* Assembly (show_file_select_menu.asm:58-62): PRINT_MENU_ITEMS, then (US
     * only) ENABLE_WORD_WRAP = $FF, then SELECTION_MENU directly — no
     * RENDER_ALL_WINDOWS in between. The caller pushes
     * GAME_MODE_SELECTION_MENU(allow_cancel=1). */
    print_menu_items();
    dt.enable_word_wrap = 0xFF;
    return -1;
}

/* Open a setup / menu window. If there is no room for it (the text layer's tile pool is shared by all
 * open windows), close the other windows and try again rather than fail: a failed build used to read as
 * "cancel" and bounced the player back to the previous dialog. */
static WindowInfo *fm_create_window(uint16_t window_id) {
    WindowInfo *w = create_window(window_id);
    if (!w) {
        close_all_windows();
        w = create_window(window_id);
    }
    return w;
}

/*
 * Display-only versions of setup menus — recreate the window with items
 * printed but no selection_menu() call.  Matches assembly's arg=1 path
 * (FILE_SELECT_TEXT_SPEED_MENU(1), FILE_SELECT_SOUND_MODE_MENU(1))
 * which highlights the previously selected option and returns immediately.
 */
static void text_speed_menu_display_only(uint8_t selected_speed) {
    WindowInfo *w = fm_create_window(WINDOW_FILE_SELECT_TEXT_SPEED);
    if (!w) return;
    w->menu_count = 0;
    set_focus_text_cursor(0, 0);
    print_string("Please select text speed.");
    add_menu_item("Fast", 1, 0, 1);
    add_menu_item("Medium", 2, 0, 2);
    add_menu_item("Slow", 3, 0, 3);
    print_menu_items();

    /* Assembly (file_select_text_speed_menu.asm:49-68): highlight with palette 6 */
    if (selected_speed >= 1 && selected_speed <= 3)
        highlight_menu_item(w, selected_speed - 1, 6, true);
}

static void sound_mode_menu_display_only(uint8_t selected_sound) {
    WindowInfo *w = fm_create_window(WINDOW_FILE_SELECT_MUSIC_MODE);
    if (!w) return;
    w->menu_count = 0;
    set_focus_text_cursor(0, 0);
    print_string("Please select sound setting.");
    add_menu_item("Stereo", 1, 0, 1);
    add_menu_item("Mono", 2, 0, 2);
    print_menu_items();

    /* Assembly (file_select_sound_mode_menu.asm:50-69): highlight with palette 6.
     * C port stores sound_setting as 0-indexed (0=Stereo, 1=Mono). */
    if (selected_sound <= 1)
        highlight_menu_item(w, selected_sound, 6, true);
}

/*
 * Text speed selection menu.
 */
/* Build the text-speed menu (formerly text_speed_menu() minus its blocking
 * selection_menu(1)). Returns -1 when ready to push, 0 on create-window failure. */
static int fm_textspeed_build(void) {
    WindowInfo *w = fm_create_window(WINDOW_FILE_SELECT_TEXT_SPEED);
    if (!w) return 0;
    w->menu_count = 0; /* Reset for re-entry (back-navigation) */

    set_focus_text_cursor(0, 0);
    print_string("Please select text speed.");

    add_menu_item("Fast", 1, 0, 1);
    add_menu_item("Medium", 2, 0, 2);
    add_menu_item("Slow", 3, 0, 3);

    print_menu_items();
    /* Assembly (file_select_text_speed_menu.asm:75) clears ENABLE_WORD_WRAP
     * here so selection_menu's confirm handler routes to APPLY_WINDOW_TEXT_ATTRIBUTES
     * instead of SET_FILE_SELECT_TEXT_HIGHLIGHT.  The C port gates highlight
     * by window ID instead (see selection_menu confirm handler), so this
     * clear is unnecessary and would break word-wrap if not restored. */
    return -1;
}

/* Apply a text-speed selection (result > 0). */
static void fm_textspeed_apply(uint16_t result) {
    game_state.text_speed = (uint8_t)result;
    /* Compute dt.text_speed_based_wait for TEXT_SPEED_DELAY.
     * Assembly (file_select_menu_loop.asm:681-688):
     * text_speed==3 (Slow) → 0 (instant text, halt at end of line).
     * Otherwise → (text_speed - 1) * 30 frames per character. */
    if (result == 3) {
        dt.text_speed_based_wait = 0;
    } else {
        dt.text_speed_based_wait = result * 30;
    }
}

/*
 * Sound mode selection menu.
 */
/* Build the sound-mode menu (formerly sound_mode_menu() minus selection_menu(1)).
 * Returns -1 when ready to push, 0 on create-window failure. */
static int fm_sound_build(void) {
    WindowInfo *w = fm_create_window(WINDOW_FILE_SELECT_MUSIC_MODE);
    if (!w) return 0;
    w->menu_count = 0; /* Reset for re-entry (back-navigation) */

    set_focus_text_cursor(0, 0);
    print_string("Please select sound setting.");

    add_menu_item("Stereo", 1, 0, 1);
    add_menu_item("Mono", 2, 0, 2);

    print_menu_items();
    return -1;
}

/* Apply a sound-mode selection (result > 0). */
static void fm_sound_apply(uint16_t result) {
    game_state.sound_setting = (uint8_t)(result - 1);
}

/*
 * Cursor move callback for flavour menu — previews window appearance as user
 * navigates.  Matches PREVIEW_WINDOW_FLAVOUR (C1EC8F.asm):
 *   1. Save original text_flavour
 *   2. Set text_flavour = new value
 *   3. LOAD_WINDOW_GFX (decompress + flavour-dependent tile generation)
 *   4. UPLOAD_TEXT_TILES_TO_VRAM(2) (re-upload scattered + bulk tiles)
 *   5. LOAD_CHARACTER_WINDOW_PALETTE
 *   6. Restore original text_flavour
 */
static void preview_flavour_callback(uint16_t value) {
    uint8_t orig = game_state.text_flavour;
    game_state.text_flavour = (uint8_t)value;  /* 1-indexed, matching assembly */
    text_load_window_gfx();
    load_character_window_palette();
    game_state.text_flavour = orig;
}

/* file_select.c-owned half of the cursor-callback id resolver (savestate pointer
 * purge, build item #3). See window_resolve_cursor_callback(). */
void (*file_select_cursor_callback_from_id(uint8_t id))(uint16_t) {
    if (id == CURSOR_CB_FLAVOUR_PREVIEW) return preview_flavour_callback;
    return NULL;
}

/*
 * Window flavour selection menu.
 */
/* Build the window-flavour menu (formerly flavour_menu() minus selection_menu(1)).
 * Returns -1 when ready to push, 0 on create-window failure. */
static int fm_flavour_build(void) {
    WindowInfo *w = fm_create_window(WINDOW_FILE_SELECT_FLAVOUR);
    if (!w) return 0;
    w->menu_count = 0; /* Reset for re-entry (back-navigation) */

    set_focus_text_cursor(0, 0);
    print_string("Which style of windows do you prefer?");

    add_menu_item("Plain flavor", 1, 0, 2);
    add_menu_item("Mint flavor", 2, 0, 3);
    add_menu_item("Strawberry flavor", 3, 0, 4);
    add_menu_item("Banana flavor", 4, 0, 5);
    add_menu_item("Peanut flavor", 5, 0, 6);
    add_menu_item("Raspberry flavor", 6, 0, 7);
    add_menu_item("Lime flavor", 7, 0, 8);
    add_menu_item("Grape flavor", 8, 0, 9);
    add_menu_item("Orange flavor", 9, 0, 10);
    add_menu_item("Chocolate flavor", 10, 0, 11);

    /* Set cursor move callback to preview palette (asm: SET_CURSOR_MOVE_CALLBACK).
     * Set the serializable id alongside the ptr (savestate hardening, D0a). */
    w->cursor_move_callback = preview_flavour_callback;
    w->cursor_move_callback_id = CURSOR_CB_FLAVOUR_PREVIEW;

    print_menu_items();
    return -1;
}

/* Apply a window-flavour selection (result > 0). */
static void fm_flavour_apply(uint16_t result) {
    game_state.text_flavour = (uint8_t)result;  /* 1-indexed, matching assembly */
    text_load_flavour_palette(game_state.text_flavour - 1);
}

/*
 * Naming screen — faithful port of text_input_dialog.asm + name_a_character.asm.
 *
 * The keyboard window (0x1C: x=1, y=9, w=30, h=16) renders EB char tiles.
 * Each selectable position has a $2F marker tile at an even text_x, with the
 * character tile at text_x+1.  The cursor can only land on positions with
 * $2F tiles.
 *
 * Grid layout (text_y 0-4, plus row 6 for buttons):
 *   Rows 0-2: 9 letters + gap + 2 punctuation  (11 selectable per row)
 *   Row 3:    10 digits + gap + 2 punctuation   (12 selectable)
 *   Row 4:    CAPITAL + small + gap + 2 punctuation
 *   Row 5:    (empty)
 *   Row 6:    Don't Care + Backspace + OK
 *
 * Layouts 0/2 = uppercase, 1/3 = lowercase.  Layout 4/5 = button labels.
 *
 * Font tile VRAM placement:
 *   The keyboard keys and buttons are printed once into the keyboard window
 *   by DISPLAY_TEXT of the KEYBOARD.ebtxt layouts (kb_display_layout), so they
 *   use the window's ALLOC_BG2_TILEMAP_ENTRY tiles like any window text.  Only
 *   the name display uses fixed tiles (0x2E0+, RENDER_KEYBOARD_INPUT_CHARACTER).
 */

/* Keyboard grid dimensions for character rows (text_y 0-3) */
#define KB_GRID_ROWS   5   /* rows 0-4 */
#define KB_GRID_COLS  14   /* max columns per row (from offset table) */
#define KB_TOTAL_ROWS  8   /* total text_y lines (0-7, height/2) */

/* EB character code for each grid position.
   Row index = text_y (0-4), column index = grid column (0-13).
   0xFF = no character at this position. */
static const uint8_t kb_upper_grid[KB_GRID_ROWS][KB_GRID_COLS] = {
    /* Row 0: A-I, gap, -, {        */
    { 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0xFF, 0xFF, 0x5D, 0x53, 0xFF },
    /* Row 1: J-R, gap, ', ♪($AE)   */
    { 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0xFF, 0xFF, 0x57, 0xAE, 0xFF },
    /* Row 2: S-Z, space, gap, ., /($5F) */
    { 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x50, 0xFF, 0xFF, 0x5E, 0x5F, 0xFF },
    /* Row 3: 0-9, gap, !, |($AC)    */
    { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0xFF, 0x51, 0xAC, 0xFF },
    /* Row 4: no chars (CAPITAL/small buttons handled separately) + gap + ?, ♫($AF) */
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x6F, 0xAF, 0xFF },
};

static const uint8_t kb_lower_grid[KB_GRID_ROWS][KB_GRID_COLS] = {
    /* Row 0: a-i, gap, -, {        */
    { 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0xFF, 0xFF, 0x5D, 0x53, 0xFF },
    /* Row 1: j-r, gap, ', ♪($AE)   */
    { 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xFF, 0xFF, 0x57, 0xAE, 0xFF },
    /* Row 2: s-z, space, gap, ., /($5F) */
    { 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0x50, 0xFF, 0xFF, 0x5E, 0x5F, 0xFF },
    /* Row 3: 0-9, gap, !, |($AC)    */
    { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0xFF, 0x51, 0xAC, 0xFF },
    /* Row 4: no chars + gap + ?, ♫($AF) */
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x6F, 0xAF, 0xFF },
};

/* Valid cursor stop positions per row.  Each entry is a text_x where a $2F
   marker tile should be placed.  -1 terminates the list. */
static const int kb_row_stops[KB_TOTAL_ROWS][15] = {
    /* text_y=0 */ { 0, 2, 4, 6, 8, 10, 12, 14, 16,  22, 24,  -1 },
    /* text_y=1 */ { 0, 2, 4, 6, 8, 10, 12, 14, 16,  22, 24,  -1 },
    /* text_y=2 */ { 0, 2, 4, 6, 8, 10, 12, 14, 16,  22, 24,  -1 },
    /* text_y=3 */ { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18,  22, 24,  -1 },
    /* text_y=4 */ { 0, 7,  22, 24,  -1 },
    /* text_y=5 */ { -1 },  /* empty row */
    /* text_y=6 */ { 0, 17, 25,  -1 },
    /* text_y=7 */ { -1 },
};

/* Map from text_x of a cursor stop to the grid column index (for rows 0-3).
   Returns -1 if no grid lookup (handled as button). */
static int kb_text_x_to_grid_col(int text_x) {
    if (text_x >= 0 && text_x <= 18 && (text_x % 2) == 0)
        return text_x / 2;
    if (text_x == 22) return 11;
    if (text_x == 24) return 12;
    return -1;
}

/* Check if a position has a valid cursor stop.
   Uses the static kb_row_stops[] table rather than scanning the tilemap. */
static bool kb_is_valid_stop(WindowInfo *kw, int text_x, int text_y) {
    (void)kw;
    if (text_y < 0 || text_y >= KB_TOTAL_ROWS) return false;
    if (text_x < 0) return false;
    for (int i = 0; kb_row_stops[text_y][i] >= 0; i++) {
        if (kb_row_stops[text_y][i] == text_x) return true;
    }
    return false;
}

/* Find next valid cursor position scanning in (dx, dy) direction.
   Matches FIND_NEXT_MENU_OPTION scanning logic:
   - Primary: scan in dy direction, keeping text_x fixed
   - Secondary: scan in dx direction at each row
   Returns packed (text_y << 8 | text_x), or -1 if none found. */
static int kb_find_next(WindowInfo *kw, int cur_x, int cur_y, int dx, int dy) {
    int half_height = (int)kw->height / 2;
    int width = (int)kw->width;

    if (dy != 0) {
        /* Vertical movement: scan rows in dy direction, try to keep text_x */
        int y = cur_y + dy;
        for (int steps = 0; steps < half_height; steps++) {
            if (y < 0) y = half_height - 1;
            if (y >= half_height) y = 0;
            if (kb_is_valid_stop(kw, cur_x, y))
                return (y << 8) | cur_x;
            /* Try scanning left from cur_x */
            for (int x = cur_x - 1; x >= 0; x--) {
                if (kb_is_valid_stop(kw, x, y))
                    return (y << 8) | x;
            }
            /* Try scanning right from cur_x */
            for (int x = cur_x + 1; x < width; x++) {
                if (kb_is_valid_stop(kw, x, y))
                    return (y << 8) | x;
            }
            y += dy;
        }
    } else if (dx != 0) {
        /* Horizontal movement: scan in dx direction on same row */
        int x = cur_x + dx;
        for (int steps = 0; steps < width; steps++) {
            if (x < 0) x = width - 1;
            if (x >= width) x = 0;
            if (kb_is_valid_stop(kw, x, cur_y))
                return (cur_y << 8) | x;
            x += dx;
        }
    }
    return -1;
}

/* Print the keyboard layout into the keyboard window (the focus window), as
   TEXT_INPUT_DIALOG does after CREATE_WINDOW (text_input_dialog.asm:44-87 and
   the @REDRAW_KEYBOARD copy at 98-144). The layout texts (KEYBOARD.ebtxt) place
   every key with FORCE_TEXT_ALIGNMENT and print it through DISPLAY_TEXT, so the
   glyph tiles come from the window's ALLOC_BG2_TILEMAP_ENTRY tiles and stay in
   the window's content tilemap; they are not re-rendered per frame.
   Without Don't Care (PARAM04 == -1) the button layout is pointer +20 and the
   key layout is pointer 8 + layout*4 (the "B" set); with it, +16 and layout*4.
   The buttons print with the current CHARACTER_PADDING, the keys with 0. */
static void kb_display_layout(const TextInputState *s) {
    static const uint32_t key_layouts[2][2] = {
        { MSG_KBD_LAYOUT_UPPERCASE_A, MSG_KBD_LAYOUT_LOWERCASE_A },
        { MSG_KBD_LAYOUT_UPPERCASE_B, MSG_KBD_LAYOUT_LOWERCASE_B },
    };
    bool no_dont_care = !s->has_dont_care;
    display_text_from_addr_inline(no_dont_care ? MSG_KBD_LAYOUT_BUTTONS_NO_DONT_CARE
                                               : MSG_KBD_LAYOUT_BUTTONS_DONT_CARE);
    character_padding = 0;
    display_text_from_addr_inline(key_layouts[no_dont_care][s->is_lowercase ? 1 : 0]);
    character_padding = 1;
}

/* VRAM tile index for name display, matching assembly's
   VRAM::TEXT_LAYER_TILES + $1700 = tile index 0x2E0 relative to BG3 tile base.
   Tiles are uploaded once on change; tilemap entries are written every frame
   since render_all_windows() clears the tilemap. */
#define NAME_VRAM_TILE_BASE 0x2E0

/* Build the EB-encoded display string for the name box.
   Entered chars + bullet at insertion point + dashes for empty slots. */
static void kb_build_name_display(uint8_t *display, const uint8_t *eb_name,
                                   int name_pos, int max_len) {
    for (int i = 0; i < max_len; i++) {
        if (i < name_pos && eb_name[i] != 0)
            display[i] = eb_name[i];   /* entered character */
        else if (i == name_pos)
            display[i] = 0x70;         /* CHAR::BULLET — centered dot at insertion point */
        else
            display[i] = 0x53;         /* placeholder dash (EB 0x53) */
    }
    display[max_len] = 0;
}

/* Upload name display VWF tiles to the dedicated VRAM range.
   Only called when the name changes (matching assembly's
   RENDER_KEYBOARD_INPUT_CHARACTER which is called per-change, not per-frame).
   Returns the number of tile columns rendered. */
static int kb_render_name_tiles(const uint8_t *display, int max_len) {
    return vwf_render_to_fixed_tiles(display, max_len, FONT_ID_NORMAL, NAME_VRAM_TILE_BASE, 0);
}

/* Write tilemap entries for the name display at a specific text row.
   Called every frame since render_all_windows() clears the tilemap.
   Matches the @WRITE_CHAR_TO_TILEMAP loop in RENDER_KEYBOARD_INPUT_CHARACTER. */
static void kb_write_name_tilemap_at(WindowInfo *nw, int num_cols, int text_y) {
    uint16_t *tilemap = (uint16_t *)win.bg2_buffer;
    uint16_t mx = nw->content_x;
    uint16_t my = nw->content_y + (uint16_t)(text_y * 2);
    for (int i = 0; i < num_cols; i++) {
        if (mx + i >= 32) break;
        uint16_t upper_tile = NAME_VRAM_TILE_BASE + i * 2;
        uint16_t lower_tile = NAME_VRAM_TILE_BASE + i * 2 + 1;
        tilemap[my * 32 + mx + i] = 0x2000 | upper_tile;
        if (my + 1 < 32)
            tilemap[(my + 1) * 32 + mx + i] = 0x2000 | lower_tile;
    }
}

/* Convenience wrapper: name display at text row 0 (for file select name box). */
static void kb_write_name_tilemap(WindowInfo *nw, int num_cols) {
    kb_write_name_tilemap_at(nw, num_cols, 0);
}

/*
 * text_input_dialog — shared keyboard input loop for naming screens.
 * Port of TEXT_INPUT_DIALOG (asm/text/text_input_dialog.asm). Run to completion
 * as GAME_MODE_TEXT_INPUT; the wrapper at the bottom seeds the state and pumps.
 * Creates the keyboard window (0x1C), runs the input loop, closes on done. Name
 * display tiles render in name_display_window_id at text row name_text_y (0 for
 * the file-select name box, 1 for NAMING_PROMPT where row 0 shows the prompt).
 */
/* Resolve a NameTargetId to the (stable global) buffer it names. The buffer is
 * only touched on confirm, so a serializable ID rather than a raw pointer keeps
 * the keyboard mode's ModeState plain-old-data. */
static uint8_t *name_target_buffer(int id) {
    switch (id) {
    case NAME_TARGET_PARTY0:    return party_characters[0].name;
    case NAME_TARGET_PARTY1:    return party_characters[1].name;
    case NAME_TARGET_PARTY2:    return party_characters[2].name;
    case NAME_TARGET_PARTY3:    return party_characters[3].name;
    case NAME_TARGET_PET:       return game_state.pet_name;
    case NAME_TARGET_FOOD:      return game_state.favourite_food;
    case NAME_TARGET_THING:     return game_state.favourite_thing + 4;
    case NAME_TARGET_M2_PLAYER: return game_state.mother2_playername;
    case NAME_TARGET_EB_PLAYER: return game_state.earthbound_playername;
    default:                    return NULL;
    }
}

/* The keyboard's battle-BG animation. TEXT_INPUT_DIALOG (text_input_dialog.asm:230)
 * draws each frame with RENDER_FRAME_TICK, whose overworld path
 * (render_frame_tick.asm:14-17) never touches the battle BG itself; the BG only
 * moves because RUN_ACTIONSCRIPT_FRAME runs EVENT_787 (EVENT_UPDATE_BATTLE_SCREEN_EFFECTS
 * every frame), which FILE_SELECT_INIT spawns (file_select_init.asm:56-63) and
 * deactivates on exit (:82-83). The port doesn't spawn EVENT_787 and calls
 * battle_bg_update() in its place, so do that only while the file menu (EVENT_787's
 * lifetime) is on the mode stack. The overworld name prompts (CC_1F_41 3/4 /
 * GAME_MODE_ENTER_NAME, e.g. Tony's call) have no EVENT_787, and a battle's BG
 * state left loaded there must not scroll the screen. The battle-mode branch of
 * RENDER_FRAME_TICK never applies: no keyboard runs in battle. */
static void text_input_bg_update(void) {
    for (int i = 0; i < g_mode_stack.depth; i++) {
        if (g_mode_stack.mode[i] == GAME_MODE_FILE_MENU) {
            battle_bg_update();
            return;
        }
    }
}

#ifdef EB_HAS_INPUT_SETTINGS
/* PC keyboard typing on the naming screen: any printable character the game's font has. */
static uint8_t kb_typed_to_eb(int c) {
    /* The game's font has glyphs the keyboard has no key for: the music note and the circle. They are
     * typed as [ and ]. */
    if (c == '[') return 0xAC;                        /* music note */
    if (c == ']') return 0xAF;                        /* circle */
    if (c == ' ') return 0x50;
    uint8_t eb = ascii_to_eb_char((char)c);
    return eb == 0x50 ? 0 : eb;                       /* characters the font lacks map to a space: reject */
}
#define TI_POP(v) (platform_text_input_enable(false), STEP_RESULT_POP(v))
#else
#define TI_POP(v) STEP_RESULT_POP(v)
#endif

/* GAME_MODE_TEXT_INPUT step — run-to-completion port of the text_input_dialog()
 * keyboard loop. Each step reads the input the pump's prior yield latched (when
 * `primed`), acts on it, then renders one frame; the pump owns the single yield.
 * See the mode header in mode_stack.h. */
StepResult mode_step_text_input(ModeState *st) {
    TextInputState *s = &st->text_input;
#ifdef EB_HAS_INPUT_SETTINGS
    platform_text_input_enable(true);                /* the PC keyboard types the name (cleared on every way out) */
#endif

    /* Resume after a parked actionscript frame (D4b): the child completed the
     * actionscript frame; run only this site's post-render tail and yield. */
    if (s->flush) {
        s->flush = 0;
        render_all_priority_sprites();
        sync_palettes_to_cgram();
        text_input_bg_update();
        s->primed = 1;
        return STEP_RESULT_CONTINUE();
    }

    WindowInfo *kw = get_window(WINDOW_FILE_SELECT_NAMING_KB);
    WindowInfo *nw = get_window(s->name_display_window_id);
    if (!kw || !nw) {
        close_window(WINDOW_FILE_SELECT_NAMING_KB);
        return TI_POP(-1);
    }

    /* --- Input handling (skipped on the first frame: no yield has happened
       yet, matching the blocking loop's render-before-first-read order) ---
     *
     * WINDOW_TICK starts with JSL RAND. `rand_ticks` counts the WINDOW_TICKs
     * this step's asm path runs (all just advance the RNG; the order between
     * them is irrelevant):
     *   - @RENDER_CURSOR's WINDOW_TICK (text_input_dialog.asm:167): on entry
     *     and after every action that jumps back to it (cursor move, character,
     *     Backspace, Don't Care, CAPITAL/small, an empty-name confirm).
     *   - @REDRAW_KEYBOARD's WINDOW_TICK_WITHOUT_INSTANT_PRINTING
     *     (text_input_dialog.asm:97) when the layout changes.
     *   - RENDER_KEYBOARD_INPUT_CHARACTER's text-speed delay
     *     (render_keyboard_input_character.asm:253): SELECTED_TEXT_SPEED + 1
     *     WINDOW_TICKs per inserted/deleted character. Instant printing is on
     *     there, so each exits right after the RAND without rendering. */
    uint16_t rand_ticks = 0;
    uint16_t typed_ticks = 0;                          /* RNG ticks for characters typed on the PC keyboard */
    const uint16_t char_ticks = (uint16_t)(dt.selected_text_speed + 1);
    if (s->relayout) {
        /* Second half of @REDRAW_KEYBOARD: the empty keyboard window has been
           shown for a frame; print the new layout. Input is next read after
           the following frame (@RENDER_CURSOR's WINDOW_TICK). */
        s->relayout = 0;
        set_instant_printing();
        kb_display_layout(s);
        clear_instant_printing();
    } else if (!s->primed) {
        rand_ticks = 1; /* @RENDER_CURSOR on entry */
    } else {
        uint16_t pressed = platform_input_get_pad_new();
        uint16_t held = core.pad1_autorepeat; /* PAD_HELD */
#ifdef EB_HAS_INPUT_SETTINGS
        const bool grid_hidden = true;      /* the desktop build has no letter grid: type the name */
        eb_hide_naming_grid = true;
#else
        const bool grid_hidden = false;
#endif

        /* Directions: the asm tests PAD_PRESS UP, LEFT, DOWN, RIGHT, then
           PAD_HELD (auto-repeat) UP, DOWN, LEFT, RIGHT, taking the first match
           and skipping the button checks. MOVE_CURSOR / FIND_NEXT_MENU_OPTION
           play the SFX only on a successful move; @UPDATE_CURSOR_POSITION
           always ends at @RENDER_CURSOR. */
        int dx = 0, dy = 0;
        if (pressed & PAD_UP)         dy = -1;
        else if (pressed & PAD_LEFT)  dx = -1;
        else if (pressed & PAD_DOWN)  dy = 1;
        else if (pressed & PAD_RIGHT) dx = 1;
        else if (held & PAD_UP)       dy = -1;
        else if (held & PAD_DOWN)     dy = 1;
        else if (held & PAD_LEFT)     dx = -1;
        else if (held & PAD_RIGHT)    dx = 1;

        /* `confirm_request` replaces the blocking loop's `goto confirm_name`. */
        bool confirm_request = false;
        bool dont_care_request = false;                    /* Tab on the PC keyboard */
#ifdef EB_HAS_INPUT_SETTINGS
        for (int typed; (typed = platform_text_input_get()) != 0; ) {
            if (typed == 9) {                         /* Tab: Don't Care */
                dont_care_request = true;
            } else if (typed == 13) {                 /* Enter / Return: done typing, like START */
                play_sfx(126);                        /* SFX::NAMING_CONFIRM_ALT */
                confirm_request = true;
            } else if (typed == 8) {                  /* Backspace: delete, but never leave the screen */
                if (s->name_pos > 0) {
                    play_sfx(125);                    /* SFX::NAMING_BACKSPACE */
                    s->name_pos--;
                    s->eb_name[s->name_pos] = 0;
                    typed_ticks += (uint16_t)(char_ticks + 1);
                }
            } else {
                uint8_t eb = kb_typed_to_eb(typed);
                if (eb && s->name_pos < s->max_len) {
                    play_sfx(122);                    /* SFX::TEXT_INPUT */
                    s->eb_name[s->name_pos] = eb;
                    s->name_pos++;
                    typed_ticks += (uint16_t)(char_ticks + 1);
                }
            }
        }
#endif

        if (!grid_hidden && (dx != 0 || dy != 0)) {
            int found = kb_find_next(kw, s->cur_x, s->cur_y, dx, dy);
            if (found >= 0) {
                s->cur_y = (found >> 8) & 0xFF;
                s->cur_x = found & 0xFF;
                play_sfx(dy != 0 ? 124 : 123); /* NAMING_CURSOR_VERTICAL / horizontal */
            }
            rand_ticks = 1;
        } else if (grid_hidden && (pressed & PAD_CONFIRM)) {
            /* A: bring up the system's on-screen keyboard (handhelds, touch); a PC keyboard just types. */
            platform_text_input_request_osk();
            rand_ticks = 1;
        } else if (grid_hidden && ((pressed & PAD_X) || dont_care_request) && s->has_dont_care) {
            /* Tab or X: Don't Care — cycle the preset names, as the grid's button did. */
            play_sfx(122); /* SFX::TEXT_INPUT */
            if (s->dont_care_row >= 6) s->dont_care_row = 0; else s->dont_care_row++;
            const uint8_t *eb_src = get_dont_care_name(s->naming_index, s->dont_care_row);
            memset(s->eb_name, 0, sizeof(s->eb_name));
            s->name_pos = 0;
            for (int i = 0; i < DONT_CARE_NAME_SIZE && i < (int)s->max_len && eb_src[i] != 0; i++) {
                s->eb_name[i] = eb_src[i];
                s->name_pos++;
            }
            rand_ticks = (uint16_t)(1 + char_ticks * s->name_pos);
        } else if (pressed & PAD_CONFIRM) {
            /* A/L button: select */
            if (s->cur_y == 6) {
                /* Row 6: buttons */
                if (s->cur_x == 0 && s->has_dont_care) {
                    /* Don't Care — cycle through preset names (assembly: @DONTCARE_SELECTED).
                       Counter starts at -1; on each press: if >= 6 reset to 0, else increment.
                       Then load dont_care_names[naming_index][row] into eb_name. */
                    play_sfx(122); /* SFX::TEXT_INPUT */
                    if (s->dont_care_row >= 6)
                        s->dont_care_row = 0;
                    else
                        s->dont_care_row++;
                    const uint8_t *eb_src = get_dont_care_name(s->naming_index, s->dont_care_row);
                    memset(s->eb_name, 0, sizeof(s->eb_name));
                    s->name_pos = 0;
                    for (int i = 0; i < DONT_CARE_NAME_SIZE && i < (int)s->max_len && eb_src[i] != 0; i++) {
                        s->eb_name[i] = eb_src[i];
                        s->name_pos++;
                    }
                    /* RENDER_DONT_CARE_NAME: one RENDER_KEYBOARD_INPUT_CHARACTER
                       per character, then @RENDER_CURSOR. */
                    rand_ticks = (uint16_t)(1 + char_ticks * s->name_pos);
                } else if (s->cur_x == 17) {
                    /* Backspace (@BACKSPACE_SELECTED): an empty name returns to
                       the previous screen, like B. */
                    play_sfx(122); /* SFX::TEXT_INPUT */
                    if (s->name_pos > 0) {
                        s->name_pos--;
                        s->eb_name[s->name_pos] = 0;
                        rand_ticks = (uint16_t)(char_ticks + 1);
                    } else if (s->naming_index != -1) {
                        close_window(WINDOW_FILE_SELECT_NAMING_KB);
                        return TI_POP(-1);
                    } else {
                        rand_ticks = 1;
                    }
                } else if (s->cur_x == 25) {
                    /* OK */
                    play_sfx(94); /* SFX::NAMING_CONFIRM */
                    confirm_request = true;
                }
            } else if (s->cur_y == 4) {
                /* Row 4: CAPITAL/small toggle or punctuation */
                play_sfx(122); /* SFX::TEXT_INPUT */
                if (s->cur_x == 0 || s->cur_x == 7) {
                    /* @SELECT_CAPITAL / @SELECT_SMALL -> @REDRAW_KEYBOARD: only
                       a changed layout re-creates the keyboard window, shows it
                       empty for one frame (WINDOW_TICK_WITHOUT_INSTANT_PRINTING)
                       and then prints the new layout (next step). */
                    bool lower = (s->cur_x == 7);
                    rand_ticks = 1;
                    if (lower != (bool)s->is_lowercase) {
                        s->is_lowercase = lower;
                        create_window(WINDOW_FILE_SELECT_NAMING_KB);
                        s->relayout = 1;
                        rand_ticks = 2;
                    }
                } else {
                    /* Punctuation at column 11/12 */
                    int col = kb_text_x_to_grid_col(s->cur_x);
                    const uint8_t (*grid)[KB_GRID_COLS] = s->is_lowercase ? kb_lower_grid : kb_upper_grid;
                    rand_ticks = 1;
                    if (col >= 0 && grid[4][col] != 0xFF && s->name_pos < s->max_len) {
                        s->eb_name[s->name_pos] = grid[4][col];
                        s->name_pos++;
                        rand_ticks += char_ticks;
                    }
                }
            } else if (s->cur_y >= 0 && s->cur_y <= 3) {
                /* Character rows 0-3 */
                play_sfx(122); /* SFX::TEXT_INPUT */
                int col = kb_text_x_to_grid_col(s->cur_x);
                const uint8_t (*grid)[KB_GRID_COLS] = s->is_lowercase ? kb_lower_grid : kb_upper_grid;
                rand_ticks = 1;
                if (col >= 0 && grid[s->cur_y][col] != 0xFF && s->name_pos < s->max_len) {
                    s->eb_name[s->name_pos] = grid[s->cur_y][col];
                    s->name_pos++;
                    rand_ticks += char_ticks;
                }
            }
        } else if (pressed & PAD_CANCEL) {
            /* B/SELECT: backspace (assembly: @CHECK_B_SELECT) */
            play_sfx(125); /* SFX::NAMING_BACKSPACE */
            if (s->name_pos > 0) {
                s->name_pos--;
                s->eb_name[s->name_pos] = 0;
                rand_ticks = (uint16_t)(char_ticks + 1);
            } else if (s->naming_index != -1) {
                /* No characters and not standalone — go back to previous screen.
                   Assembly only closes the keyboard window; name box and message
                   are left for the caller. */
                close_window(WINDOW_FILE_SELECT_NAMING_KB);
                return TI_POP(-1);
            } else {
                rand_ticks = 1;
            }
        } else if (pressed & PAD_START) {
            /* START: confirm */
            play_sfx(126); /* SFX::NAMING_CONFIRM_ALT */
            confirm_request = true;
        }

        /* Only confirm if at least one character entered
           (assembly: @CONFIRM_NAME checks STRLEN of KEYBOARD_INPUT_CHARACTERS;
           an empty name goes back to @RENDER_CURSOR) */
        if (confirm_request) {
            if (s->name_pos == 0) {
                rand_ticks = 1;
            } else {
                uint8_t *name_buf = name_target_buffer(s->name_target);
                if (name_buf) {
                    memset(name_buf, 0, (size_t)s->max_len);
                    for (int i = 0; i < (int)s->name_pos && i < (int)s->max_len; i++)
                        name_buf[i] = s->eb_name[i];
                }
                close_window(WINDOW_FILE_SELECT_NAMING_KB);
                return TI_POP(0);
            }
        }
    }
    rand_ticks += typed_ticks;
    while (rand_ticks--)
        rng_next_byte();

    /* --- Render one frame (no yield; owned by the pump) --- */

    /* Reset the per-frame VWF pool (window titles re-render from it in
       render_all_windows) and keep it clear of the name display tiles at
       0x2E0+.  Each name character uses 2 VRAM tiles (upper+lower). */
    vwf_frame_reset();
    vwf_reserve_tiles(s->max_len * 2);

    /* Render all windows: borders plus each window's content tilemap, which
       holds the prompt text and the keyboard layout printed by
       kb_display_layout(). This clears the tilemap. */
    render_all_windows();

    /* Upload the name display tiles to 0x2E0+ and write their tilemap entries
       (RENDER_KEYBOARD_INPUT_CHARACTER).  render_all_windows() rebuilds the
       tilemap, so the entries are rewritten every frame. */
    {
        uint8_t display[32];
        kb_build_name_display(display, s->eb_name, s->name_pos, s->max_len);
        int name_tile_cols = kb_render_name_tiles(display, s->max_len);
        kb_write_name_tilemap_at(nw, name_tile_cols, s->name_text_y);
    }

    /* Draw blinking cursor matching selection_menu() two-frame animation:
       Frame 0 (big):  tiles 0x41/0x51  — CURSOR_FRAME0 from TEXT_WINDOW_GFX
       Frame 1 (small): tiles 0x28D/0x29D — CURSOR_FRAME1 from TEXT_WINDOW_GFX
       Toggle every 10 frames; always draw one frame (never skip). */
    {
        bool cursor_frame = ((s->frame_counter / 10) % 2 != 0);
        uint16_t *tilemap = (uint16_t *)win.bg2_buffer;
        uint16_t mx = kw->content_x + (uint16_t)s->cur_x;
        uint16_t my = kw->content_y + (uint16_t)(s->cur_y * 2);
        if (mx < 32 && my + 1 < 32 && !eb_hide_naming_grid) {
            if (!cursor_frame) {
                tilemap[my * 32 + mx]       = 0x2441; /* tile 0x41, pal 1, pri */
                tilemap[(my + 1) * 32 + mx] = 0x2451; /* tile 0x51, pal 1, pri */
            } else {
                tilemap[my * 32 + mx]       = 0x268D; /* tile 0x28D, pal 1, pri */
                tilemap[(my + 1) * 32 + mx] = 0x269D; /* tile 0x29D, pal 1, pri */
            }
        }
    }
    s->frame_counter++;

    /* Sync win.bg2_buffer to VRAM (NMI DMA equivalent) */
    upload_battle_screen_to_vram();

    /* Run entity scripts for naming screen animations. A parked actionscript
     * frame STEP_PUSHes ACTIONSCRIPT_FRAME; the post-render tail runs on its pop
     * (the flush block at the top of this step). */
    oam_clear();
    if (run_actionscript_frame_step()) {
        s->flush = 1;
        return actionscript_frame_take_push();
    }
    render_all_priority_sprites();

    /* Sync palette mirror to CGRAM (NMI handler does this in ROM) */
    sync_palettes_to_cgram();

    /* Battle-BG animation (EVENT_787 stand-in; see text_input_bg_update) */
    text_input_bg_update();

    s->primed = 1;
    return STEP_RESULT_CONTINUE();
}

/* Create the keyboard window and seed a TextInputState for GAME_MODE_TEXT_INPUT.
 * Shared by the modes that STEP_PUSH GAME_MODE_TEXT_INPUT directly
 * (GAME_MODE_NEW_GAME_NAMING and GAME_MODE_ENTER_NAME, the M2/EB player-name
 * registry / debug Player 0/1). Any existing-name pre-fill is folded into the seed
 * so the existing_name pointer never enters the POD ModeState. */
void text_input_prepare(ModeState *init, int name_target, int max_len,
                        int naming_index, uint16_t name_display_window_id,
                        int name_text_y, const uint8_t *existing_name)
{
    /* Create keyboard window (assembly: TEXT_INPUT_DIALOG line 43) */
    create_window(WINDOW_FILE_SELECT_NAMING_KB);
    win.current_focus_window = WINDOW_FILE_SELECT_NAMING_KB;

    *init = (ModeState){0};
    TextInputState *s = &init->text_input;
    s->primed = 0;
    s->name_target = (uint8_t)name_target;
    s->naming_index = (int16_t)naming_index;
    s->has_dont_care = (naming_index >= 0);
    s->is_lowercase = false;
    /* Don't Care row counter — starts at -1, incremented to 0 on first press,
       then cycles 0→1→2→3→4→5→6→0→... (assembly: @LOCAL0A) */
    s->dont_care_row = -1;
    s->name_pos = 0;
    s->max_len = (uint16_t)max_len;
    s->name_display_window_id = name_display_window_id;
    s->name_text_y = (int16_t)name_text_y;
    s->cur_x = 0;
    s->cur_y = 0;
    s->frame_counter = 0;

    /* Pre-fill from existing name if provided (eb_name zeroed above). */
    if (existing_name) {
        for (int i = 0; i < max_len && i < (int)sizeof(s->eb_name) && existing_name[i] != 0; i++) {
            s->eb_name[i] = existing_name[i];
            s->name_pos++;
        }
    }

    /* Print the keyboard layout into the new keyboard window
       (text_input_dialog.asm:42-91: SET_INSTANT_PRINTING, DISPLAY_TEXT of the
       button and key layouts; @RENDER_CURSOR then CLEAR_INSTANT_PRINTING). */
    set_instant_printing();
    kb_display_layout(s);
    clear_instant_printing();
}

/* GAME_MODE_NAMING_PROMPT step — run-to-completion port of the name_a_character()
 * prompt-wait loop. Renders the name box + prompt each frame; the pump owns the
 * yield. `primed` reproduces the blocking loop's render-before-first-read. */
StepResult mode_step_naming_prompt(ModeState *st) {
    NamingPromptState *s = &st->naming_prompt;

    /* Resume after a parked actionscript frame (D4b): the child completed the
     * actionscript frame; run only the post-render tail and yield. */
    if (s->flush) {
        s->flush = 0;
        render_all_priority_sprites();
        sync_palettes_to_cgram();
        battle_bg_update();
        s->primed = 1;
        return STEP_RESULT_CONTINUE();
    }

    if (s->primed) {
        if (platform_input_get_pad_new() & PAD_ANY_BUTTON)
            return STEP_RESULT_POP(0);
    }

    vwf_frame_reset();
    render_all_windows();
    {
        WindowInfo *nw = get_window(WINDOW_FILE_SELECT_NAMING_BOX);
        if (nw) kb_write_name_tilemap(nw, s->name_tile_cols);
    }
    upload_battle_screen_to_vram();
    oam_clear();
    if (run_actionscript_frame_step()) {
        s->flush = 1;
        return actionscript_frame_take_push();
    }
    render_all_priority_sprites();
    sync_palettes_to_cgram();
    battle_bg_update();

    s->primed = 1;
    return STEP_RESULT_CONTINUE();
}

/*
 * RENDER_FRAME_TICK (C1004E.asm) — overworld path, naming-screen variant.
 * Executes one frame's WORK (OAM clear, run scripts, draw, sync) WITHOUT the
 * trailing wait_for_vblank() — the yield belongs to the mode pump (the single
 * host_process_frame() yield), per the savestate run-to-completion model.
 * Used by GAME_MODE_NAMING_EVENTS (init_naming_screen_events).
 */
/* Post-render tail shared by render_frame_tick_naming_work_step()'s no-park path
 * and the flush resume in mode_step_naming_events. */
static void render_frame_tick_naming_work_flush(void) {
    /* Flush queued sprites to OAM */
    render_all_priority_sprites();

    /* NMI equivalent: win.bg2_buffer sync + palette sync + BG animation */
    upload_battle_screen_to_vram();
    sync_palettes_to_cgram();
    battle_bg_update();
}

/* Run-to-completion form of render_frame_tick_naming() (savestate D4b): a parked
 * actionscript callroutine returns true so the caller STEP_PUSHes
 * GAME_MODE_ACTIONSCRIPT_FRAME (actionscript_frame_take_push) and runs the tail at
 * its flush resume. On no park it completes the frame inline and returns false. */
static bool render_frame_tick_naming_work_step(void) {
    /* OAM_CLEAR — hide all sprites and reset priority queues */
    oam_clear();

    /* RUN_ACTIONSCRIPT_FRAME (entity script execution + draw list) */
    if (run_actionscript_frame_step())
        return true;

    render_frame_tick_naming_work_flush();
    return false;
}

/*
 * INIT_NAMING_SCREEN_EVENTS (C4D830.asm)
 *
 * After a character has been named, assigns return animation scripts to the
 * walk-in entities and waits for all entity scripts to complete.
 *
 * 1. Wait for ert.wait_for_naming_screen_actionscript == 0
 * 2. Read return animation entity list (entry 7 + naming_index)
 * 3. For each entity: find by sprite_id, reassign to return animation script
 * 4. Wait for all entity scripts (slots 0 to PARTY_LEADER_ENTITY_INDEX-2) to finish
 */
/* Reassign the walk-out animation scripts (assembly lines 17-62): read the
 * return-animation entity list at entry 7+naming_index and, for each
 * (sprite_id, script_id) pair, find the live entity and swap its script.
 * Returns false on the early-out conditions (no data / bad pointer), which the
 * blocking original handled with a bare `return` (skipping the wait + cleanup). */
static bool naming_events_reassign_scripts(uint16_t naming_index) {
    uint16_t entry = 7 + naming_index;
    if (!naming_entities_data || entry >= NAMING_SCREEN_ENTITY_COUNT)
        return false;

    uint32_t ptr_off = NAMING_ENTITIES_PTR_TABLE_OFF + (uint32_t)entry * 4;
    if (ptr_off + 4 > naming_entities_data_size)
        return false;

    uint32_t ptr = read_u32_le(&naming_entities_data[ptr_off]);

    uint16_t within_bank = (uint16_t)(ptr & 0xFFFF);
    uint16_t buf_off = within_bank - NAMING_ENTITIES_ROM_BASE;

    /* Walk entity list: .WORD sprite_id, .WORD script_id, terminated by 0. */
    while (buf_off + 2 <= naming_entities_data_size) {
        uint16_t sprite_id = read_u16_le(&naming_entities_data[buf_off]);
        if (sprite_id == 0)
            break;  /* Terminator */

        uint16_t script_id = read_u16_le(&naming_entities_data[buf_off + 2]);

        int16_t slot = find_entity_by_sprite_id(sprite_id);
        if (slot >= 0) {
            reassign_entity_script(slot, script_id);
        }

        buf_off += 4;
    }
    return true;
}

/* GAME_MODE_NAMING_EVENTS step — run-to-completion port of the two
 * render_frame_tick_naming() wait loops in init_naming_screen_events(). See the
 * mode header comment in mode_stack.h for the phase/timing rationale. */
StepResult mode_step_naming_events(ModeState *st) {
    NamingEventsState *s = &st->naming_events;

    /* Resume after a parked actionscript frame (D4b): the child completed the
     * actionscript frame; run the post-render tail of whichever site parked.
     * NE_WAIT_SCRIPTS's `done` was already decided before the push. */
    if (s->flush == 1) {        /* NE_WAIT_PENDING render parked */
        s->flush = 0;
        render_frame_tick_naming_work_flush();
        return STEP_RESULT_CONTINUE();
    }
    if (s->flush == 2) {        /* NE_WAIT_SCRIPTS render parked */
        s->flush = 0;
        render_frame_tick_naming_work_flush();
        return STEP_RESULT_CONTINUE();
    }

    if (s->phase == NE_WAIT_PENDING) {
        /* Wait for any pending actionscript to complete (assembly lines 12-16).
         * Check-before: render+yield while it is still pending. */
        if (ert.wait_for_naming_screen_actionscript != 0) {
            if (render_frame_tick_naming_work_step()) {
                s->flush = 1;
                return actionscript_frame_take_push();
            }
            return STEP_RESULT_CONTINUE();
        }
        /* Pending cleared: reassign the walk-out scripts inline (no yield). An
         * early-out skips the wait + cleanup, exactly like the blocking return. */
        if (!naming_events_reassign_scripts(s->naming_index))
            return STEP_RESULT_POP(0);
        s->phase = NE_WAIT_SCRIPTS;
        s->done = 0;
        /* Fall through into the first NE_WAIT_SCRIPTS frame (no extra yield),
         * matching the blocking code's immediate entry into loop 2. */
    }

    /* NE_WAIT_SCRIPTS: wait for all entity scripts (slots 0 ..
     * PARTY_LEADER_ENTITY_INDEX-2) to finish (script_table == -1 for all).
     * The blocking loop renders once more than strictly needed (it computes the
     * AND before the render and breaks after); `done` reproduces that — the
     * final render's yield happens, then the next step cleans up and pops. */
    if (s->done) {
        /* All entities are now deactivated.  The assembly achieves a clean VRAM
         * and spritemap state via per-entity DEALLOCATE_ENTITY_SPRITE calls in
         * the walk-out scripts.  As a safety net, do a bulk clear here so the
         * next naming screen starts with a clean slate. */
        memset(sprite_vram_table, 0, sizeof(sprite_vram_table));
        clear_overworld_spritemaps();
        return STEP_RESULT_POP(0);
    }

    int16_t result = -1;
    for (int slot = 0; slot < PARTY_LEADER_ENTITY_INDEX - 1; slot++) {
        result &= entities.script_table[ENT(slot)];
    }
    if (render_frame_tick_naming_work_step()) {
        /* `done` is decided from `result` (computed before the render) regardless
         * of the park, so the flush resume needs no extra state. */
        if (result == -1)
            s->done = 1;
        s->flush = 2;
        return actionscript_frame_take_push();
    }
    if (result == -1)
        s->done = 1;
    return STEP_RESULT_CONTINUE();
}

/*
 * Create animated party sprites on the confirmation screen.
 * Port of CREATE_FILE_SELECT_PARTY_SPRITES (C4D8FA.asm).
 * Spawns 5 entities (Ness, Paula, Jeff, Poo, King) next to their name windows.
 */
static void create_file_select_party_sprites(void) {
    static const struct {
        uint16_t sprite;
        uint16_t script;
        int16_t x, y;
    } configs[] = {
        {   1, 861,  40,  44 },  /* Ness */
        {   2, 861,  40,  76 },  /* Paula */
        {   3, 861,  40, 108 },  /* Jeff */
        {   4, 861,  40, 140 },  /* Poo */
        { 359, 534, 136,  40 },  /* King (sleeping dog) */
    };
    for (int i = 0; i < 5; i++) {
        int16_t ent = create_entity(configs[i].sprite, configs[i].script, -1,
                                     configs[i].x, configs[i].y);
        if (ent >= 0)
            entities.directions[ENT(ent)] = 4; /* DIRECTION::DOWN */
    }
}

/*
 * Compute the pixel width of an EB-encoded text string.
 * Port of GET_TEXT_PIXEL_WIDTH (C44FF3.asm).
 * Sums (char_width + CHARACTER_PADDING) for each character.
 */
static uint16_t get_text_pixel_width(uint8_t font_id, const uint8_t *eb_text, int len) {
    uint16_t total = 0;
    for (int i = 0; i < len; i++) {
        uint8_t char_index = (eb_text[i] - 0x50) & 0x7F;
        total += font_get_width(font_id, char_index) + 1; /* +1 = CHARACTER_PADDING */
    }
    return total;
}

/*
 * Print an EB-encoded name right-justified on row 1 of the focus window.
 * Port of the right-justification algorithm in file_select_menu_loop.asm (lines 338-382).
 *
 * Algorithm:
 *   1. pixel_width = GET_TEXT_PIXEL_WIDTH(font=0, eb_text, strlen)
 *   2. width_tiles = pixel_width / 8
 *   3. remainder = pixel_width % 8
 *   4. If remainder != 0 OR width_tiles == 6: width_tiles += 1
 *   5. cursor_x = window_width - width_tiles
 *   6. SET_FOCUS_TEXT_CURSOR(cursor_x, 1)
 */
static void print_right_justified_name(const uint8_t *eb_name, int len,
                                        uint16_t window_id) {
    /* Find string length (stop at null) */
    int slen = 0;
    while (slen < len && eb_name[slen] != 0) slen++;

    uint16_t pixel_width = get_text_pixel_width(FONT_ID_NORMAL, eb_name, slen);
    uint16_t width_tiles = pixel_width / 8;
    uint16_t remainder = pixel_width % 8;

    if (remainder != 0 || width_tiles == 6)
        width_tiles++;

    WindowInfo *w = get_window(window_id);
    if (!w) return;

    /* Assembly (create_window.asm) subtracts 2 from config width for borders.
       w->width in C is the raw config value; subtract 2 for interior width. */
    uint16_t interior_width = w->width - 2;
    uint16_t cursor_x = interior_width - width_tiles;
    set_focus_text_cursor(cursor_x, 1);

    /* Print the EB-encoded name directly (no lossy ASCII round-trip) */
    int print_len = slen < 15 ? slen : 15;
    print_eb_string(eb_name, print_len);
}

/* Per-thing naming table: NameTargetId + max length, in naming order
 * (Ness/Paula/Jeff/Poo, pet, favourite food, favourite thing). */
static const struct {
    int target;   /* NameTargetId (resolved to a global buffer on confirm) */
    int max_len;
} ng_name_targets[THINGS_NAMED_COUNT] = {
    { NAME_TARGET_PARTY0, NAME_MAX_CHARS }, { NAME_TARGET_PARTY1, NAME_MAX_CHARS },
    { NAME_TARGET_PARTY2, NAME_MAX_CHARS }, { NAME_TARGET_PARTY3, NAME_MAX_CHARS },
    { NAME_TARGET_PET,    NAME_MAX_CHARS }, { NAME_TARGET_FOOD,   NAME_MAX_CHARS },
    { NAME_TARGET_THING,  NAME_MAX_CHARS },
};

/*
 * GAME_MODE_NEW_GAME_NAMING step — run-to-completion port of new_game_naming().
 * See NewGameNamingState in mode_stack.h for the phase machine. Folds in the
 * former blocking helpers name_a_character() and init_naming_screen_events().
 */
StepResult mode_step_new_game_naming(ModeState *ms) {
    NewGameNamingState *st = &ms->new_game_naming;

    /* Self-heal the file-select asset pointers: a savestate restored into this
     * mode may not have run file_menu_setup() (covers naming_prompts_data +
     * initial_stats_data; get_dont_care_name resolves its own). */
    ensure_file_select_assets();

    for (;;) {
        switch ((NewGameNamingPhase)st->phase) {
        case NGN_ENTER:
            /* Matching assembly @CHANGE_TO_NAMING_SCREEN_MUSIC → @UNKNOWN18:
               change music, close all windows, load naming-screen assets, then
               init the entity system once before the naming loop. */
            change_music(2); /* MUSIC::NAMING_SCREEN */
            close_all_windows();
            load_event_script_data();
            load_sprite_data();
            entity_system_init();
            clear_overworld_spritemaps();
            memset(sprite_vram_table, 0, sizeof(sprite_vram_table));
            st->char_i = 0;
            st->phase = NGN_LOOP_HEAD;
            continue;

        case NGN_LOOP_HEAD: {
            if (platform_input_quit_requested())
                return STEP_RESULT_POP(0);
            if (st->char_i >= THINGS_NAMED_COUNT) {
                st->phase = NGN_CONFIRM_BUILD;
                continue;
            }
            int i = st->char_i;

            /* Create animated walk-in entities for this naming screen
               (DISPLAY_ANIMATED_NAMING_SPRITE). */
            display_animated_naming_sprite((uint16_t)i);

            /* name_a_character() front (assembly name_a_character.asm):
               create the name box, then the message window + prompt, then the
               initial name display (bullet + dashes), then wait for a button
               via GAME_MODE_NAMING_PROMPT. */
            create_window(WINDOW_FILE_SELECT_NAMING_BOX);
            WindowInfo *msg_w = create_window(WINDOW_FILE_SELECT_NAMING_MSG);
            if (msg_w) {
                set_focus_text_cursor(0, 0);
                print_eb_string(naming_prompts_data + i * NAMING_PROMPT_ENTRY_SIZE,
                                NAMING_PROMPT_ENTRY_SIZE);
            }
            int name_tile_cols;
            {
                uint8_t empty_name[16];
                uint8_t display[16];
                memset(empty_name, 0, sizeof(empty_name));
                kb_build_name_display(display, empty_name, 0, ng_name_targets[i].max_len);
                name_tile_cols = kb_render_name_tiles(display, ng_name_targets[i].max_len);
            }
            /* name_a_character.asm:33/121 run WINDOW_TICK_WITHOUT_INSTANT_PRINTING
               after creating each window, and CC_13_14 (the prompt wait) starts
               with a WINDOW_TICK; each begins with JSL RAND. The prompt mode's
               first frame stands in for CC_13_14's render. */
            rng_next_byte();
            rng_next_byte();
            rng_next_byte();
            memset(&g_mode_push_init, 0, sizeof(g_mode_push_init));
            g_mode_push_init.naming_prompt.name_tile_cols = (int16_t)name_tile_cols;
            st->phase = NGN_PROMPT_DONE;
#ifdef EB_HAS_INPUT_SETTINGS
            /* No letter grid to wait for: the name box takes typed input as soon as it is on screen,
             * so skip the "press a button to bring up the keyboard" pause. */
            continue;
#else
            return STEP_RESULT_PUSH_INIT(GAME_MODE_NAMING_PROMPT, &g_mode_push_init);
#endif
        }

        case NGN_PROMPT_DONE: {
            /* name_a_character() step 5: the on-screen keyboard. Name display
               goes in the name box window at text row 0. */
            int i = st->char_i;
            text_input_prepare(&g_mode_push_init, ng_name_targets[i].target,
                               ng_name_targets[i].max_len, i,
                               WINDOW_FILE_SELECT_NAMING_BOX, 0, NULL);
            st->phase = NGN_INPUT_DONE;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_TEXT_INPUT, &g_mode_push_init);
        }

        case NGN_INPUT_DONE: {
            st->char_result = (int16_t)mode_child_result();
            /* Assembly (@NAMING_ADVANCE): ALWAYS run INIT_NAMING_SCREEN_EVENTS
               for the current index BEFORE advancing — the return animation
               (entities walk off) + wait for all entity scripts to finish. */
            memset(&g_mode_push_init, 0, sizeof(g_mode_push_init));
            g_mode_push_init.naming_events.phase = NE_WAIT_PENDING;
            g_mode_push_init.naming_events.naming_index = (uint16_t)st->char_i;
            st->phase = NGN_EVENTS_DONE;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_NAMING_EVENTS, &g_mode_push_init);
        }

        case NGN_EVENTS_DONE:
            if (st->char_result == 0) {
                st->char_i++;             /* confirmed — advance */
            } else {                      /* cancelled (-1) — go back */
                if (st->char_i > 0) {
                    st->char_i--;
                } else {
                    /* Back past the first character (@NAMING_NEXT_CHARACTER,
                       index == $FFFF): return to flavour. The asm's single
                       CLOSE_ALL_WINDOWS there (and its WINDOW_TICK's RAND)
                       runs in the parent's FM_NG_NAMING_RESULT. */
                    return STEP_RESULT_POP(0);
                }
            }
            st->phase = NGN_LOOP_HEAD;
            continue;

        case NGN_CONFIRM_BUILD: {
            /* Set default names for any empty slots (first name in each group).
             * Names are already EB-encoded in the ROM data — copy directly. */
            for (int c = 0; c < 4; c++) {
                if (party_characters[c].name[0] == 0) {
                    const uint8_t *eb_src = get_dont_care_name(c, 0);
                    for (int j = 0; j < DONT_CARE_NAME_SIZE && eb_src[j] != 0; j++)
                        party_characters[c].name[j] = eb_src[j];
                }
            }
            if (game_state.pet_name[0] == 0) {
                const uint8_t *eb_src = get_dont_care_name(4, 0);
                for (int j = 0; j < 6 && eb_src[j] != 0; j++)
                    game_state.pet_name[j] = eb_src[j];
            }
            if (game_state.favourite_food[0] == 0) {
                const uint8_t *eb_src = get_dont_care_name(5, 0);
                for (int j = 0; j < 6 && eb_src[j] != 0; j++)
                    game_state.favourite_food[j] = eb_src[j];
            }
            if (game_state.favourite_thing[4] == 0) {
                const uint8_t *eb_src = get_dont_care_name(6, 0);
                for (int j = 0; j < 6 && eb_src[j] != 0; j++)
                    game_state.favourite_thing[4 + j] = eb_src[j];
            }

            /* Set "PSI " prefix on favourite_thing */
            game_state.favourite_thing[0] = ascii_to_eb_char('P');
            game_state.favourite_thing[1] = ascii_to_eb_char('S');
            game_state.favourite_thing[2] = ascii_to_eb_char('I');
            game_state.favourite_thing[3] = ascii_to_eb_char(' ');

            /* Show confirmation (CLOSE_ALL_WINDOWS, SET_INSTANT_PRINTING, then
             * confirmation windows). All naming entities are already
             * deactivated by GAME_MODE_NAMING_EVENTS. */
            close_all_windows();
            set_instant_printing();

            /* Display all names for confirmation (print EB-encoded directly) */
            for (int c = 0; c < 4; c++) {
                WindowInfo *cw = create_window((uint16_t)(WINDOW_FILE_SELECT_CONFIRM_NESS + c));
                if (cw) {
                    set_focus_text_cursor(0, 0);
                    int len = 0;
                    while (len < NAME_MAX_CHARS && party_characters[c].name[len] != 0x00) len++;
                    print_eb_string(party_characters[c].name, len);
                }
            }

            /* Dog name */
            WindowInfo *kw = create_window(WINDOW_FILE_SELECT_CONFIRM_KING);
            if (kw) {
                set_focus_text_cursor(0, 0);
                int len = 0;
                while (len < NAME_MAX_CHARS && game_state.pet_name[len] != 0x00) len++;
                print_eb_string(game_state.pet_name, len);
            }

            /* Favorite food — label on row 0, right-justified value on row 1 */
            create_window(WINDOW_FILE_SELECT_CONFIRM_FOOD);
            set_focus_text_cursor(0, 0);
            print_string("Favorite food:");
            print_right_justified_name(game_state.favourite_food, NAME_MAX_CHARS, WINDOW_FILE_SELECT_CONFIRM_FOOD);

            /* Favorite thing — same layout as food */
            create_window(WINDOW_FILE_SELECT_CONFIRM_THING);
            set_focus_text_cursor(0, 0);
            print_string("Coolest thing:");
            print_right_justified_name(game_state.favourite_thing + 4, NAME_MAX_CHARS, WINDOW_FILE_SELECT_CONFIRM_THING);

            /* Are you sure? */
            WindowInfo *mw = create_window(WINDOW_FILE_SELECT_CONFIRM_MSG);
            if (mw) {
                set_focus_text_cursor(0, 0);
                print_string("Are you sure?");
                /* Horizontal layout: Yep at text_x=14, Nope at text_x=18. */
                add_menu_item("Yep", 1, 14, 0);
                add_menu_item("Nope", 0, 18, 0);
            }

            /* PRINT_MENU_ITEMS then CREATE_FILE_SELECT_PARTY_SPRITES — no
             * RENDER_ALL_WINDOWS in between (file_select_menu_loop.asm:470-471). */
            print_menu_items();
            create_file_select_party_sprites();

            /* SELECTION_MENU(1): B enabled, returns 0 (same as "Nope"). */
            memset(&g_mode_push_init, 0, sizeof(g_mode_push_init));
            g_mode_push_init.selection_menu.phase        = SM_SETUP;
            g_mode_push_init.selection_menu.allow_cancel = 1;
            st->phase = NGN_CONFIRM_DONE;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_SELECTION_MENU, &g_mode_push_init);
        }

        case NGN_CONFIRM_DONE: {
            uint16_t confirm = (uint16_t)mode_child_result();
            if (confirm == 0) {
                /* Nope — clear entities and redo naming from the top (the
                   original tail-recursion `return new_game_naming()`). */
                entity_system_init();
                close_all_windows();
                *st = (NewGameNamingState){ .phase = NGN_ENTER };
                continue;
            }
            /* Yep */
            change_music(158); /* MUSIC::NAME_CONFIRMATION */
            /* file_select_menu_loop.asm:483: WINDOW_TICK (JSL RAND first; the
               windows are already drawn), then 180 RENDER_FRAME_TICKs. The
               tick's frame is the 181st wait frame. */
            rng_next_byte();
            st->wait_frames = 181;
            st->phase = NGN_CONFIRM_WAIT;
            continue;
        }

        case NGN_CONFIRM_WAIT:
            /* Wait for the confirmation animation (assembly 487-497: 180-frame
               loop; the C original was wait_frames_or_button(180, 0) — no game
               work, just yields). */
            if (st->wait_frames > 0) {
                st->wait_frames--;
                return STEP_RESULT_CONTINUE();
            }
            st->phase = NGN_FINALIZE;
            continue;

        case NGN_FINALIZE:
            break; /* fall out of the switch into the finalize body below */
        }

        break; /* NGN_FINALIZE reached: run the synchronous finalize, then POP */
    }

    /* ---- NGN_FINALIZE: initialise the new game (synchronous) ---------------- */

    /* Clear map entities after animation (assembly line 498) */
    entity_system_init();

    /* Initialize character stats from INITIAL_STATS (asm/data/initial_stats.asm).
     * Assembly: file_select_menu_loop.asm lines 501-576.
     * For each character:
     *   1. RESET_CHAR_LEVEL_ONE(char_id, level, 0) — sets base stats and levels up
     *   2. GAIN_EXP if initial EXP > 0
     *   3. Copy max HP/PP → current (sync after level-ups)
     *   4. Clear items and set from INITIAL_STATS */

    /* Initialize all 4 characters from INITIAL_STATS ROM data.
     * Per-entry layout: coords(4), money(2), level(2), EXP(2), items(10) = 20 bytes. */
    for (int c = 0; c < 4; c++) {
        const uint8_t *entry = initial_stats_data + c * INITIAL_STATS_ENTRY_SIZE;
        uint16_t level = read_u16_le(entry + 6);
        uint16_t exp = read_u16_le(entry + 8);

        reset_char_level_one(c + 1, level, 0);
        if (exp > 0) gain_exp(0, c + 1, exp);

        memset(party_characters[c].items, ITEM_NONE, sizeof(party_characters[c].items));
        for (int j = 0; j < 10; j++) {
            uint8_t item = entry[10 + j];
            if (item != 0) party_characters[c].items[j] = item;
        }
    }

    /* New Game+: the cleared game's levels, stats and non-key items replace the fresh ones. */
    ng_plus_clear_slot((int)current_save_slot - 1);
    ng_plus_apply_new_game();

    /* Sync current HP/PP = max for all characters (assembly lines 538-555) */
    for (int i = 0; i < 4; i++) {
        party_characters[i].current_hp = party_characters[i].max_hp;
        party_characters[i].current_hp_target = party_characters[i].max_hp;
        party_characters[i].current_pp = party_characters[i].max_pp;
        party_characters[i].current_pp_target = party_characters[i].max_pp;
    }

    /* Ness's starting money from INITIAL_STATS[0] */
    game_state.money_carried = read_u16_le(initial_stats_data + 4);
    game_state.party_count = 1;
    game_state.player_controlled_party_count = 1;
    game_state.current_party_members = 1;

    /* Set starting position from INITIAL_STATS[0]::coords × 8
       (file_select_menu_loop.asm lines 593-603 → PLACE_LEADER_AT_POSITION) */
    {
        uint16_t start_x = read_u16_le(initial_stats_data + 0);
        uint16_t start_y = read_u16_le(initial_stats_data + 2);
        place_leader_at_position(start_x * 8, start_y * 8);
    }

    /* Assembly lines 604-612: Prepend "PSI " to favourite_thing.
       The naming screen fills favourite_thing+4 onward; we set the prefix here. */
    game_state.favourite_thing[0] = 0x80; /* CHAR::P */
    game_state.favourite_thing[1] = 0x83; /* CHAR::S_ */
    game_state.favourite_thing[2] = 0x79; /* CHAR::I */
    game_state.favourite_thing[3] = 0x50; /* CHAR::SPACE */

    /* Assembly line 644-645: unknownC3 = 1 */
    game_state.unknownC3 = 1;

    /* Assembly lines 647-650: Save leader position as respawn point */
    ow.respawn_x = game_state.leader_x_coord;
    ow.respawn_y = game_state.leader_y_coord;

    /* Assembly line 651: clear any pending NPC/door interactions */
    reset_queued_interactions();

    /* Assembly lines 652-654: Move leader to prologue/meteorite scene location */
    place_leader_at_position(2112, 1768);

    /* Assembly lines 655-656: Freeze entities and queue prologue text.
     * MSG_EVT0_PROLOGUE_METEORITE_FALL = $C5E70B */
    freeze_and_queue_text_interaction(MSG_EVT0_PROLOGUE_METEORITE_FALL);

    /* Assembly lines 657-659: Disable monster spawns for prologue */
    event_flag_set(EVENT_FLAG_MONSTER_OFF);

    /* Assembly lines 660-661: Enable NPC display */
    ow.show_npc_flag = 1;

    /* NOTE: No save here. The assembly does NOT call SAVE_GAME_SLOT after
     * naming/initialization. The first real save happens later when the
     * player calls Dad on the phone (via CC_1F SAVE_GAME in the event
     * script). The incremental saves during text speed/sound/flavour
     * selection are handled by those individual menu functions. */

    close_all_windows();

    return STEP_RESULT_POP(1);
}

/*
 * Main file select loop.
 * Ported from FILE_SELECT_INIT + RUN_FILE_MENU in assembly.
 */
/* @FINALIZE_AND_START (file_select_menu_loop.asm:662-689), shared by Continue
 * and New Game: item transformations, the HP-meter rolling speed from
 * HP_METER_SPEEDS, SELECTED_TEXT_SPEED (the per-letter delay), TEXT_SPEED_BASED_WAIT,
 * then DISPLAY_TEXT_PTR MSG_SYS_GAME_START_INIT (which clears
 * FLG_HOTEL_PAPERBOY_APPEAR, so a save made after a hotel night loads with the
 * plain hotel music). The caller has already closed all windows. */
static void fm_finalize_and_start(void) {
    init_all_item_transformations();

    ensure_file_select_assets();
    int idx = (game_state.text_speed & 0xFF) - 1;
    if (idx < 0) idx = 0;
    if (idx > 2) idx = 2;
    bt.hp_meter_speed = (int32_t)read_u32_le(hp_meter_speeds_data + idx * 4);

    /* STX SELECTED_TEXT_SPEED with X = text_speed - 1 */
    dt.selected_text_speed = (uint8_t)((game_state.text_speed & 0xFF) - 1);

    if ((game_state.text_speed & 0xFF) == 3)
        dt.text_speed_based_wait = 0;
    else
        dt.text_speed_based_wait = (uint16_t)((game_state.text_speed & 0xFF) * 30);

    display_text_from_addr_inline(MSG_SYS_GAME_START_INIT);
}

/* Push GAME_MODE_SELECTION_MENU as a child after a *_build() has laid out the
 * focus window. Sets the FM phase to read the result in on re-entry. Replicates
 * selection_menu()'s early-exit (no focus window / empty menu → result 0 with no
 * push), delivered inline via result_ready so the *_RESULT phase handles it. */
static StepResult fm_push_selection(FileMenuState *st, uint8_t result_phase,
                                    uint16_t allow_cancel) {
    st->phase = result_phase;
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w || w->menu_count == 0) {
        st->result_ready = 1;
        st->result = 0;
        return STEP_RESULT_CONTINUE();
    }
    st->result_ready = 0;
    memset(&g_mode_push_init, 0, sizeof(g_mode_push_init));
    g_mode_push_init.selection_menu.phase        = SM_SETUP;
    g_mode_push_init.selection_menu.allow_cancel = (uint8_t)allow_cancel;
    return STEP_RESULT_PUSH_INIT(GAME_MODE_SELECTION_MENU, &g_mode_push_init);
}

/* Read the result of the menu a *_RESULT phase is processing: the popped child's
 * value, or the inline early-exit value if no child was pushed. */
static uint16_t fm_take_result(FileMenuState *st) {
    uint16_t r = st->result_ready ? st->result : (uint16_t)mode_child_result();
    st->result_ready = 0;
    return r;
}

#ifdef EB_HAS_INPUT_SETTINGS
/* ---- Global settings menu (port addition) ----------------------------------
 * File select -> Settings: keyboard bindings, gamepad choice, stick deadzone.
 * Drawn with the game's own windows and font; the platform keeps the values
 * (platform_input_* in platform.h) and saves them to its config file. */
static uint8_t set_key_page;      /* 0 = D-Pad + Menu/View, 1 = face and shoulder buttons */
static int set_rebind_slot;       /* button slot waiting for a key press */
static const uint8_t set_page_slots[2][6] = {
    { 0, 1, 2, 3, 10, 11 },       /* D-Pad Up/Down/Left/Right, Menu, View */
    { 5, 4, 7, 6, 8, 9 },         /* Xbox A B X Y LB RB (SNES slots B A Y X L R) */
};
static const uint8_t set_deadzones[] = { 10, 15, 20, 25, 30, 35, 40, 50 };
#define SET_DEADZONE_COUNT ((int)sizeof(set_deadzones))
#define SET_MAX_PADS 6

static WindowInfo *set_open_window(int window_id, const char *prompt) {
    WindowInfo *w = fm_create_window((uint16_t)window_id);
    if (!w) return NULL;
    w->menu_count = 0;
    set_focus_text_cursor(0, 0);
    print_string(prompt);
    return w;
}

static int fm_set_menu_build(void) {
    WindowInfo *w = set_open_window(WINDOW_SETTINGS_MENU, "Settings");
    if (!w) return 0;
    char label[40];
    add_menu_item("Keyboard: Directions", 1, 0, 1);
    add_menu_item("Keyboard: Buttons", 2, 0, 2);
    add_menu_item("Gamepad", 3, 0, 3);
    snprintf(label, sizeof(label), "Stick Deadzone: %d%%", platform_input_deadzone());
    add_menu_item(label, 4, 0, 4);
    add_menu_item("Aspect Ratio", 6, 0, 5);
    add_menu_item("Screen Mode", 7, 0, 6);
    add_menu_item("Reset to Defaults", 5, 0, 7);
    print_menu_items();
    return -1;
}

static int fm_set_keys_build(void) {
    WindowInfo *w = set_open_window(WINDOW_SETTINGS_LIST, "Select a button to rebind.");
    if (!w) return 0;
    for (int i = 0; i < 6; i++) {
        int slot = set_page_slots[set_key_page][i];
        char label[40];
        snprintf(label, sizeof(label), "%s: %s", platform_input_slot_name(slot),
                 platform_input_slot_key_name(slot));
        add_menu_item(label, (uint16_t)(i + 1), 0, (uint16_t)(i + 1));
    }
    print_menu_items();
    return -1;
}

static int fm_set_pad_build(void) {
    WindowInfo *w = set_open_window(WINDOW_SETTINGS_LIST, "Select a gamepad.");
    if (!w) return 0;
    int n = platform_input_gamepad_count();
    if (n > SET_MAX_PADS) n = SET_MAX_PADS;
    if (n == 0) {
        add_menu_item("No gamepad found", 99, 0, 1);
    } else {
        for (int i = 0; i < n; i++) {
            char label[MENU_LABEL_SIZE];
            snprintf(label, sizeof(label), "%s", platform_input_gamepad_name(i));
            add_menu_item(label, (uint16_t)(i + 1), 0, (uint16_t)(i + 1));
        }
    }
    print_menu_items();
    int cur = platform_input_gamepad_selected();
    if (n > 0 && cur >= 0 && cur < n) highlight_menu_item(w, (uint16_t)cur, 6, true);
    return -1;
}

static int fm_set_deadzone_build(void) {
    WindowInfo *w = set_open_window(WINDOW_SETTINGS_LIST, "Select the stick deadzone.");
    if (!w) return 0;
    for (int i = 0; i < SET_DEADZONE_COUNT; i++) {
        char label[16];
        snprintf(label, sizeof(label), "%d%%", set_deadzones[i]);
        add_menu_item(label, set_deadzones[i], 0, (uint16_t)(i + 1));
    }
    print_menu_items();
    int cur = platform_input_deadzone();
    for (int i = 0; i < SET_DEADZONE_COUNT; i++)
        if (set_deadzones[i] == cur) highlight_menu_item(w, (uint16_t)i, 6, true);
    return -1;
}

/* ---- Display questions (port): aspect ratio, windowed / full screen ----------
 * Asked after the sound setting in the new-game and Set Up flows, and from the
 * Settings menu. They are display settings of the machine, so they live in the
 * platform config rather than in the save. */
static int fm_aspect_build(void) {
    WindowInfo *w = fm_create_window(WINDOW_DISPLAY_QUESTION);
    if (!w) return 0;
    w->menu_count = 0;
    set_focus_text_cursor(0, 0);
    print_string("Please select an aspect ratio.");
    add_menu_item("4:3", 1, 0, 1);
    add_menu_item("16:10", 2, 0, 2);
    add_menu_item("16:9", 3, 0, 3);
    add_menu_item("Fit window", 4, 0, 4);
    print_menu_items();
    highlight_menu_item(w, (uint16_t)platform_display_aspect(), 6, true);
    return -1;
}

static void fm_aspect_apply(uint16_t result) {
    platform_display_set_aspect(result - 1);
}

static int fm_screen_build(void) {
    WindowInfo *w = fm_create_window(WINDOW_DISPLAY_QUESTION);
    if (!w) return 0;
    w->menu_count = 0;
    set_focus_text_cursor(0, 0);
    print_string("Please select from the following:");
    add_menu_item("Windowed", 1, 0, 1);
    add_menu_item("Full Screen", 2, 0, 2);
    print_menu_items();
    highlight_menu_item(w, platform_display_fullscreen() ? 1 : 0, 6, true);
    return -1;
}

static void fm_screen_apply(uint16_t result) {
    platform_display_set_fullscreen(result == 2);
}

static void fm_set_keywait_open(int slot) {
    set_rebind_slot = slot;
    platform_input_rebind_begin(slot);
    WindowInfo *w = create_window(WINDOW_SETTINGS_MESSAGE);
    if (!w) return;
    set_focus_text_cursor(0, 0);
    char msg[64];
    snprintf(msg, sizeof(msg), "Press a key for %s.", platform_input_slot_name(slot));
    print_string(msg);
    set_focus_text_cursor(0, 1);
    print_string("Esc: cancel");
}
#endif /* EB_HAS_INPUT_SETTINGS */

/*
 * GAME_MODE_FILE_MENU step — run-to-completion port of the file-menu cascade
 * (the loop half of the former file_menu_loop(); its one-shot setup is now
 * file_menu_setup()).
 * See FileMenuState in mode_stack.h for the phase machine and what stays blocking.
 * The synchronous build/apply helpers and the sub-menu pushes let several
 * phases chain within one dispatch (the internal for-loop), matching the blocking
 * original's zero-yield gap between one menu closing and the next opening.
 */
StepResult mode_step_file_menu(ModeState *ms) {
    FileMenuState *st = &ms->file_menu;

    for (;;) {
        switch ((FileMenuPhase)st->phase) {
        case FM_FADEIN_WAIT:
            /* while (fade_active()) { battle_bg_update; fade_update; yield } */
            if (fade_active()) {
                battle_bg_update();
                fade_update();
                return STEP_RESULT_CONTINUE();
            }
            st->phase = FM_SELECT;
            continue;

        case FM_SELECT: {
            /* Outer-loop head: re-read every slot, draw the screen, then run the browse loop. */
            battle_bg_update();
            file_select_scan_saves();
            browse_mode = BROWSE_NORMAL;
            input_guard = INPUT_GUARD_FRAMES / 2;
            browse_pane = 1;
            browse_show_cursor(browse_cursor);
            browse_draw(true);
            browse_dirty = 0;
            window_tick_no_actionscript();
            st->phase = FM_BROWSE;
            return STEP_RESULT_CONTINUE();
        }

#ifdef EB_HAS_FILE_BROWSER
        /* ---- save backup / restore (port addition) ----------------------------- */
        case FM_FB: {
            battle_bg_update();
            uint16_t r = fb_step_input();
            if (r == 0xFFFF) {                           /* B: back to the save list */
                close_all_windows();
                st->phase = FM_SELECT;
                continue;
            }
            if (r == 1) {
                if (fb_purpose == FB_BACKUP) {
                    int slot = st->selected - 1;
                    fb_backup_name(fb_chosen, sizeof(fb_chosen), slot);
                    const char *base = strrchr(fb_chosen, '/');
                    if (backup_save(slot, fb_chosen))
                        fm_message_open(st, FM_SELECT, "Backup saved:", base ? base + 1 : fb_chosen);
                    else
                        fm_message_open(st, FM_SELECT, "The backup could not be written.", "Is the folder writable?");
                } else if (restore_check(fb_chosen) == RESTORE_OK) {
                    snprintf(restore_path, sizeof(restore_path), "%s", fb_chosen);
                    close_all_windows();
                    browse_mode = BROWSE_RESTORE_DEST;
                    input_guard = INPUT_GUARD_FRAMES;
                    browse_pane = 1;
                    {
                        uint16_t first = 0;              /* start on the first free slot */
                        while (first < SAVE_COUNT - 1 && save_files_present[first]) first++;
                        browse_show_cursor(first);
                    }
                    browse_draw(true);
                    browse_dirty = 0;
                    st->phase = FM_BROWSE;
                } else {
                    fb_dirty = 1;
                    fm_message_open(st, FM_FB, "That is not a usable save file.", "It must be a .srm saved by this game.");
                }
                continue;
            }
            if (fb_dirty) {
                fb_dirty = 0;
                fb_draw();
            }
            clear_instant_printing();
            window_tick_no_actionscript();
            return STEP_RESULT_CONTINUE();
        }

        case FM_MSG: {
            battle_bg_update();
            clear_instant_printing();
            window_tick_no_actionscript();
            if (input_guard) input_guard--;
            else if (platform_input_get_pad_new() & (PAD_CONFIRM | PAD_CANCEL)) {
                close_all_windows();
                st->phase = fb_msg_next;
                continue;
            }
            return STEP_RESULT_CONTINUE();
        }

        case FM_RESTORE_CONFIRM_RESULT: {
            uint16_t yes = fm_take_result(st);
            close_window(WINDOW_FILE_SELECT_DELETE);
            if (!yes) {                                  /* back to picking a slot */
                browse_mode = BROWSE_RESTORE_DEST;
                input_guard = INPUT_GUARD_FRAMES;
                browse_dirty = 1;
                st->phase = FM_BROWSE;
                continue;
            }
            char line[48];
            if (restore_save(st->selected - 1, restore_path))
                snprintf(line, sizeof(line), "Save restored to slot %d.", st->selected);
            else
                snprintf(line, sizeof(line), "The restore failed.");
            fm_message_open(st, FM_SELECT, line, NULL);
            continue;
        }
#endif /* EB_HAS_FILE_BROWSER */

        case FM_BROWSE: {
            battle_bg_update();
            uint16_t r = browse_step_input();
            if (r != 0) {
#ifdef EB_HAS_FILE_BROWSER
                if (browse_mode == BROWSE_RESTORE_DEST) {
                    if (r == 0xFFFF) {                   /* B: back to the folder browser */
                        close_all_windows();
                        browse_mode = BROWSE_NORMAL;
                        fb_refresh();
                        st->phase = FM_FB;
                        continue;
                    }
                    st->selected = r;                    /* 1-based target slot */
                    if (!save_files_present[r - 1]) {
                        char line[48];
                        if (restore_save(r - 1, restore_path))
                            snprintf(line, sizeof(line), "Save restored to slot %d.", r);
                        else
                            snprintf(line, sizeof(line), "The restore failed.");
                        browse_mode = BROWSE_NORMAL;
                        fm_message_open(st, FM_SELECT, line, NULL);
                        continue;
                    }
                    /* A used slot: ask before overwriting it (same dialog as Delete). */
                    WindowInfo *dw = create_window(WINDOW_FILE_SELECT_DELETE);
                    if (dw) {
                        char ask[40];
                        set_instant_printing();
                        set_focus_text_cursor(0, 0);
                        print_string("Overwrite this save?");
                        set_focus_text_cursor(0, 1);
                        snprintf(ask, sizeof(ask), "Slot %d: %s", r, slot_names[r - 1]);
                        print_string(ask);
                        add_menu_item("No", 0, 0, 2);
                        add_menu_item("Yes", 1, 0, 3);
                    }
                    print_menu_items();
                    browse_mode = BROWSE_NORMAL;
                    return fm_push_selection(st, FM_RESTORE_CONFIRM_RESULT, 1);
                }
#endif
                if (browse_mode == BROWSE_NG_SOURCE || browse_mode == BROWSE_NG_DEST || browse_mode == BROWSE_CRAZY_DEST) {
                    if (r == 0xFFFF) {                   /* B: back to the save list */
                        ng_plus_cancel();
                        close_all_windows();
                        st->phase = FM_SELECT;
                    } else if (browse_mode == BROWSE_NG_SOURCE) {
                        ng_plus_begin(r - 1);            /* now the free slot to play it in */
                        browse_mode = BROWSE_NG_DEST;
                        input_guard = INPUT_GUARD_FRAMES;
                        uint16_t first = 0;
                        while (first < SAVE_COUNT - 1 && save_files_present[first]) first++;
                        browse_show_cursor(first);
                        browse_draw(true);
                        browse_dirty = 0;
                        window_tick_no_actionscript();
                        return STEP_RESULT_CONTINUE();
                    } else {                             /* start the new game in slot r with the party carried over
                                                          * (or from a blank slate, for Crazy Mode) */
                        current_save_slot = (uint8_t)r;
                        game_state_init();
                        close_all_windows();
                        st->phase = FM_NG_TS;
                    }
                    continue;
                }
                if (browse_mode == BROWSE_COPY_DEST) {
                    dt.enable_word_wrap = 0;
                    if (r == 0xFFFF) {                   /* B: back to the save list */
                        close_all_windows();
                        st->phase = FM_SELECT;
                    } else {                             /* COPY_SAVE(to = N-1, from = current slot) */
                        copy_save(r - 1, current_save_slot - 1);
                        close_all_windows();
                        st->phase = FM_SELECT;
                    }
                    continue;
                }
                st->result_ready = 1;
                st->result = r;
                st->phase = FM_SELECT_RESULT;
                continue;
            }
            if (browse_dirty) {
                browse_dirty = 0;
                browse_draw(true);
            }
            clear_instant_printing();           /* the window pump skips its render while instant printing is on */
            window_tick_no_actionscript();
            return STEP_RESULT_CONTINUE();
        }

        case FM_SELECT_RESULT: {
            uint16_t selected = fm_take_result(st);
            st->selected = selected;
#ifdef EB_HAS_FILE_BROWSER
            if (selected == BROWSE_RESULT_RESTORE) {
                close_all_windows();
                fb_start(FB_RESTORE);
                st->phase = FM_FB;
                continue;
            }
#endif
#ifdef EB_HAS_INPUT_SETTINGS
            if (selected == BROWSE_RESULT_SETTINGS) {
                close_all_windows();
                st->phase = FM_SET_MENU;
                continue;
            }
#endif
            if (selected == BROWSE_RESULT_COPY || selected == BROWSE_RESULT_DELETE
#ifdef EB_HAS_FILE_BROWSER
                || selected == BROWSE_RESULT_BACKUP
#endif
                ) {
                /* These act on the highlighted save, which must exist. */
                int slot = browse_cursor;
                if (!save_files_present[slot]) {
#ifdef EB_HAS_FILE_BROWSER
                    fm_message_open(st, FM_SELECT, "That slot has no save.", "Highlight a save first.");
#else
                    play_sfx(4);
                    st->phase = FM_SELECT;
#endif
                    continue;
                }
                current_save_slot = (uint8_t)(slot + 1);
                st->selected = (uint16_t)(slot + 1);
                load_game(slot);                               /* the Delete dialog shows its name and level */
                st->result_ready = 1;
                st->result = selected == BROWSE_RESULT_COPY ? 2 : selected == BROWSE_RESULT_DELETE ? 3 : 5;
                st->phase = FM_SUBMENU_RESULT;
                continue;
            }
            if (selected == BROWSE_RESULT_NGPLUS) {
                browse_mode = BROWSE_NG_SOURCE;
                input_guard = INPUT_GUARD_FRAMES;
                browse_pane = 1;
                {
                    uint16_t first = 0;                  /* start on the first cleared save */
                    while (first < SAVE_COUNT - 1 && !slot_cleared[first]) first++;
                    browse_show_cursor(first);
                }
                browse_draw(true);
                browse_dirty = 0;
                window_tick_no_actionscript();
                st->phase = FM_BROWSE;
                return STEP_RESULT_CONTINUE();
            }
            if (selected == BROWSE_RESULT_CRAZY) {
                ng_plus_begin_crazy();
                browse_mode = BROWSE_CRAZY_DEST;
                input_guard = INPUT_GUARD_FRAMES;
                browse_pane = 1;
                {
                    uint16_t first = 0;                  /* start on the first free slot */
                    while (first < SAVE_COUNT - 1 && save_files_present[first]) first++;
                    browse_show_cursor(first);
                }
                browse_draw(true);
                browse_dirty = 0;
                window_tick_no_actionscript();
                st->phase = FM_BROWSE;
                return STEP_RESULT_CONTINUE();
            }
            if (selected == BROWSE_RESULT_EXIT) {
                platform_request_quit();
                st->phase = FM_SELECT;
                continue;
            }
            current_save_slot = (uint8_t)selected;   /* file_select_menu() did this */
            if (selected == 0) { st->phase = FM_SELECT; continue; }
            int slot = selected - 1;
            if (save_files_present[slot]) {
                /* FILE_SELECT_MENU @AFTER_SELECTION: LOAD_GAME_SLOT(CURRENT_SAVE_SLOT-1),
                 * so the submenu / delete confirmation show this slot's data. */
                load_game(slot);
                st->phase = FM_SUBMENU;              /* @VALID_FILE_SELECTED */
            } else {
                game_state_init();
                /* The setup dialogs stack several text windows; with the save list and options column still
                 * open the text-tile pool runs out and a dialog silently fails to open. */
                close_all_windows();
                st->phase = FM_NG_TS;
            }
            continue;
        }

        case FM_SUBMENU: {
            int e = fm_submenu_build();
            if (e >= 0) {
                st->result_ready = 1;
                st->result = (uint16_t)e;
                st->phase = FM_SUBMENU_RESULT;
                continue;
            }
            return fm_push_selection(st, FM_SUBMENU_RESULT, 1);
        }

        case FM_SUBMENU_RESULT: {
            uint16_t action = fm_take_result(st);
            int slot = st->selected - 1;
            switch (action) {
            case 0: /* B pressed — @MENU_B_PRESSED */
                close_focus_window();
                st->phase = FM_SELECT;
                continue;

            case 1: /* Continue — @MENU_STARTGAME_SELECTED */
                load_game(slot);
                reset_queued_interactions();
                reload_hotspots();
                ow.respawn_x = game_state.leader_x_coord;
                ow.respawn_y = game_state.leader_y_coord;
                close_all_windows();
                fm_finalize_and_start();
                return STEP_RESULT_POP(1);

            case 2: /* Copy — @MENU_COPY_SELECTED: SHOW_FILE_COPY_DIALOG.
                     * The submenu window stays open behind the copy window. */
                browse_mode = BROWSE_COPY_DEST;
                input_guard = INPUT_GUARD_FRAMES;
                browse_pane = 1;
                {
                    uint16_t first = 0;                  /* start on the first free slot */
                    while (first < SAVE_COUNT - 1 && save_files_present[first]) first++;
                    browse_show_cursor(first);
                }
                browse_draw(true);
                browse_dirty = 0;
                window_tick_no_actionscript();
                st->phase = FM_BROWSE;
                return STEP_RESULT_CONTINUE();

            case 3: /* Delete — @MENU_DELETE_SELECTED: CONFIRM_FILE_DELETE
                     * (asm/text/confirm_file_delete.asm). The submenu window
                     * stays open behind the confirmation window. */
            {
                WindowInfo *dw = create_window(WINDOW_FILE_SELECT_DELETE);
                if (dw) {
                    set_instant_printing();
                    set_window_number_padding(0);
                    set_focus_text_cursor(0, 0);
                    print_string("Are you sure you want to delete?");  /* 32 chars */

                    set_text_pixel_position(1, 0);
                    set_focus_text_cursor(0, 1);
                    print_number(current_save_slot, 0);
                    print_string(":");                 /* PRINT_LETTER CHAR::COLON */

                    /* PRINT_CHARACTER_NAME(1): party member 1's name */
                    set_focus_text_cursor(2, 1);
                    print_eb_string(party_characters[0].name,
                                    (int)sizeof(party_characters[0].name));

                    set_focus_text_cursor(8, 1);
                    print_string("Level:");            /* FILE_SELECT_TEXT_LEVEL, 6 chars */

                    set_focus_text_cursor(12, 1);
                    print_number(party_characters[0].level, 0);

                    add_menu_item("No", 0, 0, 2);
                    add_menu_item("Yes", 1, 0, 3);
                }
                print_menu_items();
                return fm_push_selection(st, FM_DELETE_RESULT, 1);
            }

            case 4: /* Set Up */
                close_all_windows();                     /* free the text tiles the setup dialogs need */
                load_game(slot);
                st->phase = FM_SETUP_TS;
                continue;

#ifdef EB_HAS_FILE_BROWSER
            case 5: /* Backup (port addition): pick a folder for this slot's save image */
                close_all_windows();
                fb_start(FB_BACKUP);
                st->phase = FM_FB;
                continue;
#endif

            default:
                st->phase = FM_SELECT;
                continue;
            }
        }

        case FM_DELETE_RESULT: {
            /* CONFIRM_FILE_DELETE tail: Yes (1) → ERASE_SAVE(CURRENT_SAVE_SLOT-1);
             * either way ENABLE_WORD_WRAP = 0 and CLOSE_FOCUS_WINDOW. FILE_MENU_LOOP
             * then returns to @VALID_FILE_SELECTED (the submenu) on 0 (No / B), or
             * @MENU_OTHER_SELECTED (close all, back to the slot list) otherwise. */
            uint16_t del_confirm = fm_take_result(st);
            if (del_confirm != 0)
                erase_save(current_save_slot - 1);
            dt.enable_word_wrap = 0;
            close_focus_window();
            if (del_confirm == 0) {
                st->phase = FM_SELECT;
            } else {
                close_all_windows();
                st->phase = FM_SELECT;
            }
            continue;
        }

#ifdef EB_HAS_INPUT_SETTINGS
        /* ---- display questions in the new-game / Set Up cascades (port) ------ */
        case FM_NG_ASPECT: case FM_SETUP_ASPECT: {
            bool ng = st->phase == FM_NG_ASPECT;
            int e = fm_aspect_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = ng ? FM_NG_ASPECT_RESULT : FM_SETUP_ASPECT_RESULT; continue; }
            return fm_push_selection(st, ng ? FM_NG_ASPECT_RESULT : FM_SETUP_ASPECT_RESULT, 1);
        }
        case FM_NG_ASPECT_RESULT: case FM_SETUP_ASPECT_RESULT: {
            bool ng = st->phase == FM_NG_ASPECT_RESULT;
            uint16_t r = fm_take_result(st);
            close_window(WINDOW_DISPLAY_QUESTION);
            if (r == 0) { st->phase = ng ? FM_NG_SND : FM_SETUP_SND; continue; }   /* B: back to sound */
            fm_aspect_apply(r);
            st->phase = ng ? FM_NG_SCREEN : FM_SETUP_SCREEN;
            continue;
        }
        case FM_NG_SCREEN: case FM_SETUP_SCREEN: {
            bool ng = st->phase == FM_NG_SCREEN;
            int e = fm_screen_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = ng ? FM_NG_SCREEN_RESULT : FM_SETUP_SCREEN_RESULT; continue; }
            return fm_push_selection(st, ng ? FM_NG_SCREEN_RESULT : FM_SETUP_SCREEN_RESULT, 1);
        }
        case FM_NG_SCREEN_RESULT: case FM_SETUP_SCREEN_RESULT: {
            bool ng = st->phase == FM_NG_SCREEN_RESULT;
            uint16_t r = fm_take_result(st);
            close_window(WINDOW_DISPLAY_QUESTION);
            if (r == 0) { st->phase = ng ? FM_NG_ASPECT : FM_SETUP_ASPECT; continue; }
            fm_screen_apply(r);
            st->phase = ng ? FM_NG_FLV : FM_SETUP_FLV;
            continue;
        }
        /* ---- ... and from the Settings menu ----------------------------------- */
        case FM_SET_ASPECT: {
            battle_bg_update();
            int e = fm_aspect_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_SET_ASPECT_RESULT; continue; }
            return fm_push_selection(st, FM_SET_ASPECT_RESULT, 1);
        }
        case FM_SET_ASPECT_RESULT: {
            uint16_t r = fm_take_result(st);
            close_all_windows();
            if (r != 0) fm_aspect_apply(r);
            st->phase = FM_SET_MENU;
            continue;
        }
        case FM_SET_SCREEN: {
            battle_bg_update();
            int e = fm_screen_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_SET_SCREEN_RESULT; continue; }
            return fm_push_selection(st, FM_SET_SCREEN_RESULT, 1);
        }
        case FM_SET_SCREEN_RESULT: {
            uint16_t r = fm_take_result(st);
            close_all_windows();
            if (r != 0) fm_screen_apply(r);
            st->phase = FM_SET_MENU;
            continue;
        }
#endif /* EB_HAS_INPUT_SETTINGS */

#ifdef EB_HAS_INPUT_SETTINGS
        /* ---- global settings menu (port addition) ------------------------ */
        case FM_SET_MENU: {
            battle_bg_update();
            int e = fm_set_menu_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_SET_MENU_RESULT; continue; }
            return fm_push_selection(st, FM_SET_MENU_RESULT, 1);
        }
        case FM_SET_MENU_RESULT: {
            uint16_t r = fm_take_result(st);
            close_all_windows();
            switch (r) {
            case 1: case 2: set_key_page = (uint8_t)(r - 1); st->phase = FM_SET_KEYS; break;
            case 3: st->phase = FM_SET_PAD; break;
            case 4: st->phase = FM_SET_DEADZONE; break;
            case 5: platform_input_reset_defaults(); st->phase = FM_SET_MENU; break;
            case 6: st->phase = FM_SET_ASPECT; break;
            case 7: st->phase = FM_SET_SCREEN; break;
            default: st->phase = FM_SELECT; break;      /* back / cancel */
            }
            continue;
        }
        case FM_SET_KEYS: {
            battle_bg_update();
            int e = fm_set_keys_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_SET_KEYS_RESULT; continue; }
            return fm_push_selection(st, FM_SET_KEYS_RESULT, 1);
        }
        case FM_SET_KEYS_RESULT: {
            uint16_t r = fm_take_result(st);
            close_all_windows();
            if (r == 0) { st->phase = FM_SET_MENU; continue; }
            fm_set_keywait_open(set_page_slots[set_key_page][r - 1]);
            st->phase = FM_SET_KEYWAIT;
            continue;
        }
        case FM_SET_KEYWAIT:
            battle_bg_update();
            if (platform_input_rebind_active()) {
                clear_instant_printing();
                window_tick_no_actionscript();
                return STEP_RESULT_CONTINUE();          /* one frame per poll until a key arrives */
            }
            close_all_windows();
            st->phase = FM_SET_KEYS;
            continue;
        case FM_SET_PAD: {
            battle_bg_update();
            int e = fm_set_pad_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_SET_PAD_RESULT; continue; }
            return fm_push_selection(st, FM_SET_PAD_RESULT, 1);
        }
        case FM_SET_PAD_RESULT: {
            uint16_t r = fm_take_result(st);
            close_all_windows();
            if (r >= 1 && r <= SET_MAX_PADS) platform_input_gamepad_select(r - 1);
            st->phase = FM_SET_MENU;
            continue;
        }
        case FM_SET_DEADZONE: {
            battle_bg_update();
            int e = fm_set_deadzone_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_SET_DEADZONE_RESULT; continue; }
            return fm_push_selection(st, FM_SET_DEADZONE_RESULT, 1);
        }
        case FM_SET_DEADZONE_RESULT: {
            uint16_t r = fm_take_result(st);
            close_all_windows();
            if (r != 0) platform_input_set_deadzone(r);
            st->phase = FM_SET_MENU;
            continue;
        }
#endif /* EB_HAS_INPUT_SETTINGS */

        /* ---- existing-save Set Up cascade ------------------------------- */
        case FM_SETUP_TS: {
            int e = fm_textspeed_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_SETUP_TS_RESULT; continue; }
            return fm_push_selection(st, FM_SETUP_TS_RESULT, 1);
        }
        case FM_SETUP_TS_RESULT: {
            uint16_t ts = fm_take_result(st);
            if (ts == 0) {  /* @VALID_FILE_SELECTED — back to submenu */
                close_window(WINDOW_FILE_SELECT_TEXT_SPEED);
                st->phase = FM_SUBMENU;
                continue;
            }
            fm_textspeed_apply(ts);
            st->phase = FM_SETUP_SND;
            continue;
        }
        case FM_SETUP_SND: {
            int e = fm_sound_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_SETUP_SND_RESULT; continue; }
            return fm_push_selection(st, FM_SETUP_SND_RESULT, 1);
        }
        case FM_SETUP_SND_RESULT: {
            uint16_t sm = fm_take_result(st);
            if (sm == 0) {
                close_window(WINDOW_FILE_SELECT_MUSIC_MODE);
                st->phase = FM_SETUP_TS;
                continue;
            }
            fm_sound_apply(sm);
#ifdef EB_HAS_INPUT_SETTINGS
            st->phase = FM_SETUP_ASPECT;
#else
            st->phase = FM_SETUP_FLV;
#endif
            continue;
        }
        case FM_SETUP_FLV: {
            int e = fm_flavour_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_SETUP_FLV_RESULT; continue; }
            return fm_push_selection(st, FM_SETUP_FLV_RESULT, 1);
        }
        case FM_SETUP_FLV_RESULT: {
            uint16_t fl = fm_take_result(st);
            if (fl == 0) {
                close_window(WINDOW_FILE_SELECT_FLAVOUR);
#ifdef EB_HAS_INPUT_SETTINGS
                st->phase = FM_SETUP_SCREEN;
#else
                st->phase = FM_SETUP_SND;
#endif
                continue;
            }
            fm_flavour_apply(fl);
            save_game(st->selected - 1);   /* @MENU_OTHER_SELECTED */
            close_all_windows();
            st->phase = FM_SELECT;
            continue;
        }

        /* ---- new-game cascade ------------------------------------------- */
        case FM_NG_TS: {
            int e = fm_textspeed_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_NG_TS_RESULT; continue; }
            return fm_push_selection(st, FM_NG_TS_RESULT, 1);
        }
        case FM_NG_TS_RESULT: {
            uint16_t ts = fm_take_result(st);
            if (ts == 0) {
                close_window(WINDOW_FILE_SELECT_TEXT_SPEED);
                st->phase = FM_SELECT;   /* back to file select */
                continue;
            }
            fm_textspeed_apply(ts);
            st->phase = FM_NG_SND;
            continue;
        }
        case FM_NG_SND: {
            int e = fm_sound_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_NG_SND_RESULT; continue; }
            return fm_push_selection(st, FM_NG_SND_RESULT, 1);
        }
        case FM_NG_SND_RESULT: {
            uint16_t sm = fm_take_result(st);
            if (sm == 0) {
                close_window(WINDOW_FILE_SELECT_MUSIC_MODE);
                st->phase = FM_NG_TS;
                continue;
            }
            fm_sound_apply(sm);
#ifdef EB_HAS_INPUT_SETTINGS
            st->phase = FM_NG_ASPECT;
#else
            st->phase = FM_NG_FLV;
#endif
            continue;
        }
        case FM_NG_FLV: {
            int e = fm_flavour_build();
            if (e >= 0) { st->result_ready = 1; st->result = (uint16_t)e;
                          st->phase = FM_NG_FLV_RESULT; continue; }
            return fm_push_selection(st, FM_NG_FLV_RESULT, 1);
        }
        case FM_NG_FLV_RESULT: {
            uint16_t fl = fm_take_result(st);
            if (fl == 0) {
                close_window(WINDOW_FILE_SELECT_FLAVOUR);
#ifdef EB_HAS_INPUT_SETTINGS
                st->phase = FM_NG_SCREEN;
#else
                st->phase = FM_NG_SND;
#endif
                continue;
            }
            fm_flavour_apply(fl);
            st->phase = FM_NG_NAMING;
            continue;
        }

        case FM_NG_NAMING:
            /* Naming screen — GAME_MODE_NEW_GAME_NAMING (pops 1 = started, 0 =
             * backed out past the first name). */
            current_save_slot = (uint8_t)st->selected;
            memset(&g_mode_push_init, 0, sizeof(g_mode_push_init));
            g_mode_push_init.new_game_naming.phase = NGN_ENTER;
            st->phase = FM_NG_NAMING_RESULT;
            return STEP_RESULT_PUSH_INIT(GAME_MODE_NEW_GAME_NAMING, &g_mode_push_init);

        case FM_NG_NAMING_RESULT:
            if (mode_child_result() == 0) {
                /* Back out of naming: re-display all setup windows as backdrops,
                 * change music, re-enter flavour selection (file_select_menu_loop
                 * .asm:141-150). Save choices before the display_only load_game()s
                 * clobber game_state. */
                uint8_t saved_text_speed = game_state.text_speed;
                uint8_t saved_sound_setting = game_state.sound_setting;
                close_all_windows();
                /* (the save list is not redrawn behind the setup dialogs: see FM_SELECT_RESULT) */
                text_speed_menu_display_only(saved_text_speed);
                sound_mode_menu_display_only(saved_sound_setting);
                change_music(3);  /* MUSIC::SETUP_SCREEN */
                st->phase = FM_NG_FLV;
                continue;
            }
            fm_finalize_and_start();
            return STEP_RESULT_POP(1);
        }

        /* Unreachable: every phase returns or continues. */
        return STEP_RESULT_POP(0);
    }
}

/*
 * Main file select loop — thin blocking wrapper.
 * One-shot setup (asset loads + FILE_SELECT_INIT + fade_in) stays here; the
 * fade-in wait and the menu cascade run as GAME_MODE_FILE_MENU.
 * Ported from FILE_SELECT_INIT + RUN_FILE_MENU in assembly.
 */
void file_menu_setup(void) {
    /* Load asset data for file select screen (consumers also resolve lazily). */
    ensure_file_select_assets();

    /* Run the faithful FILE_SELECT_INIT sequence */
    file_select_init();

    /* Fade in (matching assembly: FADE_IN with X=1) */
    fade_in(1, 1);

    /* The file-menu cascade itself runs as GAME_MODE_FILE_MENU (FM_FADEIN_WAIT),
     * STEP_PUSHed by the init_intro parent after this setup returns. */
}
