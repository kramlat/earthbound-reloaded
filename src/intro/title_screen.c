/*
 * Port of SHOW_TITLE_SCREEN (asm/intro/show_title_screen.asm)
 * and supporting functions.
 *
 * ROM call graph:
 *   SHOW_TITLE_SCREEN
 *     ├── FORCE_BLANK_AND_WAIT_VBLANK
 *     ├── INIT_ENTITY_SYSTEM
 *     ├── SET_BGMODE(11)                    → ppu.bgmode = 0x0B
 *     ├── SET_OAM_SIZE(3)                   → ppu.obsel = 0x03
 *     ├── SET_BG1_VRAM_LOCATION(0,$5800,0)  → ppu.bg_sc/bg_nba
 *     ├── LOAD_TITLE_SCREEN_GRAPHICS        → load_title_screen_graphics()
 *     ├── OAM_CLEAR                         → memset oam/oam_hi
 *     ├── INIT_ENTITY_WIPE(TITLE_SCREEN_1)  → entity_init_wipe()
 *     │     └── Entity events drive:
 *     │         ├── LOAD_TITLE_SCREEN_PALETTE (C0ECB7)
 *     │         └── Letter sprite animations (E1CE08)
 *     ├── (non-quick) sprite palette fade + 60-frame entity loop
 *     ├── Input wait loop (with entity system running)
 *     └── FADE_OUT_WITH_MOSAIC
 */
#include "intro/title_screen.h"
#include "entity/entity.h"
#include "entity/buffer_layout.h"
#include "game/overworld.h"
#include "data/event_script_data.h"
#include "include/binary.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "core/decomp.h"
#include "core/memory.h"
#include "game/fade.h"
#include "game/audio.h"
#include "data/assets.h"
#include "platform/platform.h"
#include "include/pad.h"
#include "core/mode_stack.h"
#include <string.h>

/* Forward declarations from main.c */
#include "game_main.h"


static void reloaded_build_gfx(void);
static void reloaded_overlay(void);
static uint16_t reloaded_frame;

/*
 * Port of LOAD_TITLE_SCREEN_GRAPHICS (asm/intro/load_title_screen_graphics.asm).
 *
 * Decompresses and DMA-transfers tile/tilemap/OBJ data to VRAM.
 * Does NOT load ert.palettes — the entity system handles those.
 *
 * VRAM layout (word addresses, from include/enums.asm):
 *   VRAM::LOGO_TILES              = $0000  ($B000 bytes, BG1 8bpp tile data)
 *   VRAM::TITLE_SCREEN_TILEMAP_EB = $5800  ($1000 bytes, BG1 tilemap)
 *   VRAM::TITLE_SCREEN_OBJ_EB    = $6000  ($4000 bytes, OBJ 4bpp tile data)
 */
void load_title_screen_graphics(void) {
    size_t comp_size;
    const uint8_t *comp_data;

    /* ROM: DECOMP TITLE_SCREEN_GRAPHICS → BUFFER
     *      COPY_TO_VRAM1P BUFFER, VRAM::LOGO_TILES, $B000, 0
     * Decompress directly to VRAM — no intermediate buffer needed. */
    comp_size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_GFX_LZHAL);
    comp_data = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_GFX_LZHAL);
    if (comp_data) {
        uint8_t *vram_dst = &ppu.vram[0x0000 * 2];
        memset(vram_dst, 0, 0xB000);
        decomp(comp_data, comp_size, vram_dst, 0xB000);
    }

    /* ROM: DECOMP TITLE_SCREEN_ARRANGEMENT → BUFFER
     *      COPY_TO_VRAM1P BUFFER, VRAM::TITLE_SCREEN_TILEMAP_EB, $1000, 0 */
    comp_size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_ARR_LZHAL);
    comp_data = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_ARR_LZHAL);
    if (comp_data) {
        uint8_t *vram_dst = &ppu.vram[0x5800 * 2];
        memset(vram_dst, 0, 0x1000);
        decomp(comp_data, comp_size, vram_dst, 0x1000);
    }

    /* ROM: DECOMP TITLE_SCREEN_LETTER_GFX → BUFFER
     *      COPY_TO_VRAM1P BUFFER, VRAM::TITLE_SCREEN_OBJ_EB, $4000, 0 */
    comp_size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_LETTERS_GFX_LZHAL);
    comp_data = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_LETTERS_GFX_LZHAL);
    if (comp_data) {
        uint8_t *vram_dst = &ppu.vram[0x6000 * 2];
        memset(vram_dst, 0, 0x4000);
        decomp(comp_data, comp_size, vram_dst, 0x4000);
    }
    reloaded_build_gfx();
}


