/*
Copyright (C) 2018 Andrey Nazarov

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "gl.h"

#define MAX_SHADER_CHARS    8192

#define GLSL(x)     Q_strlcat(buf, #x "\n", MAX_SHADER_CHARS);
#define GLSF(x)     Q_strlcat(buf, x, MAX_SHADER_CHARS)

enum {
    VERT_ATTR_POS,
    VERT_ATTR_TC,
    VERT_ATTR_LMTC,
    VERT_ATTR_COLOR
};

static void write_header(char *buf)
{
    *buf = 0;
    if (gl_config.ver_es) {
        GLSF("#version 300 es\n");
    } else if (gl_config.ver_sl >= 140) {
        GLSF("#version 140\n");
    } else {
        GLSF("#version 130\n");
        GLSF("#extension GL_ARB_uniform_buffer_object : require\n");
    }
}

static void write_block(char *buf)
{
    GLSF("layout(std140) uniform u_block {\n");
        GLSL(mat4 m_view;)
        GLSL(mat4 m_proj;)
        GLSL(float u_time;)
        GLSL(float u_modulate;)
        GLSL(float u_add;)
        GLSL(float u_intensity;)
        GLSL(vec4 u_fog_color;)
        GLSL(vec4 u_fog_eye;)
        GLSL(vec4 u_fog_up;)
        GLSL(vec4 u_fog_hstart;)
        GLSL(vec4 u_fog_hend;)
    GLSF("};\n");
}

/*
The rerelease's fog, as KEX draws it. The formulas are q2repro's
(src/refresh/shader.c), which reproduces KEX's quirks - the same ones the
software renderer's sw/fog.c uses, line for line, so the two agree:

  distance fog   d = fog_density / 64 * depth; mix by 1 - exp(-d^2)
  height fog     KEX's write_height_fog, its "very broken" colour ramp and all
  sky            fog_sky_factor of the way to fog_color, nothing else

v_eye is the fragment in eye space: depth is -z, and the world +Z axis in eye
space (u_fog_up) gives how far up or down the view ray has gone. An additive
draw fades out into the fog instead of being tinted by it, or every glow and
particle would add the fog colour on top of what is behind it.
*/
static void write_fog(char *buf, GLbitfield bits)
{
    bool additive = bits & GLS_BLEND_ADD;

    if (bits & GLS_FOG_SKY) {
        if (additive)
            GLSL(diffuse.rgb *= 1.0 - u_fog_up.w;)
        else
            GLSL(diffuse.rgb = mix(diffuse.rgb, u_fog_color.rgb, u_fog_up.w);)
        return;
    }

    GLSL(float fog_z = -v_eye.z;)
    GLSL(float fog_d = u_fog_color.a * fog_z;)
    GLSL(float fog_dist = 1.0 - exp(-(fog_d * fog_d));)
    GLSL(float fog_keep = 1.0 - fog_dist;)
    GLSL(vec3 fog_add = u_fog_color.rgb * fog_dist;)

    GLSF("if (u_fog_eye.w > 0.0) {\n");
        // the integral of exp(-falloff * height) along the ray, whose own
        // form takes two nearly equal exponentials from each other on a
        // level ray: (1 - exp(-x)) / x is the series there instead
        GLSL(float fog_dz = dot(v_eye, u_fog_up.xyz);)
        GLSL(float fog_x = u_fog_eye.z * fog_dz;)
        GLSL(float fog_g = abs(fog_x) < 0.03 ? 1.0 - fog_x * (0.5 - fog_x / 6.0) : (1.0 - exp(-fog_x)) / fog_x;)
        GLSL(float fog_ext = clamp(1.0 - exp(-u_fog_eye.y * length(v_eye) * fog_g), 0.0, 1.0);)
        GLSL(float fog_frac = clamp((u_fog_eye.x + fog_dz - u_fog_hstart.w) * u_fog_hend.w, 0.0, 1.0);)
        GLSL(float fog_h = (1.0 - exp(-u_fog_eye.w * fog_z)) * fog_ext;)
        GLSL(vec3 fog_hcolor = mix(u_fog_hstart.rgb, u_fog_hend.rgb, fog_frac);)
        GLSL(fog_add = fog_add * (1.0 - fog_h) + fog_hcolor * (fog_ext * fog_h);)
        GLSL(fog_keep *= 1.0 - fog_h;)
    GLSF("}\n");

    if (additive)
        GLSL(diffuse.rgb *= fog_keep;)
    else
        GLSL(diffuse.rgb = diffuse.rgb * fog_keep + fog_add;)
}

