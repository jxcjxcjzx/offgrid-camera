/*
Copyright (c) 2013, Broadcom Europe Ltd
Copyright (c) 2013, Tim Gover
All rights reserved.


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "calibration.h"
#include "RaspiTex.h"
#include "RaspiTexUtil.h"
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>

/* \file calibration.c
 * Example code for implementing Calibration filter as GLSL shaders.
 * The input image is a greyscale texture from the MMAL buffer Y plane.
 */

static GLfloat quad_varray[] = {
   -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, -1.0f,
   -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f,
};

static GLuint quad_vbo;

static RASPITEXUTIL_SHADER_PROGRAM_T calibration_shader =
{
    .vertex_source = NULL,
    .fragment_source = NULL,
    .uniform_names = {"tex", "tex_unit"},
    .attribute_names = {"vertex"},
};

static const EGLint calibration_egl_config_attribs[] =
{
   EGL_RED_SIZE,   8,
   EGL_GREEN_SIZE, 8,
   EGL_BLUE_SIZE,  8,
   EGL_ALPHA_SIZE, 8,
   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
   EGL_NONE
};


/**
 * Initialisation of shader uniforms.
 *
 * @param width Width of the EGL image.
 * @param width Height of the EGL image.
 */
static int shader_set_uniforms(RASPITEXUTIL_SHADER_PROGRAM_T *shader,
      int width, int height)
{
   GLCHK(glUseProgram(shader->program));
   GLCHK(glUniform1i(shader->uniform_locations[0], 0)); // Texture unit

   /* Dimensions of a single pixel in texture co-ordinates */
   GLCHK(glUniform2f(shader->uniform_locations[1],
            1.0 / (float) width, 1.0 / (float) height));

   /* Enable attrib 0 as vertex array */
   GLCHK(glEnableVertexAttribArray(shader->attribute_locations[0]));
   return 0;
}

static const char* VERTEX_SHADER_SOURCE =
  "attribute vec2 vertex;\n"                    \
  "varying vec2 texcoord;\n"                    \
  "\n"                                          \
  "void main(void) {\n"                         \
  "   texcoord = 0.5 * (vertex + 1.0);\n"       \
  "   gl_Position = vec4(vertex, 0.0, 1.0);\n"  \
  "}\n";

static const char* FRAGMENT_SHADER_SOURCE =
  "#extension GL_OES_EGL_image_external : require\n"    \
  "\n"                                                  \
  "uniform samplerExternalOES tex;\n"                   \
  "varying vec2 texcoord;\n"                            \
  "uniform vec2 tex_unit;\n"                            \
  "\n"                                                  \
  "float sum(vec4 p) {\n"                               \
  "  return p[0] + p[1] + p[2];\n"                      \
  "}\n"                                                 \
  "\n"                                                  \
  "void main(void) {\n"                                 \
  "    float x = texcoord.x;\n"                         \
  "    float y = texcoord.y;\n"                         \
  "    float x1 = x - tex_unit.x;\n"                    \
  "    float y1 = y - tex_unit.y;\n"                    \
  "    float x2 = x + tex_unit.x;\n"                    \
  "    float y2 = y + tex_unit.y;\n"                    \
  "    vec4 p0 = texture2D(tex, vec2(x1, y1));\n"       \
  "    vec4 p1 = texture2D(tex, vec2(x, y1));\n"        \
  "    vec4 p2 = texture2D(tex, vec2(x2, y1));\n"       \
  "    vec4 p3 = texture2D(tex, vec2(x1, y));\n"        \
  "    vec4 p4 = texture2D(tex, vec2(x, y));\n"         \
  "    vec4 p5 = texture2D(tex, vec2(x2, y));\n"        \
  "    vec4 p6 = texture2D(tex, vec2(x1, y2));\n"       \
  "    vec4 p7 = texture2D(tex, vec2(x, y2));\n"        \
  "    vec4 p8 = texture2D(tex, vec2(x2, y2));\n"       \
  "\n"                                                  \
  "    float sum4 = sum(p4);\n"                         \
  "    if (sum4 == sum4 || sum4 >= 2.4 &&\n"                            \
  "        sum4 == sum(p0) &&\n"                        \
  "        sum4 == sum(p1) &&\n"                        \
  "        sum4 == sum(p2) &&\n"                        \
  "        sum4 == sum(p3) &&\n"                        \
  "        sum4 == sum(p5) &&\n"                        \
  "        sum4 == sum(p6) &&\n"                        \
  "        sum4 == sum(p7) &&\n"                        \
  "        sum4 == sum(p8)) {\n"                        \
  "      gl_FragColor = p4;\n"                          \
  "    } else {\n"                                      \
  "      gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"    \
  "    }\n"                                             \
  "\n"                                                  \
  "    gl_FragColor.a = 1.0;\n"                         \
  "}\n";

/**
 * Creates the OpenGL ES 2.X context and builds the shaders.
 * @param raspitex_state A pointer to the GL preview state.
 * @return Zero if successful.
 */
static int calibration_init(RASPITEX_STATE *raspitex_state)
{
    int rc = 0;
    int width = raspitex_state->width;
    int height = raspitex_state->height;

    calibration_shader.vertex_source = VERTEX_SHADER_SOURCE;
    calibration_shader.fragment_source = FRAGMENT_SHADER_SOURCE;

    vcos_log_trace("%s", VCOS_FUNCTION);
    raspitex_state->egl_config_attribs = calibration_egl_config_attribs;
    rc = raspitexutil_gl_init_2_0(raspitex_state);
    if (rc != 0)
      return rc;

    rc = raspitexutil_build_shader_program(&calibration_shader);
    if (rc != 0)
      return rc;

    rc = shader_set_uniforms(&calibration_shader, width, height);
    if (rc != 0)
      return rc;

    GLCHK(glGenBuffers(1, &quad_vbo));
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
    GLCHK(glBufferData(GL_ARRAY_BUFFER, sizeof(quad_varray), quad_varray, GL_STATIC_DRAW));
    GLCHK(glClearColor(0.0f, 0.0f, 0.0f, 1.0f));

    return rc;
}

/* Redraws the scene with the latest luma buffer.
 *
 * @param raspitex_state A pointer to the GL preview state.
 * @return Zero if successful.
 */
static int calibration_redraw(RASPITEX_STATE* state)
{
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   GLCHK(glUseProgram(calibration_shader.program));

   /* Bind the Y plane texture */
   /* GLCHK(glActiveTexture(GL_TEXTURE0)); */
   GLCHK(glBindTexture(GL_TEXTURE_EXTERNAL_OES, state->texture));
   GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
   /* GLCHK(glEnableVertexAttribArray(calibration_shader.attribute_locations[0])); */
   /* GLCHK(glVertexAttribPointer(calibration_shader.attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, 0)); */
   GLCHK(glDrawArrays(GL_TRIANGLES, 0, 6));

   return 0;
}

int calibration_open(RASPITEX_STATE *state)
{
   state->ops.gl_init = calibration_init;
   state->ops.redraw = calibration_redraw;
   state->ops.update_texture = raspitexutil_update_texture;
   return 0;
}
