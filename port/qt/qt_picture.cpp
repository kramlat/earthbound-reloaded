#include "qt_picture.h"

#include <QFile>
#include <QGuiApplication>
#include <QLibrary>
#include <QOffscreenSurface>
#include <QPainter>
#include <QRhiWidget>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

/* Whether Vulkan works here: the loader is there, makes an instance, and that instance has a
 * device. Asked through the loader's own entry points so that this builds where Qt has Vulkan
 * but the Vulkan headers are not installed (the same probe the Sonic ports use). */
bool VulkanStarts() {
#if defined(Q_OS_WIN)
    QLibrary loader("vulkan-1");
#elif defined(Q_OS_MACOS)
    QLibrary loader("libvulkan.1.dylib");
#else
    QLibrary loader("vulkan", 1);
#endif
    if (!loader.load())
        return false;
    struct InstanceInfo {   /* VkInstanceCreateInfo */
        int32_t type;       /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 1 */
        const void *next;
        uint32_t flags;
        const void *application;
        uint32_t layers;
        const char *const *layer_names;
        uint32_t extensions;
        const char *const *extension_names;
    };
    using Instance = void *;
    using GetProc = void *(*)(Instance, const char *);
    using Create = int32_t (*)(const InstanceInfo *, const void *, Instance *);
    using Destroy = void (*)(Instance, const void *);
    using Devices = int32_t (*)(Instance, uint32_t *, void **);
    const auto get_proc = reinterpret_cast<GetProc>(loader.resolve("vkGetInstanceProcAddr"));
    if (!get_proc) return false;
    const auto create = reinterpret_cast<Create>(get_proc(nullptr, "vkCreateInstance"));
    if (!create) return false;
    const InstanceInfo info = { 1, nullptr, 0, nullptr, 0, nullptr, 0, nullptr };
    Instance instance = nullptr;
    if (create(&info, nullptr, &instance) != 0 || !instance)
        return false;
    const auto devices = reinterpret_cast<Devices>(get_proc(instance, "vkEnumeratePhysicalDevices"));
    const auto destroy = reinterpret_cast<Destroy>(get_proc(instance, "vkDestroyInstance"));
    uint32_t count = 0;
    const bool found = devices && devices(instance, &count, nullptr) == 0 && count > 0;
    if (destroy) destroy(instance, nullptr);
    return found;
}

bool OpenGLStarts() {
    std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());
    QRhiGles2InitParams params;
    params.fallbackSurface = surface.get();
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &params));
    return rhi != nullptr;
}

QShader LoadShader(const char *name) {
    QFile f(QString(":/eb/shaders/") + name);
    if (!f.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "picture: shader %s missing from the resources\n", name);
        return QShader();
    }
    return QShader::fromSerialized(f.readAll());
}

/* The picture through the GPU: one texture drawn as a quad into the part of the widget it
 * fits, nearest texel per pixel. */