static void write_vertex_shader(char *buf, GLbitfield bits)
{
    write_header(buf);
    write_block(buf);
    GLSL(in vec4 a_pos;)
    GLSL(in vec2 a_tc;)
    GLSL(out vec2 v_tc;)
    if (bits & GLS_LIGHTMAP_ENABLE) {
        GLSL(in vec2 a_lmtc;)
        GLSL(out vec2 v_lmtc;)
    }
    if (!(bits & GLS_TEXTURE_REPLACE)) {
        GLSL(in vec4 a_color;)
        GLSL(out vec4 v_color;)
    }
    if (bits & GLS_FOG_ENABLE)
        GLSL(out vec3 v_eye;)
    GLSF("void main() {\n");
        GLSL(vec2 tc = a_tc;)
        if (bits & GLS_FLOW_ENABLE) {
            if (bits & GLS_WARP_ENABLE)
                GLSL(tc.s -= u_time * 0.5;)
            else
                GLSL(tc.s -= 64.0 * fract(u_time * 0.025);)
        }
        GLSL(v_tc = tc;)
        if (bits & GLS_LIGHTMAP_ENABLE)
            GLSL(v_lmtc = a_lmtc;)
        if (!(bits & GLS_TEXTURE_REPLACE))
            GLSL(v_color = a_color;)
        if (bits & GLS_FOG_ENABLE) {
            GLSL(vec4 eye = m_view * a_pos;)
            GLSL(v_eye = eye.xyz;)
            GLSL(gl_Position = m_proj * eye;)
        } else {
            GLSL(gl_Position = m_proj * m_view * a_pos;)
        }
    GLSF("}\n");
}

static void write_fragment_shader(char *buf, GLbitfield bits)
{
    write_header(buf);

    if (gl_config.ver_es)
        GLSL(precision mediump float;)

    if (bits & (GLS_WARP_ENABLE | GLS_LIGHTMAP_ENABLE | GLS_INTENSITY_ENABLE | GLS_FOG_MASK))
        write_block(buf);

    GLSL(uniform sampler2D u_texture;)
    GLSL(in vec2 v_tc;)

    if (bits & GLS_FOG_ENABLE)
        GLSL(in vec3 v_eye;)

    if (bits & GLS_LIGHTMAP_ENABLE) {
        GLSL(uniform sampler2D u_lightmap;)
        GLSL(in vec2 v_lmtc;)
    }

    if (!(bits & GLS_TEXTURE_REPLACE))
        GLSL(in vec4 v_color;)

    GLSL(out vec4 o_color;)

    GLSF("void main() {\n");
        GLSL(vec2 tc = v_tc;)

        if (bits & GLS_WARP_ENABLE)
            GLSL(tc += 0.0625 * sin(tc.ts * 4.0 + u_time);)

        GLSL(vec4 diffuse = texture(u_texture, tc);)

        if (bits & GLS_ALPHATEST_ENABLE)
            GLSL(if (diffuse.a <= 0.666) discard;)

        if (bits & GLS_LIGHTMAP_ENABLE) {
            GLSL(vec4 lightmap = texture(u_lightmap, v_lmtc);)
            GLSL(diffuse.rgb *= (lightmap.rgb + u_add) * u_modulate;)
        }

        if (bits & GLS_INTENSITY_ENABLE)
            GLSL(diffuse.rgb *= u_intensity;)

        if (!(bits & GLS_TEXTURE_REPLACE))
            GLSL(diffuse *= v_color;)

        if (bits & GLS_FOG_MASK)
            write_fog(buf, bits);

        GLSL(o_color = diffuse;)
    GLSF("}\n");
}

