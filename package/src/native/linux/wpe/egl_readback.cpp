// egl_readback.cpp — WPE EGLImage compositor with one glReadPixels transfer.

#include "egl_readback.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace electrobun {
namespace wpe {

namespace {

GLuint compileShader(GLenum kind, const char* source, std::string& error) {
    GLuint shader = glCreateShader(kind);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::vector<char> log(std::max(1, length));
    glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
    error = std::string("shader compile failed: ") + log.data();
    glDeleteShader(shader);
    return 0;
}

bool hasExtension(const char* extensions, const char* wanted) {
    if (!extensions || !wanted || !*wanted || std::strchr(wanted, ' ')) return false;
    const size_t wantedLength = std::strlen(wanted);
    const char* cursor = extensions;
    while ((cursor = std::strstr(cursor, wanted))) {
        const bool startsAtBoundary = cursor == extensions || cursor[-1] == ' ';
        const char after = cursor[wantedLength];
        const bool endsAtBoundary = after == '\0' || after == ' ';
        if (startsAtBoundary && endsAtBoundary) return true;
        cursor += wantedLength;
    }
    return false;
}

} // namespace

struct EglReadback::Impl {
    int renderFd = -1;
    gbm_device* gbmDevice = nullptr;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    EGLConfig eglConfig = nullptr;

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture = nullptr;

    GLuint program = 0;
    GLuint inputTexture = 0;
    GLuint outputTexture = 0;
    GLuint framebuffer = 0;
    GLuint vertexBuffer = 0;
    GLint positionAttribute = -1;
    GLint textureAttribute = -1;
    GLint textureUniform = -1;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    int rotationQuarters = 0;
    bool directScanout = false;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> bgra;
    std::string error;
    uint64_t frameCount = 0;

    void fail(const std::string& message) {
        error = message;
        fprintf(stderr, "[WpeEgl] %s\n", error.c_str());
        fflush(stderr);
    }

    bool makeCurrent() {
        if (eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) return true;
        char message[96];
        std::snprintf(message, sizeof(message),
                      "eglMakeCurrent failed (0x%04x)", eglGetError());
        fail(message);
        return false;
    }

    void destroy() {
        if (eglDisplay != EGL_NO_DISPLAY && eglContext != EGL_NO_CONTEXT) {
            if (makeCurrent()) {
                if (vertexBuffer) glDeleteBuffers(1, &vertexBuffer);
                if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
                if (outputTexture) glDeleteTextures(1, &outputTexture);
                if (inputTexture) glDeleteTextures(1, &inputTexture);
                if (program) glDeleteProgram(program);
            }
            eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (eglDisplay != EGL_NO_DISPLAY && eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay, eglSurface);
        }
        if (eglDisplay != EGL_NO_DISPLAY && eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay, eglContext);
        }
        if (eglDisplay != EGL_NO_DISPLAY) eglTerminate(eglDisplay);
        if (gbmDevice) gbm_device_destroy(gbmDevice);
        if (renderFd >= 0) close(renderFd);

        renderFd = -1;
        gbmDevice = nullptr;
        eglDisplay = EGL_NO_DISPLAY;
        eglContext = EGL_NO_CONTEXT;
        eglSurface = EGL_NO_SURFACE;
    }

    bool initialize(
        uint32_t requestedWidth,
        uint32_t requestedHeight,
        int rotationQuarters) {
        width = requestedWidth;
        height = requestedHeight;

        const char* renderNode = std::getenv("ELECTROBUN_RENDER_NODE");
        if (!renderNode || !*renderNode) renderNode = "/dev/dri/renderD128";
        renderFd = open(renderNode, O_RDWR | O_CLOEXEC);
        if (renderFd < 0) {
            fail(std::string("cannot open render node ") + renderNode);
            return false;
        }

        gbmDevice = gbm_create_device(renderFd);
        if (!gbmDevice) {
            fail("gbm_create_device failed");
            return false;
        }

        auto getPlatformDisplay =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (!getPlatformDisplay) {
            fail("EGL_EXT_platform_base is unavailable");
            return false;
        }
        eglDisplay = getPlatformDisplay(
            EGL_PLATFORM_GBM_KHR, static_cast<void*>(gbmDevice), nullptr);
        if (eglDisplay == EGL_NO_DISPLAY) {
            fail("eglGetPlatformDisplayEXT(GBM) failed");
            return false;
        }

        EGLint major = 0;
        EGLint minor = 0;
        if (!eglInitialize(eglDisplay, &major, &minor)) {
            fail("eglInitialize failed");
            return false;
        }
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            fail("eglBindAPI(OpenGL ES) failed");
            return false;
        }

