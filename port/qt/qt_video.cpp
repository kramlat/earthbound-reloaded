/* The Qt host's window, picture and event bridge.
 *
 * Video: the PPU's scanlines land in a BGR565 frame buffer (as with the SDL build); at the end
 * of the frame it is expanded to RGBA and handed to the picture surface (qt_picture.cpp), which
 * draws it with Vulkan or OpenGL through Qt's RHI, or with QPainter as the last resort.
 *
 * Input: the keyboard/mouse/touch/gamepad code is shared with the SDL build and reads SDL
 * events, so this window turns its Qt events into SDL events (SDL_PushEvent). Gamepads come
 * from SDL's game controller subsystem; SDL never opens a window here. */
#include <QApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QResizeEvent>
#include <QSurfaceFormat>
#include <QTouchEvent>
#include <QWidget>

#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include "platform/platform.h"
#include "snes/ppu.h"
#include "game/smooth_camera.h"
}
#include "qt_host.h"
#include "qt_picture.h"

#ifndef EB_WINDOW_SCALE
#define EB_WINDOW_SCALE 3
#endif

extern "C" {
void touch_draw(pixel_t *fb, int pitch_bytes);
bool platform_video_finger_to_picture(float nx, float ny, int *px, int *py);
int video_frames_rendered;
const pixel_t *video_last_frame(void);
bool platform_display_fullscreen(void);
int platform_display_aspect(void);
void platform_video_set_fullscreen(bool fullscreen);
}