static GLuint create_shader(GLenum type, const char *src)
{
    GLuint shader = qglCreateShader(type);
    if (!shader) {
        Com_EPrintf("Couldn't create shader\n");
        return 0;
    }

    qglShaderSource(shader, 1, &src, NULL);
    qglCompileShader(shader);
    GLint status;
    qglGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char buffer[MAX_STRING_CHARS] = { 0 };

        qglGetShaderInfoLog(shader, sizeof(buffer), NULL, buffer);
        qglDeleteShader(shader);

        if (buffer[0])
            Com_Printf("%s", buffer);

        Com_EPrintf("Error compiling shader\n");
        return 0;
    }

    return shader;
}

static GLuint create_and_use_program(GLbitfield bits)
{
    char buffer[MAX_SHADER_CHARS];

    GLuint program = qglCreateProgram();
    if (!program) {
        Com_EPrintf("Couldn't create program\n");
        return 0;
    }

    write_vertex_shader(buffer, bits);
    GLuint shader_v = create_shader(GL_VERTEX_SHADER, buffer);
    if (!shader_v)
        return program;

    write_fragment_shader(buffer, bits);
    GLuint shader_f = create_shader(GL_FRAGMENT_SHADER, buffer);
    if (!shader_f) {
        qglDeleteShader(shader_v);
        return program;
    }

    qglAttachShader(program, shader_v);
    qglAttachShader(program, shader_f);

    qglBindAttribLocation(program, VERT_ATTR_POS, "a_pos");
    qglBindAttribLocation(program, VERT_ATTR_TC, "a_tc");
    if (bits & GLS_LIGHTMAP_ENABLE)
        qglBindAttribLocation(program, VERT_ATTR_LMTC, "a_lmtc");
    if (!(bits & GLS_TEXTURE_REPLACE))
        qglBindAttribLocation(program, VERT_ATTR_COLOR, "a_color");

    qglLinkProgram(program);

    qglDeleteShader(shader_v);
    qglDeleteShader(shader_f);

    GLint status = 0;
    qglGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char buffer[MAX_STRING_CHARS] = { 0 };

        qglGetProgramInfoLog(program, sizeof(buffer), NULL, buffer);

        if (buffer[0])
            Com_Printf("%s", buffer);

        Com_EPrintf("Error linking program\n");
        return program;
    }

    GLuint index = qglGetUniformBlockIndex(program, "u_block");
    if (index == GL_INVALID_INDEX) {
        Com_EPrintf("Uniform block not found\n");
        return program;
    }

    GLint size = 0;
    qglGetActiveUniformBlockiv(program, index, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
    if (size != sizeof(gls.u_block)) {
        Com_EPrintf("Uniform block size mismatch: %d != %zu\n", size, sizeof(gls.u_block));
        return program;
    }

    qglUniformBlockBinding(program, index, 0);

    qglUseProgram(program);

    qglUniform1i(qglGetUniformLocation(program, "u_texture"), 0);
    if (bits & GLS_LIGHTMAP_ENABLE)
        qglUniform1i(qglGetUniformLocation(program, "u_lightmap"), 1);

    return program;
}

// Which program a draw needs: its shader bits, plus - under fog only - whether
// it is additive (write_fog), kept in GLS_SHADE_SMOOTH's slot, which no
// program reads.
static GLuint program_index(GLbitfield bits)
{
    GLuint i = (bits & GLS_SHADER_MASK) >> 6;

    if ((bits & GLS_FOG_MASK) && (bits & GLS_BLEND_ADD))
        i |= GLS_SHADE_SMOOTH >> 6;

    return i & (MAX_PROGRAMS - 1);
}

static void shader_state_bits(GLbitfield bits)
{
    GLbitfield diff = bits ^ gls.state_bits;

    if (diff & GLS_COMMON_MASK)
        GL_CommonStateBits(bits);

    if (program_index(bits) != program_index(gls.state_bits)) {
        GLuint i = program_index(bits);

        if (gl_static.programs[i])
            qglUseProgram(gl_static.programs[i]);
        else
            gl_static.programs[i] = create_and_use_program(bits);
    }
}

