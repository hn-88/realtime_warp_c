// prompted Grok with code from pdbourke/vlc-warp-2.1, which has the GPL license as below
/*****************************************************************************
 * opengl.c: OpenGL and OpenGL ES output common code
 *****************************************************************************
 * Copyright (C) 2004-2013 VLC authors and VideoLAN
 * Copyright (C) 2009, 2011 Laurent Aimar
 *
 * Authors: Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 *          Ilkka Ollakka <ileoo@videolan.org>
 *          Rémi Denis-Courmont
 *          Adrien Maglo <magsoft at videolan dot org>
 *          Felix Paul Kühne <fkuehne at videolan dot org>
 *          Pierre d'Herbemont <pdherbemont at videolan dot org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/* -------------------------------------------------------------
 *  video_player.c – Full-featured OpenGL video player
 *  - Hardware decoding (VAAPI / VideoToolbox / DXVA2)
 *  - Audio via SDL2
 *  - Seeking with ImGui progress bar
 *  - YUV-to-RGB via OpenGL (inspired by vlc-warp opengl.c)
 * ------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/glew.h>

#include <SDL2/SDL.h>

// === LOCAL IMGUI ===
#define IMGUI_IMPL_OPENGL_LOADER_GLEW
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#define WINDOW_WIDTH  960
#define WINDOW_HEIGHT 540

/* -------------------------------------------------------------
 *  OpenGL: shaders, quad, textures
 * ------------------------------------------------------------- */
static const char *vs_src = "#version 330 core\n"
    "layout(location=0) in vec2 p; layout(location=1) in vec2 uv;\n"
    "out vec2 vUV; void main(){ gl_Position=vec4(p,0,1); vUV=uv; }\n";

static const char *fs_src = "#version 330 core\n"
    "in vec2 vUV; out vec4 c;\n"
    "uniform sampler2D y,u,v;\n"
    "void main(){\n"
    "  float Y = texture(y,vUV).r;\n"
    "  float U = texture(u,vUV).r-0.5;\n"
    "  float V = texture(v,vUV).r-0.5;\n"
    "  c = vec4(Y+1.402*V, Y-0.344*U-0.714*V, Y+1.772*U, 1);\n"
    "}\n";

static GLuint prog, vao, vbo, ebo, texY, texU, texV;
static GLint locY, locU, locV;

/* -------------------------------------------------------------
 *  Video state
 * ------------------------------------------------------------- */
static AVFormatContext *fmt = NULL;
static AVCodecContext  *vdec = NULL, *adec = NULL;
static int vidx = -1, aidx = -1;
static AVFrame *vframe = NULL, *aframe = NULL;
static AVPacket pkt;
static struct SwsContext *sws = NULL;
static double duration = 0.0, pts = 0.0;
static bool seeking = false;
static int64_t seek_target = 0;

/* -------------------------------------------------------------
 *  Audio state (SDL)
 * ------------------------------------------------------------- */
static SDL_AudioDeviceID audio_dev;
static uint8_t *audio_buf = NULL;
static uint32_t audio_buf_size = 0, audio_buf_index = 0;

/* -------------------------------------------------------------
 *  Hardware decoding
 * ------------------------------------------------------------- */
static enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
static AVBufferRef *hw_device_ctx = NULL;

static int init_hw_decoder(AVCodecContext *ctx)
{
    for (int i = 0;; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(ctx->codec, i);
        if (!cfg) break;
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            cfg->device_type != AV_HWDEVICE_TYPE_NONE) {
            hw_type = cfg->device_type;
            if (av_hwdevice_ctx_create(&hw_device_ctx, hw_type, NULL, NULL, 0) < 0)
                continue;
            ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            printf("Using HW decoder: %s\n", av_hwdevice_get_type_name(hw_type));
            return 0;
        }
    }
    return -1;
}

/* -------------------------------------------------------------
 *  OpenGL setup
 * ------------------------------------------------------------- */