/* ---- "Reloaded" overlay (port addition) ---------------------------------------------------------
 * A white "Reloaded" that drops in from the top of the title screen and settles just below the
 * EARTHBOUND logo. It is drawn with sprites: the letters are rendered into free OBJ tiles (the
 * title's letter graphics leave the tail of the table empty) and palette group 9, and added to OAM
 * by screen_overlay_hook after the entity sprites. Nothing is read from the ROM. */
#define RELOADED_TILE_BASE   96          /* first free tile of OBJ name table 1 (tile 352 overall) */
#define RELOADED_CHUNKS      6           /* 16x16 sprites across */
#define RELOADED_WIDTH       96
#define RELOADED_FINAL_Y     131         /* native Y of the text's top when it has settled */
#define RELOADED_START_DELAY 90          /* frames after the title appears before it drops */
#define RELOADED_DROP        40          /* frames the drop takes (bounce included) */

static const char *const reloaded_glyph[6][7] = {
    /* R */ { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" },
    /* e */ { ".....", ".....", ".###.", "#...#", "#####", "#....", ".###." },
    /* l */ { ".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###." },
    /* o */ { ".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###." },
    /* a */ { ".....", ".....", ".###.", "....#", ".####", "#...#", ".####" },
    /* d */ { "....#", "....#", ".####", "#...#", "#...#", "#...#", ".####" },
};
static const uint8_t reloaded_text[8] = { 0, 1, 2, 3, 4, 5, 1, 5 };      /* R e l o a d e d */

static uint8_t reloaded_px[16][RELOADED_WIDTH];   /* 0 clear, 1 outline, 2 fill, 3 highlight */
/* Sprite palette group 9 is loaded by the game (it shares CGRAM with the logo's 8bpp BG, so its
 * colours cannot be changed): colours 1-5 and 14-15 are white, 6-13 black. White letters, black outline. */
static const uint8_t reloaded_color[4] = { 0, 6, 1, 2 };
static void reloaded_build_gfx(void) {
    memset(reloaded_px, 0, sizeof(reloaded_px));
    /* glyphs at 2x, 1 px of outline all round */
    for (int g = 0; g < 8; g++) {
        const char *const *rows = reloaded_glyph[reloaded_text[g]];
        int x0 = 1 + g * 12;
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++) {
                if (rows[gy][gx] != '#') continue;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int px = x0 + gx * 2 + dx, py = 1 + gy * 2 + dy;
                        reloaded_px[py][px] = (dy == 0 && gy == 0) ? 3 : (dy == 0 ? 3 : 2);
                    }
            }
    }
    for (int y = 0; y < 16; y++)                    /* outline: any clear pixel touching the letters */
        for (int x = 0; x < RELOADED_WIDTH; x++) {
            if (reloaded_px[y][x]) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && yy < 16 && xx >= 0 && xx < RELOADED_WIDTH &&
                        (reloaded_px[yy][xx] == 2 || reloaded_px[yy][xx] == 3))
                        reloaded_px[y][x] = 1;
                }
        }
    /* 4bpp planar tiles into OBJ name table 1: chunk c's 16x16 = tiles t, t+1, t+16, t+17 */
    for (int c = 0; c < RELOADED_CHUNKS; c++)
        for (int ty = 0; ty < 2; ty++)
            for (int tx = 0; tx < 2; tx++) {
                int tile = RELOADED_TILE_BASE + c * 2 + tx + ty * 16;
                uint8_t *t = &ppu.vram[(0x7000 + tile * 16) * 2];
                for (int row = 0; row < 8; row++) {
                    uint8_t plane[4] = { 0, 0, 0, 0 };
                    for (int col = 0; col < 8; col++) {
                        uint8_t v = reloaded_color[reloaded_px[ty * 8 + row][c * 16 + tx * 8 + col]];
                        for (int b = 0; b < 4; b++)
                            plane[b] |= (uint8_t)(((v >> b) & 1) << (7 - col));
                    }
                    t[row * 2] = plane[0];
                    t[row * 2 + 1] = plane[1];
                    t[16 + row * 2] = plane[2];
                    t[16 + row * 2 + 1] = plane[3];
                }
            }
}

