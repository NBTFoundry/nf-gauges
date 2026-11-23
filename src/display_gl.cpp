// SPDX-License-Identifier: MPL-2.0
#include "display_gl.hpp"

#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "nbt/app/log.hpp"

namespace gauges {
namespace {

DisplayGl *g_display = NULL;

void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)px_map; // direct mode: the whole buffer, areas in screen coordinates
    if (g_display) {
        g_display->flushed(area->x1, area->y1, area->x2, area->y2, lv_display_flush_is_last(disp));
    }
    lv_display_flush_ready(disp);
}

const char *const kVertex = "attribute vec2 a_pos;\n"
                            "attribute vec2 a_uv;\n"
                            "varying vec2 v_uv;\n"
                            "void main() {\n"
                            "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                            "  v_uv = a_uv;\n"
                            "}\n";
// LVGL's XRGB8888 is B, G, R, X in memory: uploaded as RGBA, swizzled here.
const char *const kFragment = "precision mediump float;\n"
                              "uniform sampler2D u_tex;\n"
                              "varying vec2 v_uv;\n"
                              "void main() {\n"
                              "  gl_FragColor = vec4(texture2D(u_tex, v_uv).bgr, 1.0);\n"
                              "}\n";

GLuint compile(GLenum type, const char *source) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        nbt::app::log_info("display: shader: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

DisplayGl::DisplayGl()
    : width_(0), height_(0), stride_(0), buffer_(NULL), scratch_(NULL), gl_(false), frame_(false),
      texture_(0), program_(0), vbo_(0), a_pos_(-1), a_uv_(-1), u_tex_(-1), display_(NULL) {}

DisplayGl::~DisplayGl() {
    shutdown();
}

bool DisplayGl::init(int width, int height, bool gl) {
    width_ = width;
    height_ = height;
    gl_ = gl;
    stride_ = static_cast<int>(lv_draw_buf_width_to_stride(static_cast<uint32_t>(width),
                                                           LV_COLOR_FORMAT_XRGB8888));
    const size_t size = static_cast<size_t>(stride_) * static_cast<size_t>(height);
    buffer_ = static_cast<uint8_t *>(calloc(1, size));
    scratch_ = gl ? static_cast<uint8_t *>(malloc(size)) : NULL;
    if (!buffer_ || (gl && !scratch_)) {
        nbt::app::log_info("display: no memory for %dx%d", width, height);
        return false;
    }
    lv_display_t *disp = lv_display_create(width, height);
    if (!disp) {
        return false;
    }
    display_ = disp;
    g_display = this;
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, buffer_, NULL, static_cast<uint32_t>(size),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, &flush_cb);
    if (!gl) {
        return true;
    }
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs) {
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        nbt::app::log_info("display: the shader program does not link");
        return false;
    }
    a_pos_ = glGetAttribLocation(program_, "a_pos");
    a_uv_ = glGetAttribLocation(program_, "a_uv");
    u_tex_ = glGetUniformLocation(program_, "u_tex");
    // A full-screen quad; the texture's first row at the top.
    const GLfloat quad[] = {-1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0};
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    vbo_ = vbo;
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    texture_ = tex;
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        nbt::app::log_info("display: texture %dx%d: GL error 0x%x", width, height, err);
        return false;
    }
    return true;
}

void DisplayGl::flushed(int x1, int y1, int x2, int y2, bool last) {
    if (gl_ && texture_) {
        if (x1 < 0) {
            x1 = 0;
        }
        if (y1 < 0) {
            y1 = 0;
        }
        if (x2 >= width_) {
            x2 = width_ - 1;
        }
        if (y2 >= height_) {
            y2 = height_ - 1;
        }
        if (x2 >= x1 && y2 >= y1) {
            const int w = x2 - x1 + 1, h = y2 - y1 + 1;
            glBindTexture(GL_TEXTURE_2D, texture_);
            const uint8_t *src = buffer_ + static_cast<size_t>(y1) * stride_ + static_cast<size_t>(x1) * 4;
            if (w == width_ && stride_ == width_ * 4) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y1, w, h, GL_RGBA, GL_UNSIGNED_BYTE, src);
            } else {
                // OpenGL ES 2 has no row length: the area's rows, packed.
                for (int r = 0; r < h; ++r) {
                    memcpy(scratch_ + static_cast<size_t>(r) * w * 4, src + static_cast<size_t>(r) * stride_,
                           static_cast<size_t>(w) * 4);
                }
                glTexSubImage2D(GL_TEXTURE_2D, 0, x1, y1, w, h, GL_RGBA, GL_UNSIGNED_BYTE, scratch_);
            }
        }
    }
    if (last) {
        frame_ = true;
    }
}

bool DisplayGl::take_frame() {
    const bool f = frame_;
    frame_ = false;
    return f;
}

void DisplayGl::draw(int vw, int vh) {
    if (!gl_ || !program_) {
        return;
    }
    glViewport(0, 0, vw, vh);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glUniform1i(u_tex_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(static_cast<GLuint>(a_pos_));
    glEnableVertexAttribArray(static_cast<GLuint>(a_uv_));
    glVertexAttribPointer(static_cast<GLuint>(a_pos_), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);
    glVertexAttribPointer(static_cast<GLuint>(a_uv_), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void *>(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void DisplayGl::shutdown() {
    if (gl_) {
        if (texture_) {
            GLuint t = texture_;
            glDeleteTextures(1, &t);
        }
        if (vbo_) {
            GLuint b = vbo_;
            glDeleteBuffers(1, &b);
        }
        if (program_) {
            glDeleteProgram(program_);
        }
    }
    texture_ = vbo_ = program_ = 0;
    if (display_) {
        lv_display_delete(static_cast<lv_display_t *>(display_));
        display_ = NULL;
    }
    if (g_display == this) {
        g_display = NULL;
    }
    free(buffer_);
    free(scratch_);
    buffer_ = scratch_ = NULL;
}

} // namespace gauges