static void shader_array_bits(GLbitfield bits)
{
    GLbitfield diff = bits ^ gls.array_bits;

    if (diff & GLA_VERTEX) {
        if (bits & GLA_VERTEX) {
            qglEnableVertexAttribArray(VERT_ATTR_POS);
        } else {
            qglDisableVertexAttribArray(VERT_ATTR_POS);
        }
    }

    if (diff & GLA_TC) {
        if (bits & GLA_TC) {
            qglEnableVertexAttribArray(VERT_ATTR_TC);
        } else {
            qglDisableVertexAttribArray(VERT_ATTR_TC);
        }
    }

    if (diff & GLA_LMTC) {
        if (bits & GLA_LMTC) {
            qglEnableVertexAttribArray(VERT_ATTR_LMTC);
        } else {
            qglDisableVertexAttribArray(VERT_ATTR_LMTC);
        }
    }

    if (diff & GLA_COLOR) {
        if (bits & GLA_COLOR) {
            qglEnableVertexAttribArray(VERT_ATTR_COLOR);
        } else {
            qglDisableVertexAttribArray(VERT_ATTR_COLOR);
        }
    }
}

static void shader_vertex_pointer(GLint size, GLsizei stride, const GLfloat *pointer)
{
    qglVertexAttribPointer(VERT_ATTR_POS, size, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * stride, pointer);
}

static void shader_tex_coord_pointer(GLint size, GLsizei stride, const GLfloat *pointer)
{
    qglVertexAttribPointer(VERT_ATTR_TC, size, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * stride, pointer);
}

static void shader_light_coord_pointer(GLint size, GLsizei stride, const GLfloat *pointer)
{
    qglVertexAttribPointer(VERT_ATTR_LMTC, size, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * stride, pointer);
}

static void shader_color_byte_pointer(GLint size, GLsizei stride, const GLubyte *pointer)
{
    qglVertexAttribPointer(VERT_ATTR_COLOR, size, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLfloat) * stride, pointer);
}

static void shader_color_float_pointer(GLint size, GLsizei stride, const GLfloat *pointer)
{
    qglVertexAttribPointer(VERT_ATTR_COLOR, size, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * stride, pointer);
}

static void shader_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    qglVertexAttrib4f(VERT_ATTR_COLOR, r, g, b, a);
}

static void upload_u_block(void)
{
    qglBufferData(GL_UNIFORM_BUFFER, sizeof(gls.u_block), &gls.u_block, GL_DYNAMIC_DRAW);
}

/*
The fog for the view about to be drawn: the map's own values under KEX's rules,
trigger_fog fades included (CL_GetMapFogKEX, the software renderer's source
too). GL_Setup3D runs this before it uploads the matrices, so the block goes up
with them.
*/
static void shader_update_fog(void)
{
    mapfog_kex_t fog;
    vec3_t axis[3];
    float eye, ramp;

    gls.fog_bits = 0;

    if (!gl_fog->integer || (glr.fd.rdflags & RDF_NOWORLDMODEL) || !CL_GetMapFogKEX(&fog))
        return;

    // forward, left, up as GL_RotateForViewer makes the eye space of them:
    // x = -left, y = up, z = -forward
    AnglesToAxis(glr.fd.viewangles, axis);

    VectorCopy(fog.color, gls.u_block.fog_color);
    gls.u_block.fog_color[3] = max(fog.density, 0) / 64;

    eye = glr.fd.vieworg[2] - fog.hf_ref_z;
    gls.u_block.fog_eye[0] = glr.fd.vieworg[2];
    gls.u_block.fog_eye[1] = exp(-fog.hf_falloff * eye);
    gls.u_block.fog_eye[2] = fog.hf_falloff;
    gls.u_block.fog_eye[3] = max(fog.hf_density, 0);

    gls.u_block.fog_up[0] = -axis[1][2];
    gls.u_block.fog_up[1] = axis[2][2];
    gls.u_block.fog_up[2] = -axis[0][2];
    gls.u_block.fog_up[3] = fog.sky_factor;
    clamp(gls.u_block.fog_up[3], 0, 1);

    ramp = fog.hf_color_end_z - fog.hf_color_start_z;
    VectorCopy(fog.hf_start_color, gls.u_block.fog_hstart);
    gls.u_block.fog_hstart[3] = fog.hf_color_start_z;
    VectorCopy(fog.hf_end_color, gls.u_block.fog_hend);
    gls.u_block.fog_hend[3] = ramp ? 1.0f / ramp : 0;

    gls.fog_bits = GLS_FOG_ENABLE;
}