static void reloaded_overlay(void) {
    uint16_t f = reloaded_frame++;
    if (f < RELOADED_START_DELAY) return;

    /* drop from above the picture and bounce once */
    int t = f - RELOADED_START_DELAY;
    int y = RELOADED_FINAL_Y;
    if (t < RELOADED_DROP) {
        static const int16_t rise[RELOADED_DROP] = {
            -147, -143, -137, -129, -119, -107, -93, -77, -59, -39, -17, 0, 0, 0, 0, 0,
            -8, -14, -18, -20, -20, -18, -14, -8, 0, -3, -5, -6, -5, -3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        y += rise[t];
    }
    int x0 = (SNES_WIDTH - RELOADED_WIDTH) / 2;
    for (int c = 0; c < RELOADED_CHUNKS && ert.oam_write_index < 128; c++) {
        uint16_t oi = ert.oam_write_index++;
        int sx = x0 + c * 16;
        ppu.oam[oi].x = (uint8_t)(sx & 0xFF);
        ppu.oam[oi].y = (uint8_t)((y - 1) & 0xFF);
        ppu.oam[oi].tile = (uint8_t)(RELOADED_TILE_BASE + c * 2);
        ppu.oam[oi].attr = 0x30 | (1 << 1) | 1;      /* priority 3, palette group 9, name table 1 */
        ppu.oam_full_x[oi] = (int16_t)sx;
        ppu.oam_full_y[oi] = (int16_t)(y - 1);
        uint16_t hi = oi >> 2;
        uint8_t pos = (uint8_t)((oi & 3) * 2);
        ppu.oam_hi[hi] &= (uint8_t)~(0x03 << pos);
        ppu.oam_hi[hi] |= (uint8_t)((2 | ((sx >> 8) & 1)) << pos);   /* large (16x16) */
    }
}

/* ---- GAME_MODE_TITLE_SCREEN -----------------------------------------------
 * Run-to-completion port of show_title_screen()'s warm-up loop, input/
 * actionscript wait, and fade-out. See the TitleScreenState comment in
 * mode_stack.h. The single yield is owned by the pump; this body never calls
 * wait_for_vblank() (the warm-up/input frames use render_frame_tick_work()). */

/* The manual fade-out + exit cleanup, run as its own phase and also called
 * inline when TS_INPUT decides to exit (so there is no extra yield between the
 * decision and the first fade frame). Displays brightness 0x0F..1 for four
 * frames each, then force-blanks — matching the existing manual loop exactly. */
static StepResult title_fadeout_step(TitleScreenState *s) {
    if (s->fade_delay_left > 0) {
        s->fade_delay_left--;
        return STEP_RESULT_CONTINUE();
    }
    if (s->fade_b == 0) {
        ppu.inidisp = 0x80;
        ppu.bg_viewport_fill[0] = BG_VIEWPORT_CENTER;
        ppu.sprite_x_offset = 0;
        ppu.sprite_y_offset = 0;
        screen_overlay_hook = NULL;
        ert.actionscript_state = 0;
        setup_entity_color_math();
        entity_system_init();
        return STEP_RESULT_POP(s->result);
    }
    ppu.inidisp = s->fade_b;
    s->fade_b--;
    s->fade_delay_left = 3; /* this step yields once; three more follow = 4/level */
    return STEP_RESULT_CONTINUE();
}

StepResult mode_step_title_screen(ModeState *st) {
    TitleScreenState *s = &st->title_screen;

    /* Resume after a parked actionscript frame (D4b): finish the render, then run
     * the exact post-render tail of whichever site parked. */
    if (s->flush == 1) {        /* TS_WARMUP render parked */
        s->flush = 0;
        render_frame_tick_work_flush();
        if (++s->frame >= 60)
            s->phase = TS_INPUT;
        return STEP_RESULT_CONTINUE();
    }
    if (s->flush == 2) {        /* TS_INPUT render parked */
        s->flush = 0;
        render_frame_tick_work_flush();
        return STEP_RESULT_CONTINUE();
    }

    switch ((TitleScreenPhase)s->phase) {
    case TS_WARMUP:
        /* Non-quick (post-attract) reveal: title_screen_setup left the screen
         * force-blanked so the BG palette loader's brief full-brightness write
         * isn't shown. By the third warm-up frame UPDATE_MAP_PALETTE_ANIMATION's
         * fade has pulled the logo down to black and is ramping it up, so turn the
         * screen on now — no full-brightness "EARTHBOUND" flash. (Quick mode uses
         * the NMI brightness fade and never force-blanks here.) */
        if (!s->quick_mode && s->frame >= 2)
            ppu.inidisp = 0x0F;

        if (s->quick_mode) {
            if (fade_active()) fade_update();
        } else {
            /* Sprite-palette lerp (group 8, colors 128-143) toward the saved
             * fade target, re-derived from ert.buffer each frame. */
            PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
            for (int c = 128; c < 144; c++) {
                uint16_t target = fade->target[c];
                uint8_t tr = target & 0x1F;
                uint8_t tg = (target >> 5) & 0x1F;
                uint8_t tb = (target >> 10) & 0x1F;
                uint8_t cr = (uint8_t)((tr * (s->frame + 1)) / 60);
                uint8_t cg = (uint8_t)((tg * (s->frame + 1)) / 60);
                uint8_t cb = (uint8_t)((tb * (s->frame + 1)) / 60);
                ert.palettes[c] = (uint16_t)cr | ((uint16_t)cg << 5) |
                                  ((uint16_t)cb << 10);
            }
            ert.palette_upload_mode = PALETTE_UPLOAD_FULL;
            update_map_palette_animation();
        }
        if (render_frame_tick_work_step()) {
            s->flush = 1;
            return actionscript_frame_take_push();
        }
        if (++s->frame >= 60)
            s->phase = TS_INPUT;
        return STEP_RESULT_CONTINUE();

    case TS_INPUT:
        /* @CHECK_ACTIONSCRIPT: state 1 -> exit to attract (result 0). State 0/2
         * keep looping. (Input + state are read post-yield, at the top.) */
        if (ert.actionscript_state != 0 && ert.actionscript_state != 2) {
            s->result = 0;
            s->phase = TS_FADEOUT;
            s->fade_b = 0x0F;
            s->fade_delay_left = 0;
            return title_fadeout_step(s);
        }
        /* @INPUT_LOOP: any button exits to file select (result 1). */
        if (platform_input_get_pad_new() & PAD_ANY_BUTTON) {
            s->result = 1;
            s->phase = TS_FADEOUT;
            s->fade_b = 0x0F;
            s->fade_delay_left = 0;
            return title_fadeout_step(s);
        }
        /* @NO_BUTTON: run one frame. */
        if (render_frame_tick_work_step()) {
            s->flush = 2;
            return actionscript_frame_take_push();
        }
        return STEP_RESULT_CONTINUE();

    case TS_FADEOUT:
        return title_fadeout_step(s);
    }
    return STEP_RESULT_POP(s->result);
}

/*
 * Port of SHOW_TITLE_SCREEN (asm/intro/show_title_screen.asm).
 *
 * Now uses the entity system for:
 *   - Palette loading and fade (via entity event scripts)
 *   - Letter sprite fly-in animation (via entity movement + draw callbacks)
 *   - State management (ACTIONSCRIPT_STATE signals when animation completes)
 *
 * Returns: 0 = timeout (attract mode), 1 = button pressed
 */
void title_screen_setup(uint16_t quick_mode) {
    /* ROM: JSL FORCE_BLANK_AND_WAIT_VBLANK */
    ppu.inidisp = 0x80;

    /* Script data is loaded in init_intro() — available for both
     * gas station (EVENT_860) and title screen (EVENT_BATTLE_FX through TITLE_SCREEN_11, 787-798). */

    /* ROM: JSL INIT_ENTITY_SYSTEM */
    entity_system_init();

    /* Clear color math registers — may have residual values from
     * setup_entity_color_math() at previous title screen exit or
     * from attract mode entity scripts. The assembly relies on
     * SNES hardware prevent_mode handling; the C port must clear. */
    ppu.cgadsub = 0;
    ppu.cgwsel = 0;
    ppu.coldata_r = 0;
    ppu.coldata_g = 0;
    ppu.coldata_b = 0;

    /* ROM: LDA #11 / JSL SET_BGMODE
     * $0B = mode 3 (8bpp BG1, 4bpp BG2) + BG3 priority bit */
    ppu.bgmode = 0x0B;

    reloaded_frame = 0;
    screen_overlay_hook = reloaded_overlay;

    /* ROM: LDA #3 / JSL SET_OAM_SIZE
     * $03: OBJ name base at VRAM word $6000, size mode 0 (8x8/16x16) */
    ppu.obsel = 0x03;

    /* ROM: SET_BG1_VRAM_LOCATION(0,$5800,0)
     * BG1 tile data at word $0000, tilemap at word $5800, 32x32 screen */
    ppu.bg_sc[0] = 0x58;
    ppu.bg_nba[0] = (ppu.bg_nba[0] & 0xF0) | 0x00;

    /* ROM: STZ BG1_X_POS / STZ BG1_Y_POS / etc. */
    ppu.bg_hofs[0] = 0; ppu.bg_vofs[0] = 0;
    ppu.bg_hofs[1] = 0; ppu.bg_vofs[1] = 0;
    ppu.bg_hofs[2] = 0; ppu.bg_vofs[2] = 0;

    /* ROM: JSL LOAD_TITLE_SCREEN_GRAPHICS */
    load_title_screen_graphics();

    /* ROM: LDA #$11 / STA TM_MIRROR — enable BG1 + OBJ on main screen */
    ppu.tm = 0x11;
    ppu.ts = 0x00;

    /* Extend BG1 edges into the border area (clamp, no tilemap wrapping).
     * This avoids glow/letter wraparound while filling the borders with
     * the dark space color from the image edges. */
    ppu.bg_viewport_fill[0] = BG_VIEWPORT_CLAMP;

    /* Shift sprites to match the centered BG.  Non-filling BGs are rendered
     * at SNES_WIDTH and then centered in the viewport (temp_nf path), so
     * sprite OAM X values (designed for 256px) need the same offset. */
    ppu.sprite_x_offset = EB_VIEWPORT_PAD_LEFT;

    /* Vertically center too: in wide mode (CLAMP keeps it active) the BG and
     * OBJ vertical position is driven solely by sprite_y_offset, so without
     * this the logo sits top-aligned with the bottom gutter clamp-filled.
     * CLAMP still edge-extends the top/bottom gutters (dark space). Reset to 0
     * on exit so the following attract-mode overworld (camera-centered) isn't
     * double-shifted. Matches gas_station.c / file_select.c. */
    ppu.sprite_y_offset = EB_VIEWPORT_PAD_TOP;

    /* ROM: JSL OAM_CLEAR */
    memset(ppu.oam, 0, sizeof(ppu.oam));
    memset(ppu.oam_hi, 0, sizeof(ppu.oam_hi));
    for (int i = 0; i < 128; i++) {
        ppu.oam[i].y = EB_VIEWPORT_HEIGHT;
        ppu.oam_full_y[i] = EB_VIEWPORT_HEIGHT;
    }

    /* Store quick_mode flag for entity scripts to read */
    ert.title_screen_quick_mode = quick_mode;

    /* ROM (show_title_screen.asm:61-64): JSL INIT_ENTITY_WIPE with TITLE_SCREEN_1
     * This spawns the title screen sequencer entity (TITLE_SCREEN_1, script 788),
     * which in turn spawns palette loader (TITLE_SCREEN_2, 789) and letter entities (790-798).
     * NOTE: EVENT_BATTLE_FX (787) is spawned by FILE_SELECT_INIT, NOT here. */
    entity_init_wipe(EVENT_SCRIPT_TITLE_SCREEN_1);

    /* ROM line 65: STZ ACTIONSCRIPT_STATE — clear after entity wipe.
     * entity_system_init already set it to 0, but the assembly explicitly
     * clears it again here as a safety measure. */
    ert.actionscript_state = 0;

    /* Non-quick path: load sprite palette and set up initial fade */
    if (!quick_mode) {
        /* ROM (lines 68-95):
         *   1. Zero all PALETTES
         *   2. Set INIDISP to $0F (full brightness, screen on)
         *   3. Decompress sprite palette (E1AE7C) → PALETTES+256
         *   4. COPY_FADE_BUFFER_TO_PALETTES (save to BUFFER)
         *   5. Zero PALETTES again
         *   6. PREPARE_PALETTE_FADE(60, $0100) — 60-frame sprite palette fade
         *   7. 60-frame loop: UPDATE_MAP_PALETTE_ANIMATION + RUN_ACTIONSCRIPT_FRAME */

        /* Zero all ert.palettes */
        memset(ert.palettes, 0, sizeof(ert.palettes));

        /* ROM sets INIDISP=$0F here, but the title entity palette loader
         * (TITLE_SCREEN_2) writes the BG palette at FULL brightness for a frame
         * or two before UPDATE_MAP_PALETTE_ANIMATION's fade pulls it down to
         * black — on the SNES the NMI fade masks that, but the C port's mode
         * pump renders those warm-up frames, flashing the EARTHBOUND logo at
         * full brightness when returning from attract mode. Keep the screen
         * force-blanked (0x80, set above) and turn it on in TS_WARMUP once the
         * fade has dimmed the palette (see mode_step_title_screen). */

        /* Load sprite palette to upper half (colors 128-255) */
        size_t comp_size;
        comp_size = ASSET_SIZE(ASSET_E1AE7C_BIN_LZHAL);
        const uint8_t *comp_data = ASSET_DATA(ASSET_E1AE7C_BIN_LZHAL);
        if (comp_data) {
            /* Decompress directly to ert.palettes[128..255] (upper half) */
            decomp(comp_data, comp_size, (uint8_t *)&ert.palettes[128], 0x100);
        }

        /* Save current palette to BUFFER as fade target */
        PaletteFadeBuffer *fade = buf_palette_fade(ert.buffer);
        memcpy(fade->target, ert.palettes, sizeof(fade->target));

        /* Zero ert.palettes again for fade-from-black */
        memset(ert.palettes, 0, sizeof(ert.palettes));

        /* PREPARE_PALETTE_FADE(60, $0100) — fade sprite palette group 8 */
        /* $0100 = bit 8 set = palette group 8 (colors 128-143) */
        /* Actually, let's just fade all sprite palette entries for correctness */
        /* In the ROM: mask=$0100 means only palette group 8.
         * But entity events will also set up the BG palette fade.
         * For now, we set up the sprite palette fade. */

        /* The entity system will handle BG palette fade via LOAD_TITLE_SCREEN_PALETTE.
         * We do a simple 60-frame sprite palette fade here. */

        /* Copy fade target to ert.buffer[0..511], set up slopes */
        /* The fade is from black (current=0) to the saved sprite palette */
        /* Use the full fade engine for groups that have data */
        {
            /* Re-save: fade->target already has the target from above */
            /* Accumulators start at 0 (current palette is 0) */
            palette_fade_clear_slopes(fade);

            /* Compute slopes for palette group 8 (colors 128-143) */
            for (int i = 128; i < 144; i++) {
                uint16_t target = fade->target[i];
                int16_t t_r = target & 0x1F;
                int16_t t_g = (target >> 5) & 0x1F;
                int16_t t_b = (target >> 10) & 0x1F;

                /* slope = (target << 8) / 60 (current is 0) */
                fade->slope_r[i] = (int16_t)((t_r << 8) / 60);
                fade->slope_g[i] = (int16_t)((t_g << 8) / 60);
                fade->slope_b[i] = (int16_t)((t_b << 8) / 60);
            }
        }

        /* The 60-frame sprite-palette warm-up — and the @INPUT_LOOP /
         * @CHECK_ACTIONSCRIPT wait and the closing fade-out — now run as
         * GAME_MODE_TITLE_SCREEN (TS_WARMUP body re-derives `fade` from
         * ert.buffer each frame; slopes/target were set up above). */

        /* Push the just-zeroed palette to CGRAM now (the SNES NMI uploads the
         * PALETTES mirror every vblank). Without this, the single host frame that
         * renders between this setup and TS_WARMUP's first render (the push-yield)
         * shows the title BG tiles under the PREVIOUS scene's stale CGRAM — e.g.
         * the bright "EARTHBOUND" glow flashes for a frame when returning from
         * attract mode. inidisp is already $0F (screen on), so the palette must be
         * black here for the transition frame to stay dark. */
        sync_palettes_to_cgram();
    } else {
        /* Quick mode (asm @QUICK_MODE): NMI-driven brightness fade runs
         * concurrently with a 60-frame RENDER_FRAME_TICK warm-up loop.
         * Entity scripts load the correct palette via LOAD_FILE_SELECT_PALETTES
         * during this warm-up.  The brightness fade (starting from force blank)
         * keeps the screen dark until entity scripts have loaded the palette. */

        /* ROM: LDX #1 / LDA #4 / JSL FADE_IN — non-blocking (NMI-driven on the
         * SNES); the warm-up mode advances it each frame via fade_update(). */
        fade_in(4, 1);
    }
}

