#include "sdl2_touch.h"
#include "include/pad.h"
#include "core/types.h"
#include <stdlib.h>
#include <string.h>

enum { ROLE_NONE = 0, ROLE_DPAD, ROLE_BUTTON, ROLE_WORLD };
#define MAX_FINGERS 10

typedef struct {
    bool down;
    SDL_FingerID id;
    int role;
    int x, y;
    int button;        /* ROLE_BUTTON: index into buttons[] */
} Finger;

static Finger fingers[MAX_FINGERS];
static bool visible;       /* touch has been used since the keyboard/mouse/pad last were */
static bool disabled;      /* EB_TOUCH=0 */

typedef struct {
    const char *label;
    uint16_t pad;
    int dx, dy;            /* from the bottom-right corner (negative = left/up), or bottom centre if centre */
    bool centre;
    int radius;
} Button;

/* A (menu / confirm), B (cancel / HP-PP), X (town map), then L (check), R and Start along the bottom. */
static const Button buttons[] = {
    { "A", PAD_A,      -22, -46, false, 12 },
    { "B", PAD_B,      -50, -30, false, 12 },
    { "X", PAD_X,      -22, -18, false, 12 },
    { "L", PAD_L,      -44,  -9, true,  10 },
    { "R", PAD_R,        0,  -9, true,  10 },
    { "S", PAD_START,   44,  -9, true,  10 },
    { "LT", PAD_LT,    -88,  -9, true,  10 },
    { "RT", PAD_RT,     88,  -9, true,  10 },
};
#define BUTTON_COUNT ((int)(sizeof(buttons) / sizeof(buttons[0])))

#define DPAD_R      34     /* outer radius */
#define DPAD_DEAD   9

static void dpad_center(int *cx, int *cy) {
    *cx = 40;
    *cy = EB_VIEWPORT_HEIGHT - 40;
}

static void button_center(int i, int *cx, int *cy) {
    *cx = buttons[i].centre ? EB_VIEWPORT_WIDTH / 2 + buttons[i].dx : EB_VIEWPORT_WIDTH + buttons[i].dx;
    *cy = EB_VIEWPORT_HEIGHT + buttons[i].dy;
}

void touch_init(void) {
    const char *env = getenv("EB_TOUCH");
    if (env && env[0] == '0') disabled = true;
    if (env && env[0] == '1') visible = true;
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");     /* fingers are handled here, not as a second mouse */
}

static int hit_button(int x, int y) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        int cx, cy;
        button_center(i, &cx, &cy);
        int r = buttons[i].radius + 4;                  /* a little slack for a thumb */
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return i;
    }
    return -1;
}

static bool in_dpad(int x, int y) {
    int cx, cy;
    dpad_center(&cx, &cy);
    int r = DPAD_R + 10;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

void touch_finger(int id, int px, int py, int phase) {
    if (disabled) return;
    visible = true;
    Finger *f = NULL;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (fingers[i].down && fingers[i].id == id) f = &fingers[i];
    if (phase == 0) {
        if (!f)
            for (int i = 0; i < MAX_FINGERS; i++)
                if (!fingers[i].down) { f = &fingers[i]; break; }
        if (!f) return;
        memset(f, 0, sizeof(*f));
        f->down = true;
        f->id = id;
        f->x = px;
        f->y = py;
        int b = hit_button(px, py);
        if (in_dpad(px, py)) {
            f->role = ROLE_DPAD;
        } else if (b >= 0) {
            f->role = ROLE_BUTTON;
            f->button = b;
        } else {
            f->role = ROLE_WORLD;                       /* a tap on the picture is a mouse click */
            platform_mouse_script(px, py, true, false);
        }
    } else if (f && phase == 1) {
        f->x = px;
        f->y = py;
        if (f->role == ROLE_WORLD)
            platform_mouse_script(px, py, false, false);   /* pointing, so menus follow the finger */
    } else if (f && phase == 2) {
        f->down = false;
    }
}

bool touch_handle_event(const SDL_Event *e) {
    if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERMOTION && e->type != SDL_FINGERUP)
        return false;
    int px, py;
    if (platform_video_finger_to_picture(e->tfinger.x, e->tfinger.y, &px, &py))
        touch_finger((int)e->tfinger.fingerId, px, py,
                     e->type == SDL_FINGERDOWN ? 0 : e->type == SDL_FINGERMOTION ? 1 : 2);
    return true;
}