static void shader_update(void)
{
    gls.u_block.time = glr.fd.time;
    gls.u_block.modulate = gl_modulate->value * gl_modulate_world->value;
    gls.u_block.add = gl_brightness->value;
    gls.u_block.intensity = gl_intensity->value;

    shader_update_fog();
}

static void shader_view_matrix(const GLfloat *matrix)
{
    static const GLfloat identity[16] = { [0] = 1, [5] = 1, [10] = 1, [15] = 1 };

    if (!matrix)
        matrix = identity;

    memcpy(gls.u_block.view, matrix, sizeof(gls.u_block.view));
    upload_u_block();
}

static void shader_proj_matrix(const GLfloat *matrix)
{
    memcpy(gls.u_block.proj, matrix, sizeof(gls.u_block.proj));
    upload_u_block();
}

static void shader_reflect(void)
{
    gls.u_block.proj[0] = -gls.u_block.proj[0];
    upload_u_block();
}

static void shader_clear(void)
{
    qglActiveTexture(GL_TEXTURE1);
    qglBindTexture(GL_TEXTURE_2D, 0);

    qglActiveTexture(GL_TEXTURE0);
    qglBindTexture(GL_TEXTURE_2D, 0);

    qglDisableVertexAttribArray(VERT_ATTR_POS);
    qglDisableVertexAttribArray(VERT_ATTR_TC);
    qglDisableVertexAttribArray(VERT_ATTR_LMTC);
    qglDisableVertexAttribArray(VERT_ATTR_COLOR);

    if (gl_static.programs[0])
        qglUseProgram(gl_static.programs[0]);
    else
        gl_static.programs[0] = create_and_use_program(GLS_DEFAULT);
}

static void shader_init(void)
{
    qglGenBuffers(1, &gl_static.u_bufnum);
    qglBindBuffer(GL_UNIFORM_BUFFER, gl_static.u_bufnum);
    qglBindBufferBase(GL_UNIFORM_BUFFER, 0, gl_static.u_bufnum);
    qglBufferData(GL_UNIFORM_BUFFER, sizeof(gls.u_block), NULL, GL_DYNAMIC_DRAW);
}

static void shader_shutdown(void)
{
    qglUseProgram(0);
    for (int i = 0; i < MAX_PROGRAMS; i++) {
        if (gl_static.programs[i]) {
            qglDeleteProgram(gl_static.programs[i]);
            gl_static.programs[i] = 0;
        }
    }

    qglBindBuffer(GL_UNIFORM_BUFFER, 0);
    if (gl_static.u_bufnum) {
        qglDeleteBuffers(1, &gl_static.u_bufnum);
        gl_static.u_bufnum = 0;
    }
}

const glbackend_t backend_shader = {
    .name = "GLSL",

    .init = shader_init,
    .shutdown = shader_shutdown,
    .clear = shader_clear,
    .update = shader_update,

    .proj_matrix = shader_proj_matrix,
    .view_matrix = shader_view_matrix,
    .reflect = shader_reflect,

    .state_bits = shader_state_bits,
    .array_bits = shader_array_bits,

    .vertex_pointer = shader_vertex_pointer,
    .tex_coord_pointer = shader_tex_coord_pointer,
    .light_coord_pointer = shader_light_coord_pointer,
    .color_byte_pointer = shader_color_byte_pointer,
    .color_float_pointer = shader_color_float_pointer,
    .color = shader_color,
};
