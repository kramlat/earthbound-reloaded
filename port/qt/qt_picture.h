#pragma once
/* The picture's way onto the screen: the GPU through Qt's RHI (Vulkan, or OpenGL where Vulkan
 * will not start) or the CPU through QPainter as the last resort. Same contract as the Sonic
 * ports' PictureView: the frame is scaled up to fit the widget, aspect kept, pixels left sharp.
 * EB_DRAWING=vulkan|opengl|cpu asks for one (a GPU mode that cannot start still falls back). */
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QWidget>

enum class PictureDrawing { Vulkan, OpenGL, Cpu };

PictureDrawing Picture_Drawing();
const char *Picture_DrawingName(PictureDrawing drawing);
/* the scenery shift for a smooth frame, in picture pixels, rounded to whole screen pixels */
QPointF Picture_SmoothShift(QPointF frac, double scale);
QRectF Picture_Fit(const QSizeF &widget, const QSize &picture);

class PictureSurface {
public:
    virtual ~PictureSurface() = default;
    virtual QWidget *widget() = 0;
    virtual void setFrame(const QImage &frame) = 0;   /* RGBA8888 */
    /* A smooth-camera frame: `frame` is twice the picture's height, the scenery above and the party's pixels
     * (alpha 0 where there are none) below; frac is the leader's sub-pixel position, 0..1 */
    virtual void setSmoothFrame(const QImage &frame, QPointF frac) = 0;
    /* Physical screen pixels per picture pixel when a picture of this size is shown now */
    double scaleFor(QSize picture);
    virtual QImage grabScreen() = 0;                  /* what the widget shows now, as drawn (EB_QT_GRAB) */
};
PictureSurface *Picture_Make(QWidget *parent);