class RhiPicture final : public QRhiWidget, public PictureSurface {
public:
    RhiPicture(Api api, QWidget *parent) : QRhiWidget(parent) {
        setApi(api);
        setAttribute(Qt::WA_TransparentForMouseEvents);   /* the window takes the mouse */
        setFocusPolicy(Qt::NoFocus);
    }
    QWidget *widget() override { return this; }
    void setFrame(const QImage &image) override {
        frame = image;
        smooth = false;
        frame_new = true;
        update();
    }
    void setSmoothFrame(const QImage &image, QPointF f) override {
        frame = image;
        smooth = true;
        frac = f;
        frame_new = true;
        update();
    }
    QImage grabScreen() override { return grabFramebuffer(); }

protected:
    void initialize(QRhiCommandBuffer *) override {
        if (made_for != rhi()) {   /* a new device (the first, or the window moved to another GPU) */
            releaseResources();
            made_for = rhi();
            first_upload = true;
        }
        if (pipeline)
            return;
        vertices.reset(rhi()->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
        vertices->create();
        uniforms.reset(rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 96));
        uniforms->create();
        sampler.reset(rhi()->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        sampler->create();
        MakeTexture(TextureSize());
        bindings.reset(rhi()->newShaderResourceBindings());
        Bind();
        pipeline.reset(rhi()->newGraphicsPipeline());
        pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        pipeline->setShaderStages({ { QRhiShaderStage::Vertex, LoadShader("picture.vert.qsb") },
                                    { QRhiShaderStage::Fragment, LoadShader("picture.frag.qsb") } });
        QRhiVertexInputLayout layout;
        layout.setBindings({ { 2 * sizeof(float) } });
        layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 } });
        pipeline->setVertexInputLayout(layout);
        pipeline->setSampleCount(sampleCount());
        pipeline->setShaderResourceBindings(bindings.get());
        pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        pipeline->create();
    }

    void render(QRhiCommandBuffer *cb) override {
        QRhiResourceUpdateBatch *batch = rhi()->nextResourceUpdateBatch();
        if (first_upload) {
            batch->uploadStaticBuffer(vertices.get(), kQuad);
            first_upload = false;
        }
        const QSize target = renderTarget()->pixelSize();
        if (!frame.isNull()) {   /* the frame's uniforms: the clip space correction and the smooth shift */
            const QSize pic = smooth ? QSize(frame.width(), frame.height() / 2) : frame.size();
            const QRectF fit = Picture_Fit(QSizeF(target), pic);
            const QPointF shift = smooth ? Picture_SmoothShift(frac, fit.width() / pic.width()) : QPointF();
            float u[24];
            memcpy(u, rhi()->clipSpaceCorrMatrix().constData(), 64);
            const float more[8] = { float(shift.x()), float(shift.y()), float(pic.width()), float(pic.height()),
                                    smooth ? 1.0f : 0.0f, 0, 0, 0 };
            memcpy(u + 16, more, sizeof(more));
            batch->updateDynamicBuffer(uniforms.get(), 0, 96, u);
        }
        if (!frame.isNull() && frame_new) {
            if (texture->pixelSize() != TextureSize()) {    /* the aspect ratio changed */
                MakeTexture(TextureSize());
                Bind();
            }
            batch->uploadTexture(texture.get(),
                QRhiTextureUploadDescription({ 0, 0, QRhiTextureSubresourceUploadDescription(frame) }));
            frame_new = false;
        }
        cb->beginPass(renderTarget(), Qt::black, { 1.0f, 0 }, batch);
        if (!frame.isNull()) {
            const QRectF fit = Picture_Fit(QSizeF(target), smooth ? QSize(frame.width(), frame.height() / 2) : frame.size());
            cb->setGraphicsPipeline(pipeline.get());
            cb->setViewport(QRhiViewport(float(fit.x()), float(target.height() - fit.bottom()),
                                         float(fit.width()), float(fit.height())));
            cb->setShaderResources(bindings.get());
            const QRhiCommandBuffer::VertexInput input(vertices.get(), 0);
            cb->setVertexInput(0, 1, &input);
            cb->draw(4);
        }
        cb->endPass();
    }

    void releaseResources() override {
        pipeline.reset();
        bindings.reset();
        texture.reset();
        sampler.reset();
        vertices.reset();
        uniforms.reset();
        made_for = nullptr;
        frame_new = true;      /* drawn again on the new device */
    }

private:
    static constexpr float kQuad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };

    /* twice the picture's height always, so that a smooth frame and a plain one share the texture */
    QSize TextureSize() const {
        if (frame.isNull()) return QSize(1, 2);
        return QSize(frame.width(), smooth ? frame.height() : frame.height() * 2);
    }
    void MakeTexture(const QSize &size) {
        texture.reset(rhi()->newTexture(QRhiTexture::RGBA8, size));
        texture->create();
    }
    void Bind() {
        bindings->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, texture.get(), sampler.get()) });
        bindings->create();
    }

    QImage frame;
    bool frame_new = false, first_upload = true, smooth = false;
    QPointF frac;
    QRhi *made_for = nullptr;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    std::unique_ptr<QRhiShaderResourceBindings> bindings;
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiBuffer> vertices, uniforms;
};