static void init_gl(void)
{
    glewInit();

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL); glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL); glCompileShader(fs);

    prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);

    locY = glGetUniformLocation(prog, "y");
    locU = glGetUniformLocation(prog, "u");
    locV = glGetUniformLocation(prog, "v");

    float verts[] = { -1,1,0,1, -1,-1,0,0, 1,1,1,1, 1,-1,1,0 };
    unsigned int idx[] = {0,1,2, 1,3,2};
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 4*sizeof(float), 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, 0, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);

    glGenTextures(1, &texY); glGenTextures(1, &texU); glGenTextures(1, &texV);
    for (int i = 0; i < 3; ++i) {
        GLuint t = (i==0)?texY:(i==1)?texU:texV;
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glUseProgram(prog);
    glUniform1i(locY, 0); glUniform1i(locU, 1); glUniform1i(locV, 2);
}

/* -------------------------------------------------------------
 *  Upload NV12 frame (from software or hardware)
 * ------------------------------------------------------------- */
static void upload_nv12(AVFrame *f, int w, int h)
{
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, f->data[0]);

    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, texU);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w/2, h/2, 0, GL_RED, GL_UNSIGNED_BYTE, f->data[1]);

    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, texV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w/2, h/2, 0, GL_RED, GL_UNSIGNED_BYTE, f->data[1]+1);
}

/* -------------------------------------------------------------
 *  Render frame
 * ------------------------------------------------------------- */
static void render_frame(AVFrame *f, int w, int h)
{
    glClear(GL_COLOR_BUFFER_BIT);
    upload_nv12(f, w, h);
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

/* -------------------------------------------------------------
 *  Audio callback (SDL)
 * ------------------------------------------------------------- */
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    if (audio_buf_index >= audio_buf_size) {
        SDL_memset(stream, 0, len);
        return;
    }
    int copy = audio_buf_size - audio_buf_index;
    if (copy > len) copy = len;
    SDL_memcpy(stream, audio_buf + audio_buf_index, copy);
    audio_buf_index += copy;
    if (copy < len)
        SDL_memset(stream + copy, 0, len - copy);
}

/* -------------------------------------------------------------
 *  Open file + streams
 * ------------------------------------------------------------- */
static int open_file(const char *path)
{
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) return -1;

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream *st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vidx < 0) vidx = i;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && aidx < 0) aidx = i;
    }
    if (vidx < 0) return -1;

    duration = fmt->duration * 1e-6;  // seconds

    /* --- Video --- */
    AVCodecParameters *vpar = fmt->streams[vidx]->codecpar;
    const AVCodec *vcodec = avcodec_find_decoder(vpar->codec_id);
    vdec = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(vdec, vpar);
    if (init_hw_decoder(vdec) < 0)
        printf("No HW decoder, using software\n");
    if (avcodec_open2(vdec, vcodec, NULL) < 0) return -1;

    /* --- Audio --- */
    if (aidx >= 0) {
        AVCodecParameters *apar = fmt->streams[aidx]->codecpar;
        const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
        adec = avcodec_alloc_context3(acodec);
        avcodec_parameters_to_context(adec, apar);
        if (avcodec_open2(adec, acodec, NULL) < 0)
            adec = NULL;
    }

    vframe = av_frame_alloc();
    if (aidx >= 0) aframe = av_frame_alloc();

    sws = sws_getContext(vdec->width, vdec->height, vdec->pix_fmt,
                         vdec->width, vdec->height, AV_PIX_FMT_NV12,
                         SWS_BILINEAR, NULL, NULL, NULL);
    return 0;
}

/* -------------------------------------------------------------
 *  Main loop
 * ------------------------------------------------------------- */