        const EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 1,
            EGL_GREEN_SIZE, 1,
            EGL_BLUE_SIZE, 1,
            EGL_ALPHA_SIZE, 0,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE,
        };
        EGLint matched = 0;
        if (!eglChooseConfig(
                eglDisplay, configAttributes, &eglConfig, 1, &matched) ||
            matched != 1) {
            fail("no suitable EGL window configuration");
            return false;
        }

        const EGLint contextAttributes[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE,
        };
        eglContext = eglCreateContext(
            eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttributes);
        if (eglContext == EGL_NO_CONTEXT) {
            fail("eglCreateContext failed");
            return false;
        }

        // Mesa/V3D's GBM display exposes window configurations, not pbuffer
        // configurations. EGL_KHR_surfaceless_context lets the FBO-backed
        // compositor run without allocating a GBM window surface.
        eglSurface = EGL_NO_SURFACE;
        if (!makeCurrent()) return false;

        const char* glExtensions =
            reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (!hasExtension(glExtensions, "GL_OES_EGL_image")) {
            fail("GL_OES_EGL_image is unavailable");
            return false;
        }
        this->rotationQuarters = ((rotationQuarters % 4) + 4) % 4;
        directScanout =
            hasExtension(glExtensions, "GL_EXT_read_format_bgra");
        const bool swapsAxes =
            this->rotationQuarters == 1 || this->rotationQuarters == 3;
        outputWidth = directScanout && swapsAxes ? height : width;
        outputHeight = directScanout && swapsAxes ? width : height;
        imageTargetTexture =
            reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!imageTargetTexture) {
            fail("glEGLImageTargetTexture2DOES is unavailable");
            return false;
        }

        static const char* vertexSource =
            "attribute vec2 a_position;\n"
            "attribute vec2 a_texture;\n"
            "varying vec2 v_texture;\n"
            "void main() {\n"
            "  v_texture = a_texture;\n"
            "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
            "}\n";
        static const char* fragmentSource =
            "precision mediump float;\n"
            "uniform sampler2D u_texture;\n"
            "varying vec2 v_texture;\n"
            "void main() {\n"
            "  gl_FragColor = texture2D(u_texture, v_texture);\n"
            "}\n";

        GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource, error);
        if (!vertexShader) return false;
        GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource, error);
        if (!fragmentShader) {
            glDeleteShader(vertexShader);
            return false;
        }

        program = glCreateProgram();
        glAttachShader(program, vertexShader);
        glAttachShader(program, fragmentShader);
        glBindAttribLocation(program, 0, "a_position");
        glBindAttribLocation(program, 1, "a_texture");
        glLinkProgram(program);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            fail("GL shader program link failed");
            return false;
        }
        positionAttribute = glGetAttribLocation(program, "a_position");
        textureAttribute = glGetAttribLocation(program, "a_texture");
        textureUniform = glGetUniformLocation(program, "u_texture");
        if (positionAttribute < 0 || textureAttribute < 0 || textureUniform < 0) {
            fail("GL shader locations are incomplete");
            return false;
        }

        // EGLImages use a top-left texture origin. The direct-scanout texture
        // coordinates rotate each layer into the panel's native portrait
        // orientation; the normal coordinates mirror Cog's EGLImage renderer.
        GLfloat vertices[] = {
            // clip-space position
            -1.0f,  1.0f,
             1.0f,  1.0f,
            -1.0f, -1.0f,
             1.0f, -1.0f,
            // texture coordinate
             0.0f,  0.0f,
             1.0f,  0.0f,
             0.0f,  1.0f,
             1.0f,  1.0f,
        };
        // Texture coordinates paired with GL viewport corners:
        // high-Y left, high-Y right, low-Y left, low-Y right. glReadPixels
        // writes low-Y first, which is also the scanout buffer's top row.
        static const GLfloat directTextureCoordinates[4][8] = {
            { 0.0f, 1.0f,  1.0f, 1.0f,  0.0f, 0.0f,  1.0f, 0.0f },
            { 1.0f, 1.0f,  1.0f, 0.0f,  0.0f, 1.0f,  0.0f, 0.0f },
            { 1.0f, 0.0f,  0.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f },
            { 0.0f, 0.0f,  0.0f, 1.0f,  1.0f, 0.0f,  1.0f, 1.0f },
        };
        if (directScanout) {
            std::memcpy(
                vertices + 8,
                directTextureCoordinates[this->rotationQuarters],
                sizeof(directTextureCoordinates[0]));
        }
        glGenBuffers(1, &vertexBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        glBufferData(
            GL_ARRAY_BUFFER,
            sizeof(vertices),
            vertices,
            GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glGenTextures(1, &inputTexture);
        glBindTexture(GL_TEXTURE_2D, inputTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenTextures(1, &outputTexture);
        glBindTexture(GL_TEXTURE_2D, outputTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA, outputWidth, outputHeight, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, outputTexture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fail("offscreen GL framebuffer is incomplete");
            return false;
        }

        const size_t outputBytes =
            static_cast<size_t>(outputWidth) * outputHeight * 4;
        if (directScanout) {
            bgra.resize(outputBytes);
        } else {
            rgba.resize(outputBytes);
            bgra.resize(outputBytes);
        }

        fprintf(stderr,
                "[WpeEgl] initialized EGL %d.%d, GL renderer=%s, "
                "%ux%u readback%s\n",
                major, minor,
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                outputWidth, outputHeight,
                directScanout ? " (direct BGRA scanout)" : "");
        fflush(stderr);
        return true;
    }

    bool render(
        const std::vector<EglLayer>& layers,
        uint8_t* scanoutPixels,
        uint32_t scanoutPitch,
        uint32_t scanoutWidth,
        uint32_t scanoutHeight) {
        if (!makeCurrent()) return false;
        const auto started = std::chrono::steady_clock::now();

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glViewport(0, 0, outputWidth, outputHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(textureUniform, 0);
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        glVertexAttribPointer(
            positionAttribute, 2, GL_FLOAT, GL_FALSE, 0,
            reinterpret_cast<void*>(0));
        glVertexAttribPointer(
            textureAttribute, 2, GL_FLOAT, GL_FALSE, 0,
            reinterpret_cast<void*>(8 * sizeof(GLfloat)));
        glEnableVertexAttribArray(positionAttribute);
        glEnableVertexAttribArray(textureAttribute);

        for (const auto& layer : layers) {
            if (!layer.image || layer.width <= 0 || layer.height <= 0) continue;
            if (layer.transparent) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }
            if (directScanout) {
                switch (rotationQuarters) {
                    case 0:
                        glViewport(
                            layer.x, layer.y,
                            layer.width, layer.height);
                        break;
                    case 1:
                        glViewport(
                            static_cast<GLint>(height) -
                                layer.y - layer.height,
                            layer.x,
                            layer.height,
                            layer.width);
                        break;
                    case 2:
                        glViewport(
                            static_cast<GLint>(width) -
                                layer.x - layer.width,
                            static_cast<GLint>(height) -
                                layer.y - layer.height,
                            layer.width,
                            layer.height);
                        break;
                    case 3:
                    default:
                        glViewport(
                            layer.y,
                            static_cast<GLint>(width) -
                                layer.x - layer.width,
                            layer.height,
                            layer.width);
                        break;
                }
            } else {
                glViewport(
                    layer.x,
                    static_cast<GLint>(height) - layer.y - layer.height,
                    layer.width,
                    layer.height);
            }
            glBindTexture(GL_TEXTURE_2D, inputTexture);
            imageTargetTexture(
                GL_TEXTURE_2D,
                reinterpret_cast<GLeglImageOES>(layer.image));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        glDisableVertexAttribArray(positionAttribute);
        glDisableVertexAttribArray(textureAttribute);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // glReadPixels synchronizes exported-image sampling before WPE is
        // allowed to recycle those buffers. The fast path writes panel-native
        // BGRA rows directly into the next DRM dumb buffer.
        if (directScanout) {
            if (!scanoutPixels ||
                scanoutWidth != outputWidth ||
                scanoutHeight != outputHeight) {
                fail("direct scanout dimensions do not match EGL output");
                return false;
            }
            const uint32_t rowBytes = outputWidth * 4;
            if (scanoutPitch == rowBytes) {
                glReadPixels(
                    0, 0, outputWidth, outputHeight,
                    GL_BGRA_EXT, GL_UNSIGNED_BYTE, scanoutPixels);
            } else {
                glReadPixels(
                    0, 0, outputWidth, outputHeight,
                    GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra.data());
                for (uint32_t y = 0; y < outputHeight; y++) {
                    std::memcpy(
                        scanoutPixels + static_cast<size_t>(y) * scanoutPitch,
                        bgra.data() + static_cast<size_t>(y) * rowBytes,
                        rowBytes);
                }
            }
        } else {
            glReadPixels(
                0, 0, width, height,
                GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        }
        const GLenum readError = glGetError();
        if (readError != GL_NO_ERROR) {
            char message[96];
            std::snprintf(message, sizeof(message),
                          "glReadPixels failed (0x%04x)", readError);
            fail(message);
            return false;
        }

        if (!directScanout) {
            // GL rows arrive bottom-to-top in RGBA. The dumb-buffer
            // compositor consumes top-to-bottom BGRA.
            const size_t rowBytes = static_cast<size_t>(width) * 4;
            for (uint32_t y = 0; y < height; y++) {
                const uint8_t* src =
                    rgba.data() +
                    static_cast<size_t>(height - 1 - y) * rowBytes;
                uint8_t* dst =
                    bgra.data() + static_cast<size_t>(y) * rowBytes;
                for (uint32_t x = 0; x < width; x++) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 0xff;
                }
            }
        }

        frameCount++;
        // Avoid steady-state compositor logging. On an appliance this stream
        // may be persisted and would otherwise create needless flash writes.
        if (frameCount == 1) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started);
            fprintf(stderr,
                    "[WpeEgl] composed frame #%llu in %.2f ms (%zu layers)\n",
                    static_cast<unsigned long long>(frameCount),
                    elapsed.count() / 1000.0, layers.size());
            fflush(stderr);
        }
        return true;
    }
};

EglReadback::EglReadback() : impl_(new Impl) {}

EglReadback::~EglReadback() {
    impl_->destroy();
}

bool EglReadback::init(
    uint32_t width,
    uint32_t height,
    int rotationQuarters) {
    return impl_->initialize(width, height, rotationQuarters);
}

void* EglReadback::display() const {
    return impl_->eglDisplay;
}

bool EglReadback::compose(const std::vector<EglLayer>& layers) {
    return impl_->render(layers, nullptr, 0, 0, 0);
}

bool EglReadback::canComposeToScanout() const {
    return impl_->directScanout;
}

bool EglReadback::composeToScanout(
    const std::vector<EglLayer>& layers,
    uint8_t* pixels,
    uint32_t pitch,
    uint32_t width,
    uint32_t height) {
    return impl_->render(layers, pixels, pitch, width, height);
}

const uint8_t* EglReadback::pixels() const {
    return impl_->bgra.data();
}

uint32_t EglReadback::stride() const {
    return impl_->width * 4;
}

const std::string& EglReadback::lastError() const {
    return impl_->error;
}

} // namespace wpe
} // namespace electrobun
