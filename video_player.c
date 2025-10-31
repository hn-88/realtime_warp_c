// prompted Grok with code from VLC, which has the GPL license as below
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/glew.h>

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600

// Shaders inspired by vlc-warp's opengl.c
const char *vertexShaderSrc = R"(
#version 330 core
layout (location = 0) in vec2 position;
layout (location = 1) in vec2 texCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    TexCoord = texCoord;
}
)";

const char *fragmentShaderSrc = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;
uniform sampler2D yTex;
uniform sampler2D uTex;
uniform sampler2D vTex;
void main() {
    float y = texture(yTex, TexCoord).r;
    float u = texture(uTex, TexCoord).r - 0.5;
    float v = texture(vTex, TexCoord).r - 0.5;
    float r = y + 1.402 * v;
    float g = y - 0.344 * u - 0.714 * v;
    float b = y + 1.772 * u;
    FragColor = vec4(r, g, b, 1.0);
}
)";

// OpenGL resources
GLuint shaderProgram, VAO, VBO, yTexture, uTexture, vTexture;
GLint uniY, uniU, uniV;

// Video state
AVFormatContext *fmtCtx = NULL;
AVCodecContext *codecCtx = NULL;
AVCodec *codec = NULL;
AVFrame *frame = NULL;
AVPacket packet;
struct SwsContext *swsCtx = NULL;
int videoStream = -1;
double frameRate = 30.0;
double lastTime = 0.0;

// Quad vertices (full screen)
float quadVertices[] = {
    // positions   // texCoords
    -1.0f,  1.0f,  0.0f, 1.0f,
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 0.0f
};
unsigned int indices[] = { 0, 1, 2, 1, 3, 2 };

static GLuint compileShader(const char *src, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        printf("Shader compilation failed: %s\n", infoLog);
        exit(1);
    }
    return shader;
}

static void initOpenGL() {
    // Init GLEW
    glewInit();

    // Shaders
    GLuint vertexShader = compileShader(vertexShaderSrc, GL_VERTEX_SHADER);
    GLuint fragmentShader = compileShader(fragmentShaderSrc, GL_FRAGMENT_SHADER);
    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint success;
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        printf("Shader linking failed: %s\n", infoLog);
        exit(1);
    }

    uniY = glGetUniformLocation(shaderProgram, "yTex");
    uniU = glGetUniformLocation(shaderProgram, "uTex");
    uniV = glGetUniformLocation(shaderProgram, "vTex");

    // Quad
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);  // Assuming you have unsigned int EBO; add it globally if needed
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Textures
    glGenTextures(1, &yTexture);
    glGenTextures(1, &uTexture);
    glGenTextures(1, &vTexture);

    glBindTexture(GL_TEXTURE_2D, yTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, uTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, vTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glUseProgram(shaderProgram);
    glUniform1i(uniY, 0);
    glUniform1i(uniU, 1);
    glUniform1i(uniV, 2);
}

static void uploadFrame(AVFrame *avFrame, int width, int height) {
    // For simplicity, assume NV12 (common planar YUV); adapt for I420 if needed
    // Y plane
    glBindTexture(GL_TEXTURE_2D, yTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, avFrame->data[0]);

    // UV plane (downsampled, chroma)
    glBindTexture(GL_TEXTURE_2D, uTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, avFrame->data[1]);

    glBindTexture(GL_TEXTURE_2D, vTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width / 2, height / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, avFrame->data[1] + 1);  // Interleaved UV
}

static void renderFrame(AVFrame *avFrame, int width, int height) {
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(shaderProgram);
    glBindVertexArray(VAO);

    // Active textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, yTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, vTexture);

    uploadFrame(avFrame, width, height);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

static int openVideo(const char *filename) {
    avformat_network_init();
    if (avformat_open_input(&fmtCtx, filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open video file\n");
        return -1;
    }
    if (avformat_find_stream_info(fmtCtx, NULL) < 0) {
        fprintf(stderr, "Could not find stream info\n");
        return -1;
    }

    for (int i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }
    if (videoStream == -1) {
        fprintf(stderr, "No video stream found\n");
        return -1;
    }

    AVCodecParameters *codecPar = fmtCtx->streams[videoStream]->codecpar;
    codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) {
        fprintf(stderr, "Unsupported codec\n");
        return -1;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codecCtx, codecPar) < 0) {
        fprintf(stderr, "Could not copy codec params\n");
        return -1;
    }
    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return -1;
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate frame\n");
        return -1;
    }

    // SWS for YUV conversion if needed (here we assume decoder outputs YUV)
    swsCtx = sws_getContext(codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
                            codecCtx->width, codecCtx->height, AV_PIX_FMT_NV12,
                            SWS_BILINEAR, NULL, NULL, NULL);

    frameRate = av_q2d(fmtCtx->streams[videoStream]->r_frame_rate);
    return 0;
}

static void closeVideo() {
    if (swsCtx) sws_freeContext(swsCtx);
    if (frame) av_frame_free(&frame);
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (fmtCtx) avformat_close_input(&fmtCtx);
}

static void glfwError(int id, const char *description) {
    fprintf(stderr, "GLFW Error %d: %s\n", id, description);
}

static void runPlayer(GLFWwindow *window, const char *filename) {
    if (openVideo(filename) < 0) exit(1);

    initOpenGL();

    av_read_frame(fmtCtx, &packet);  // Prime the packet

    while (!glfwWindowShouldClose(window)) {
        double currentTime = glfwGetTime();
        if (currentTime - lastTime >= 1.0 / frameRate) {
            lastTime = currentTime;

            // Decode
            if (avcodec_send_packet(codecCtx, &packet) == 0) {
                while (avcodec_receive_frame(codecCtx, frame) == 0) {
                    // Convert if needed (assume NV12 output)
                    AVFrame *yuvFrame = av_frame_alloc();
                    av_image_fill_arrays(yuvFrame->data, yuvFrame->linesize, NULL,
                                         AV_PIX_FMT_NV12, codecCtx->width, codecCtx->height, 1);
                    sws_scale(swsCtx, frame->data, frame->linesize, 0, codecCtx->height,
                              yuvFrame->data, yuvFrame->linesize);

                    renderFrame(yuvFrame, codecCtx->width, codecCtx->height);

                    av_frame_free(&yuvFrame);
                }
            }
            av_packet_unref(&packet);
            if (av_read_frame(fmtCtx, &packet) < 0) break;  // EOF
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    closeVideo();
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <video_file>\n", argv[0]);
        return 1;
    }

    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Custom OpenGL Video Player", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);

    // Add global EBO declaration if not already (for compilation)
    unsigned int EBO;

    runPlayer(window, argv[1]);

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &EBO);
    glDeleteProgram(shaderProgram);
    glDeleteTextures(1, &yTexture);
    glDeleteTextures(1, &uTexture);
    glDeleteTextures(1, &vTexture);

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