static void run(GLFWwindow *win, const char *path)
{
    if (open_file(path) < 0) { fprintf(stderr, "Failed to open file\n"); return; }

    /* --- Audio init --- */
    if (aidx >= 0 && adec) {
        SDL_AudioSpec want = {0}, have;
        want.freq = adec->sample_rate;
        want.format = AUDIO_S16SYS;
        want.channels = adec->channel_layout.nb_channels;
        want.samples = 1024;
        want.callback = audio_callback;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
    }

    /* --- Temp NV12 frame --- */
    AVFrame *nv12 = av_frame_alloc();
    uint8_t *buf = NULL;
    int size = av_image_get_buffer_size(AV_PIX_FMT_NV12, vdec->width, vdec->height, 1);
    buf = (uint8_t*)av_malloc(size);
    av_image_fill_arrays(nv12->data, nv12->linesize, buf, AV_PIX_FMT_NV12, vdec->width, vdec->height, 1);

    double start = glfwGetTime();
    double last_video = 0.0;

    while (!glfwWindowShouldClose(win)) {
        double now = glfwGetTime();
        double video_time = now - start;

        /* --- Seeking --- */
        if (seeking) {
            av_seek_frame(fmt, -1, seek_target * AV_TIME_BASE, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(vdec);
            if (adec) avcodec_flush_buffers(adec);
            start = now - (seek_target / 1000000.0);
            seeking = false;
            audio_buf_index = audio_buf_size = 0;
        }

        /* --- Decode loop --- */
        int got_video = 0;
        while (!got_video) {
            if (av_read_frame(fmt, &pkt) < 0) goto end;
            if (pkt.stream_index == vidx) {
                avcodec_send_packet(vdec, &pkt);
                if (avcodec_receive_frame(vdec, vframe) == 0) {
                    if (vframe->pts != AV_NOPTS_VALUE)
                        pts = vframe->pts * av_q2d(fmt->streams[vidx]->time_base);
                    got_video = 1;
                }
            } else if (aidx >= 0 && pkt.stream_index == aidx && adec) {
                avcodec_send_packet(adec, &pkt);
                while (avcodec_receive_frame(adec, aframe) == 0) {
                    int samples = aframe->nb_samples * aframe->channel_layout.nb_channels;
                    int bytes = samples * 2;
                    if ((uint32_t)(audio_buf_size - audio_buf_index) < (uint32_t)bytes) {
                        audio_buf_size = audio_buf_index + bytes;
                        audio_buf = (uint8_t*)av_realloc(audio_buf, audio_buf_size);
                    }
                    int16_t *dst = (int16_t*)(audio_buf + audio_buf_index);
                    for (int i = 0; i < samples; ++i)
                        dst[i] = ((int16_t*)aframe->data[0])[i];
                    audio_buf_index += bytes;
                }
            }
            av_packet_unref(&pkt);
        }

        if (got_video && video_time - last_video >= 1.0 / 30.0) {  // ~30 FPS cap
            sws_scale(sws, vframe->data, vframe->linesize, 0, vdec->height,
                      nv12->data, nv12->linesize);
            render_frame(nv12, vdec->width, vdec->height);
            last_video = video_time;
        }

        /* --- ImGui --- */
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Controls", NULL, ImGuiWindowFlags_AlwaysAutoResize);
        float pos = (float)(pts / duration * 100.0f);
        if (ImGui::SliderFloat("##seek", &pos, 0.0f, 100.0f, "%.2f %%")) {
            seek_target = (int64_t)(pos / 100.0 * duration * AV_TIME_BASE);
            seeking = true;
        }
        ImGui::Text("Duration: %.1f s", duration);
        ImGui::Text("Position: %.2f s", pts);
        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(win);
        glfwPollEvents();
    }

end:
    av_free(buf);
    av_frame_free(&nv12);
    av_frame_free(&vframe);
    av_frame_free(&aframe);
    avcodec_free_context(&vdec);
    if (adec) avcodec_free_context(&adec);
    avformat_close_input(&fmt);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    av_buffer_unref(&hw_device_ctx);
}

/* -------------------------------------------------------------
 *  Main
 * ------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <video>\n", argv[0]);
        return 1;
    }

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL init failed\n");
        return 1;
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *win = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Video Player", NULL, NULL);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    init_gl();
    run(win, argv[1]);

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glDeleteTextures(1, &texY); glDeleteTextures(1, &texU); glDeleteTextures(1, &texV);
    glDeleteVertexArrays(1, &vao); glDeleteBuffers(1, &vbo); glDeleteBuffers(1, &ebo);
    glDeleteProgram(prog);

    glfwDestroyWindow(win);
    glfwTerminate();
    SDL_Quit();
    return 0;
}