/* The picture by the CPU: QPainter scales it into the widget. */
class CpuPicture final : public QWidget, public PictureSurface {
public:
    explicit CpuPicture(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }
    QWidget *widget() override { return this; }
    void setFrame(const QImage &image) override { frame = image; smooth = false; update(); }
    void setSmoothFrame(const QImage &image, QPointF f) override { frame = image; smooth = true; frac = f; update(); }
    QImage grabScreen() override { return grab().toImage(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (frame.isNull()) return;
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        if (!smooth) {
            p.drawImage(Picture_Fit(QSizeF(size()), frame.size()), frame);
            return;
        }
        const QSize pic(frame.width(), frame.height() / 2);
        const QRectF fit = Picture_Fit(QSizeF(size()), pic);
        const double scale = fit.width() / pic.width();
        const QPointF shift = Picture_SmoothShift(frac, scale * devicePixelRatioF()) ;
        const QRectF scenery(0, 0, pic.width(), pic.height()), party(0, pic.height(), pic.width(), pic.height());
        p.setClipRect(fit);
        p.drawImage(fit, frame, scenery);                      /* the underlay at the integer position */
        p.drawImage(fit.translated(-shift.x() * scale, -shift.y() * scale), frame, scenery);
        p.drawImage(fit, frame, party);
    }

private:
    QImage frame;
    bool smooth = false;
    QPointF frac;
};

}   // namespace

PictureDrawing Picture_Drawing() {
    static int chosen = -1;
    if (chosen >= 0)
        return PictureDrawing(chosen);
    const char *wanted = getenv("EB_DRAWING");
    PictureDrawing drawing = PictureDrawing::Cpu;
    const bool headless = QGuiApplication::platformName().startsWith("offscreen") ||
                          QGuiApplication::platformName().startsWith("minimal");
    if (wanted && strcmp(wanted, "cpu") == 0)
        drawing = PictureDrawing::Cpu;
    else if (headless)
        drawing = PictureDrawing::Cpu;
    else if (!(wanted && strcmp(wanted, "opengl") == 0) && VulkanStarts())
        drawing = PictureDrawing::Vulkan;
    else if (OpenGLStarts())
        drawing = PictureDrawing::OpenGL;
    fprintf(stderr, "Drawing the picture with %s\n", Picture_DrawingName(drawing));
    chosen = int(drawing);
    return drawing;
}

const char *Picture_DrawingName(PictureDrawing drawing) {
    switch (drawing) {
    case PictureDrawing::Vulkan: return "Vulkan";
    case PictureDrawing::OpenGL: return "OpenGL";
    default:                     return "the CPU";
    }
}

QPointF Picture_SmoothShift(QPointF frac, double scale) {
    if (scale <= 0) return QPointF();
    return QPointF(std::round(frac.x() * scale) / scale, std::round(frac.y() * scale) / scale);
}

double PictureSurface::scaleFor(QSize picture) {
    QWidget *w = widget();
    const QRectF fit = Picture_Fit(QSizeF(w->size()) * w->devicePixelRatioF(), picture);
    return picture.isEmpty() ? 0 : fit.width() / picture.width();
}

QRectF Picture_Fit(const QSizeF &widget, const QSize &picture) {
    if (picture.isEmpty())
        return QRectF();
    const double scale = qMin(widget.width() / picture.width(), widget.height() / picture.height());
    const double w = picture.width() * scale, h = picture.height() * scale;
    return QRectF((widget.width() - w) / 2, (widget.height() - h) / 2, w, h);
}

PictureSurface *Picture_Make(QWidget *parent) {
    switch (Picture_Drawing()) {
    case PictureDrawing::Vulkan: return new RhiPicture(QRhiWidget::Api::Vulkan, parent);
    case PictureDrawing::OpenGL: return new RhiPicture(QRhiWidget::Api::OpenGL, parent);
    default:                     return new CpuPicture(parent);
    }
}