uint16_t touch_pad(void) {
    uint16_t pad = 0;
    int cx, cy;
    dpad_center(&cx, &cy);
    for (int i = 0; i < MAX_FINGERS; i++) {
        const Finger *f = &fingers[i];
        if (!f->down) continue;
        if (f->role == ROLE_BUTTON) {
            pad |= buttons[f->button].pad;
        } else if (f->role == ROLE_DPAD) {
            int dx = f->x - cx, dy = f->y - cy;
            int ax = abs(dx), ay = abs(dy);
            if (ax < DPAD_DEAD && ay < DPAD_DEAD) continue;
            if (ax * 5 >= ay * 2) pad |= dx > 0 ? PAD_RIGHT : PAD_LEFT;   /* diagonals when both axes count */
            if (ay * 5 >= ax * 2) pad |= dy > 0 ? PAD_DOWN : PAD_UP;
        }
    }
    return pad;
}

void touch_not_in_use(void) {
    const char *env = getenv("EB_TOUCH");
    if (env && env[0] == '1') return;
    visible = false;
}

/* ---- overlay ----------------------------------------------------------------------------- */

static void blend(pixel_t *fb, int pitch, int x, int y, pixel_t color) {
    if (x < 0 || y < 0 || x >= EB_VIEWPORT_WIDTH || y >= EB_VIEWPORT_HEIGHT) return;
    pixel_t *p = (pixel_t *)((uint8_t *)fb + (size_t)y * pitch) + x;
    *p = (pixel_t)(((*p >> 1) & 0x7BEF) + ((color >> 1) & 0x7BEF));
}

static void disc(pixel_t *fb, int pitch, int cx, int cy, int r, pixel_t color) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) blend(fb, pitch, cx + x, cy + y, color);
}

/* 5x7 glyphs for the button labels */
static const uint8_t glyph_A[7] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
static const uint8_t glyph_B[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
static const uint8_t glyph_X[7] = { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 };
static const uint8_t glyph_L[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
static const uint8_t glyph_R[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
static const uint8_t glyph_S[7] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E };
static const uint8_t glyph_T[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };

static const uint8_t *glyph_for(char c) {
    switch (c) {
    case 'A': return glyph_A;
    case 'B': return glyph_B;
    case 'X': return glyph_X;
    case 'L': return glyph_L;
    case 'R': return glyph_R;
    case 'T': return glyph_T;
    default:  return glyph_S;
    }
}

static void glyph(pixel_t *fb, int pitch, int cx, int cy, char c, pixel_t color) {
    const uint8_t *g = glyph_for(c);
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (g[row] & (0x10 >> col)) {
                blend(fb, pitch, cx - 2 + col, cy - 3 + row, color);
                blend(fb, pitch, cx - 2 + col, cy - 3 + row, color);   /* twice: more opaque than the discs */
            }
}

void touch_draw(pixel_t *fb, int pitch) {
    if (!visible || disabled || !fb) return;
    const pixel_t white = PIXEL_RGB(255, 255, 255), lit = PIXEL_RGB(255, 230, 120), dark = PIXEL_RGB(40, 30, 80);
    uint16_t held = touch_pad();

    int cx, cy;
    dpad_center(&cx, &cy);
    disc(fb, pitch, cx, cy, DPAD_R, dark);
    /* four arms; an arm lights while its direction is held */
    static const struct { int dx, dy; uint16_t pad; } arms[] = {
        { 0, -1, PAD_UP }, { 0, 1, PAD_DOWN }, { -1, 0, PAD_LEFT }, { 1, 0, PAD_RIGHT },
    };
    for (int a = 0; a < 4; a++) {
        pixel_t c = (held & arms[a].pad) ? lit : white;
        int ax = cx + arms[a].dx * 22, ay = cy + arms[a].dy * 22;
        for (int t = 0; t < 8; t++)                      /* a small triangle pointing outwards */
            for (int s = -t; s <= t; s++) {
                int px = ax + (arms[a].dx ? -arms[a].dx * (8 - t) + arms[a].dx * 4 : s);
                int py = ay + (arms[a].dy ? -arms[a].dy * (8 - t) + arms[a].dy * 4 : s);
                blend(fb, pitch, px, py, c);
            }
    }

    for (int i = 0; i < BUTTON_COUNT; i++) {
        int bx, by;
        button_center(i, &bx, &by);
        bool on = (held & buttons[i].pad) != 0;
        disc(fb, pitch, bx, by, buttons[i].radius, on ? lit : dark);
        const char *lab = buttons[i].label;
        int n = (int)strlen(lab);
        for (int k = 0; k < n; k++)                       /* labels are one or two letters */
            glyph(fb, pitch, bx - (n - 1) * 3 + k * 6, by, lab[k], on ? dark : white);
    }
}