namespace {

int g_argc = 1;
char g_name[] = "earthbound";
char *g_default_argv[] = { g_name, nullptr };
char **g_argv = g_default_argv;
bool g_vsync = true;

pixel_t g_fb[EB_VIEWPORT_MAX_WIDTH * EB_VIEWPORT_MAX_HEIGHT];   /* the frame being drawn, and the last one for dumps */
/* Smooth camera (presentation only; see game/smooth_camera.h): an eligible frame is rendered twice,
 * once normally (which also records which pixels the party's sprites won) and once with the party's
 * OAM entries parked off-screen (scenery only). The picture surface draws the scenery shifted by the
 * leader's sub-pixel fraction, rounded to whole screen pixels, with the party's pixels unshifted on top. */
pixel_t g_scenery[EB_VIEWPORT_MAX_WIDTH * EB_VIEWPORT_MAX_HEIGHT];
uint8_t g_party_mask[EB_VIEWPORT_MAX_WIDTH * EB_VIEWPORT_MAX_HEIGHT];
bool g_smooth_pending;
uint16_t g_smooth_frac_x, g_smooth_frac_y;
scanline_stamp_cb_t g_stamp_cb;

QImage g_image[2];
int g_image_next;
uint32_t g_lut[65536];                                          /* BGR565 -> RGBA8888 bytes */

void BuildLut() {
    for (int p = 0; p < 65536; p++) {
        const uint32_t b = (p >> 11) & 31, g = (p >> 5) & 63, r = p & 31;
        const uint32_t r8 = (r << 3) | (r >> 2), g8 = (g << 2) | (g >> 4), b8 = (b << 3) | (b >> 2);
        g_lut[p] = r8 | (g8 << 8) | (b8 << 16) | 0xFF000000u;   /* bytes R,G,B,A in memory (little endian) */
    }
}

SDL_Scancode ScancodeFor(const QKeyEvent *e) {
    const int k = e->key();
    if (k >= Qt::Key_A && k <= Qt::Key_Z) return SDL_Scancode(SDL_SCANCODE_A + (k - Qt::Key_A));
    if (k >= Qt::Key_1 && k <= Qt::Key_9) return SDL_Scancode(SDL_SCANCODE_1 + (k - Qt::Key_1));
    if (k == Qt::Key_0) return SDL_SCANCODE_0;
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12) return SDL_Scancode(SDL_SCANCODE_F1 + (k - Qt::Key_F1));
    /* The two shift/ctrl/alt keys are one Qt key; Linux's native scan code (evdev + 8) tells them apart. */
    const quint32 native = e->nativeScanCode();
    const bool right = native == 62 || native == 105 || native == 108;
    switch (k) {
    case Qt::Key_Up: return SDL_SCANCODE_UP;
    case Qt::Key_Down: return SDL_SCANCODE_DOWN;
    case Qt::Key_Left: return SDL_SCANCODE_LEFT;
    case Qt::Key_Right: return SDL_SCANCODE_RIGHT;
    case Qt::Key_Return: return SDL_SCANCODE_RETURN;
    case Qt::Key_Enter: return SDL_SCANCODE_KP_ENTER;
    case Qt::Key_Escape: return SDL_SCANCODE_ESCAPE;
    case Qt::Key_Tab: case Qt::Key_Backtab: return SDL_SCANCODE_TAB;
    case Qt::Key_Backspace: return SDL_SCANCODE_BACKSPACE;
    case Qt::Key_Space: return SDL_SCANCODE_SPACE;
    case Qt::Key_Shift: return right ? SDL_SCANCODE_RSHIFT : SDL_SCANCODE_LSHIFT;
    case Qt::Key_Control: return right ? SDL_SCANCODE_RCTRL : SDL_SCANCODE_LCTRL;
    case Qt::Key_Alt: return right ? SDL_SCANCODE_RALT : SDL_SCANCODE_LALT;
    case Qt::Key_Insert: return SDL_SCANCODE_INSERT;
    case Qt::Key_Delete: return SDL_SCANCODE_DELETE;
    case Qt::Key_Home: return SDL_SCANCODE_HOME;
    case Qt::Key_End: return SDL_SCANCODE_END;
    case Qt::Key_PageUp: return SDL_SCANCODE_PAGEUP;
    case Qt::Key_PageDown: return SDL_SCANCODE_PAGEDOWN;
    case Qt::Key_Comma: return SDL_SCANCODE_COMMA;
    case Qt::Key_Period: return SDL_SCANCODE_PERIOD;
    case Qt::Key_Slash: return SDL_SCANCODE_SLASH;
    case Qt::Key_Semicolon: return SDL_SCANCODE_SEMICOLON;
    case Qt::Key_Apostrophe: return SDL_SCANCODE_APOSTROPHE;
    case Qt::Key_Minus: return SDL_SCANCODE_MINUS;
    case Qt::Key_Equal: return SDL_SCANCODE_EQUALS;
    case Qt::Key_BracketLeft: return SDL_SCANCODE_LEFTBRACKET;
    case Qt::Key_BracketRight: return SDL_SCANCODE_RIGHTBRACKET;
    case Qt::Key_Backslash: return SDL_SCANCODE_BACKSLASH;
    case Qt::Key_QuoteLeft: return SDL_SCANCODE_GRAVE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

class HostWindow final : public QWidget {
public:
    HostWindow() {
        setWindowTitle("EarthBound");
        setMouseTracking(true);
        setAttribute(Qt::WA_AcceptTouchEvents);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        picture.reset(Picture_Make(this));
        picture->widget()->setGeometry(rect());
    }
    PictureSurface *surface() { return picture.get(); }

    /* Where a point in the window falls in the picture, in picture pixels (may lie outside it, in the bars). */
    void ToPicture(const QPointF &p, int *px, int *py) const {
        const QSize pic(eb_viewport_width, eb_viewport_height);
        const QRectF fit = Picture_Fit(QSizeF(size()), pic);
        if (fit.isEmpty()) { *px = *py = 0; return; }
        *px = int((p.x() - fit.x()) * pic.width() / fit.width() - (p.x() < fit.x() ? 1 : 0));
        *py = int((p.y() - fit.y()) * pic.height() / fit.height() - (p.y() < fit.y() ? 1 : 0));
    }

protected:
    void resizeEvent(QResizeEvent *) override {
        picture->widget()->setGeometry(rect());
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_WINDOWEVENT;
        ev.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
        SDL_PushEvent(&ev);
    }
    void closeEvent(QCloseEvent *e) override {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_QUIT;
        SDL_PushEvent(&ev);
        e->ignore();            /* the game loop ends; main() shuts everything down in order */
    }
    void keyPressEvent(QKeyEvent *e) override { Key(e, true); }
    void keyReleaseEvent(QKeyEvent *e) override { Key(e, false); }
    void mouseMoveEvent(QMouseEvent *e) override {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_MOUSEMOTION;
        ToPicture(e->position(), &ev.motion.x, &ev.motion.y);
        SDL_PushEvent(&ev);
    }
    void mousePressEvent(QMouseEvent *e) override { Button(e, SDL_MOUSEBUTTONDOWN); }
    void mouseReleaseEvent(QMouseEvent *e) override { Button(e, SDL_MOUSEBUTTONUP); }
    bool event(QEvent *e) override {
        switch (e->type()) {
        case QEvent::TouchBegin: case QEvent::TouchUpdate: case QEvent::TouchEnd: case QEvent::TouchCancel:
            Touch(static_cast<QTouchEvent *>(e));
            e->accept();
            return true;
        default:
            return QWidget::event(e);
        }
    }

private:
    void Key(QKeyEvent *e, bool down) {
        const SDL_Scancode sc = ScancodeFor(e);
        if (sc != SDL_SCANCODE_UNKNOWN) {
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
            ev.key.repeat = e->isAutoRepeat() ? 1 : 0;
            ev.key.keysym.scancode = sc;
            ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
            SDL_PushEvent(&ev);
        }
        if (down) {   /* typed characters, for name entry */
            const QByteArray text = e->text().toUtf8();
            if (!text.isEmpty() && text.size() < int(sizeof(SDL_TextInputEvent::text))) {
                SDL_Event ev;
                SDL_zero(ev);
                ev.type = SDL_TEXTINPUT;
                memcpy(ev.text.text, text.constData(), size_t(text.size()));
                SDL_PushEvent(&ev);
            }
        }
        e->accept();
    }
    void Button(QMouseEvent *e, Uint32 type) {
        if (e->source() != Qt::MouseEventNotSynthesized)
            return;
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = type;
        ev.button.button = e->button() == Qt::LeftButton ? SDL_BUTTON_LEFT
                         : e->button() == Qt::RightButton ? SDL_BUTTON_RIGHT : 0;
        if (!ev.button.button) return;
        ev.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
        ToPicture(e->position(), &ev.button.x, &ev.button.y);
        SDL_PushEvent(&ev);
    }
    void Touch(QTouchEvent *e) {
        for (const QEventPoint &p : e->points()) {
            SDL_Event ev;
            SDL_zero(ev);
            switch (p.state()) {
            case QEventPoint::Pressed: ev.type = SDL_FINGERDOWN; break;
            case QEventPoint::Released: ev.type = SDL_FINGERUP; break;
            case QEventPoint::Updated: ev.type = SDL_FINGERMOTION; break;
            default: continue;
            }
            ev.tfinger.fingerId = p.id();
            ev.tfinger.x = float(p.position().x() / qMax(1, width()));
            ev.tfinger.y = float(p.position().y() / qMax(1, height()));
            SDL_PushEvent(&ev);
        }
    }

    std::unique_ptr<PictureSurface> picture;
};

QApplication *g_app;   /* never deleted: Qt Multimedia's exit-time cleanup still needs it */
HostWindow *g_window;

void AspectToSize(int aspect, int *w, int *h) {
    static const int widths[3] = { 320, 384, 424 };
    *h = 240;
    if (aspect == 3) {                 /* fit window: as wide as the window's shape, in whole tiles */
        const int ww = g_window ? g_window->width() : 0, wh = g_window ? g_window->height() : 0;
        int fit = (ww > 0 && wh > 0) ? int(240L * ww / wh) : 424;
        fit = (fit + 4) / 8 * 8;
        *w = fit < 320 ? 320 : (fit > EB_VIEWPORT_MAX_WIDTH ? EB_VIEWPORT_MAX_WIDTH : fit);
        return;
    }
    *w = widths[aspect < 0 || aspect > 2 ? 0 : aspect];
}

}   // namespace

extern "C" {

void qt_host_set_args(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

void qt_host_pump(void) {
    if (g_app)
        QCoreApplication::processEvents();
}

const pixel_t *video_last_frame(void) { return g_fb; }

static void SceneryScanline(int y, const pixel_t *pixels) {
    memcpy(&g_scenery[y * eb_viewport_width], pixels, size_t(eb_viewport_width) * sizeof(pixel_t));
}
static void StampedSend(int y, const pixel_t *pixels) {
    g_stamp_cb(y, const_cast<pixel_t *>(pixels));
    platform_video_send_scanline(y, pixels);
}
static void StampedScenery(int y, const pixel_t *pixels) {
    g_stamp_cb(y, const_cast<pixel_t *>(pixels));
    SceneryScanline(y, pixels);
}

/* Replaces the library's default (src/platform/platform_render.c). With the smooth camera off this is
 * exactly the default sequence. */
void platform_render_frame(scanline_stamp_cb_t fps_overlay_cb) {
    static SmoothCameraFrame sf;
    g_stamp_cb = fps_overlay_cb;
    scanline_callback_t send = fps_overlay_cb ? StampedSend : platform_video_send_scanline;

    g_smooth_pending = smooth_camera_enabled && g_window &&
                       g_window->surface()->scaleFor(QSize(eb_viewport_width, eb_viewport_height)) >= 2.0 &&
                       smooth_camera_prepare(&sf);
    platform_video_begin_frame();
    if (!g_smooth_pending) {
        ppu_render_frame(send);
        platform_video_end_frame();
        return;
    }
    ppu_set_obj_mask(sf.party_oam, g_party_mask);
    ppu_render_frame(send);
    ppu_set_obj_mask(nullptr, nullptr);

    /* Scenery pass: park the party's OAM entries below the screen, render, put them back. */
    int16_t saved_y[128];
    for (int i = 0; i < 128; i++) {
        saved_y[i] = ppu.oam_full_y[i];
        if (sf.party_oam[i])
            ppu.oam_full_y[i] = int16_t(eb_viewport_height);
    }
    ppu_render_frame(fps_overlay_cb ? StampedScenery : SceneryScanline);
    memcpy(ppu.oam_full_y, saved_y, sizeof(saved_y));

    g_smooth_frac_x = sf.frac_x;
    g_smooth_frac_y = sf.frac_y;
    platform_video_end_frame();
}

void platform_display_apply_aspect(void) {
    int w, h;
    AspectToSize(platform_display_aspect(), &w, &h);
    if (w == eb_viewport_width && h == eb_viewport_height) return;
    eb_viewport_width = w;
    eb_viewport_height = h;
    if (!g_window) return;
    if (platform_display_aspect() != DISPLAY_ASPECT_FIT && !g_window->isFullScreen())
        g_window->resize(w * EB_WINDOW_SCALE, h * EB_WINDOW_SCALE);
}

void platform_video_set_fullscreen(bool fullscreen) {
    if (!g_window) return;
    if (fullscreen) g_window->showFullScreen(); else g_window->showNormal();
}

bool platform_video_init(void) {
    {   /* the picture size comes from the saved aspect ratio, before anything is drawn */
        int w, h;
        AspectToSize(platform_display_aspect(), &w, &h);
        eb_viewport_width = w;
        eb_viewport_height = h;
    }
    BuildLut();
    if (platform_headless) {
        smooth_camera_enabled = false;     /* presentation only: nothing to smooth without a window */
        return true;                       /* a frame buffer is all the scripted tests need */
    }

    const char *sc = getenv("EB_SMOOTH_CAMERA");
    if (sc && sc[0] && strcmp(sc, "0") != 0)
        smooth_camera_enabled = true;
    const char *v = getenv("EB_QT_VSYNC");
    g_vsync = !(v && strcmp(v, "0") == 0);
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(g_vsync ? 1 : 0);
    QSurfaceFormat::setDefaultFormat(format);

    g_app = new QApplication(g_argc, g_argv);
    g_window = new HostWindow;
    g_window->resize(eb_viewport_width * EB_WINDOW_SCALE, eb_viewport_height * EB_WINDOW_SCALE);
    if (platform_display_fullscreen()) g_window->showFullScreen(); else g_window->show();
    g_window->setFocus();
    qt_host_pump();
    return true;
}

void platform_video_shutdown(void) {
    delete g_window;
    g_window = nullptr;
}

void platform_video_begin_frame(void) {
    video_frames_rendered++;
}

void platform_video_send_scanline(int y, const pixel_t *pixels) {
    memcpy(&g_fb[y * eb_viewport_width], pixels, size_t(eb_viewport_width) * sizeof(pixel_t));
}

pixel_t *platform_video_get_framebuffer(void) { return g_fb; }

bool platform_video_finger_to_picture(float nx, float ny, int *px, int *py) {
    if (!g_window) {                 /* headless: the window is the picture */
        *px = int(nx * eb_viewport_width);
        *py = int(ny * eb_viewport_height);
        return true;
    }
    g_window->ToPicture(QPointF(nx * g_window->width(), ny * g_window->height()), px, py);
    return true;
}

void platform_video_end_frame(void) {
    touch_draw(g_fb, eb_viewport_width * int(sizeof(pixel_t)));   /* the on-screen controls, while touch is in use */
    if (!g_window)
        return;                      /* headless: g_fb stays readable for the debug dump */
    const int w = eb_viewport_width, h = eb_viewport_height;
    QImage &img = g_image[g_image_next ^= 1];
    const QSize want(w, g_smooth_pending ? h * 2 : h);
    if (img.size() != want)
        img = QImage(want, QImage::Format_RGBA8888);
    if (!g_smooth_pending) {
        for (int y = 0; y < h; y++) {
            uint32_t *dst = reinterpret_cast<uint32_t *>(img.scanLine(y));
            const pixel_t *src = &g_fb[y * w];
            for (int x = 0; x < w; x++)
                dst[x] = g_lut[src[x]];
        }
        g_window->surface()->setFrame(img);
    } else {   /* scenery above, the party's pixels (transparent elsewhere) below */
        for (int y = 0; y < h; y++) {
            uint32_t *scenery = reinterpret_cast<uint32_t *>(img.scanLine(y));
            uint32_t *party = reinterpret_cast<uint32_t *>(img.scanLine(y + h));
            for (int x = 0; x < w; x++) {
                scenery[x] = g_lut[g_scenery[y * w + x]];
                party[x] = g_party_mask[y * w + x] ? g_lut[g_fb[y * w + x]] : 0;
            }
        }
        g_window->surface()->setSmoothFrame(img, QPointF(g_smooth_frac_x / 65536.0, g_smooth_frac_y / 65536.0));
        g_smooth_pending = false;
    }
    qt_host_pump();
    static const char *grab = getenv("EB_QT_GRAB");   /* "FRAME:PATH": save what the window shows at that frame, then go on */
    if (grab && video_frames_rendered == atoi(grab)) {
        const char *colon = strchr(grab, ':');
        if (colon) g_window->surface()->grabScreen().save(colon + 1);
    }                  /* draws it (and with vsync, waits for the display) */
}

void platform_video_set_vsync(bool enabled) {
    g_vsync = enabled;               /* Qt fixes the swap interval when the window is made (EB_QT_VSYNC) */
}

}   // extern "C"
