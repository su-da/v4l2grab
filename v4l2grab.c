/* V4L2 video picture grabber
   Copyright (C) 2009 Mauro Carvalho Chehab <mchehab@infradead.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <libv4l2.h>

#include <getopt.h>             /* getopt_long() */

#include "decoder.h"
#include "decoder_mjpeg.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer {
    void *start;
    size_t length;
};

static void errno_exit(const char *s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static void xioctl(int fh, int request, void *arg)
{
    int r;

    do {
        r = v4l2_ioctl(fh, request, arg);
    } while (r == -1 && ((errno == EINTR) || (errno == EAGAIN)));

    if (r == -1) {
        fprintf(stderr, "error %d, %s\n", errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

#define TICK_INTERVAL    50

Uint32 TimeLeft(void)
{
    Uint32 next_tick = 0;
    Uint32 cur_tick;

    cur_tick = SDL_GetTicks();
    if (next_tick <= cur_tick) {
        next_tick = cur_tick + TICK_INTERVAL;
        return 0;
    } else {
        return (next_tick - cur_tick);
    }
}

static void usage(FILE *fp, int argc, char **argv)
{
    fprintf(fp,
            "Usage: %s [options]\n\n"
            "Options:\n"
            "-d | --device name   Video device name [/dev/video0]\n"
            "-h | --help          Print this message\n"
            "-c | --count         Number of frames to grab [3]\n"
            "-n | --dry           Don't save images but display them\n"
            "",
            argv[0]);
}

static const char short_options[] = "d:hc:n";

static const struct option
long_options[] = {
        { "device", required_argument, NULL, 'd' },
        { "help",   no_argument,       NULL, 'h' },
        { "count",  required_argument, NULL, 'c' },
        { "dry",    no_argument,       NULL, 'n' },
        { 0, 0, 0, 0 }
};

int main(int argc, char **argv)
{
    struct v4l2_format fmt;
    struct v4l2_buffer buf;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type;
    fd_set fds;
    struct timeval tv;
    int r, fd = -1;
    unsigned int i, n_buffers;
    char *dev_name = "/dev/video0";
    char out_name[256];
    FILE *fout;
    struct buffer *buffers;
    int frame_count = 3;
    unsigned char *out_buf = NULL;
    Decoder *decoder = NULL;
    int jpeg_size;
    int dry = 0;

    SDL_Window *sdlWindow;
    SDL_Renderer *sdlRenderer;
    SDL_RWops* buffer_stream;
    SDL_Surface* picture;
    SDL_Texture *texture;
    SDL_Event event;

    for (;;) {
        int idx;
        int c;

        c = getopt_long(argc, argv,
                        short_options, long_options, &idx);

        if (-1 == c)
            break;

        switch (c) {
        case 0: /* getopt_long() flag */
            break;

        case 'd':
            dev_name = optarg;
            break;

        case 'h':
            usage(stdout, argc, argv);
            exit(EXIT_SUCCESS);

        case 'c':
            errno = 0;
            frame_count = strtol(optarg, NULL, 0);
            if (errno)
                errno_exit(optarg);
            break;

        case 'n':
            dry = 1;
            break;

        default:
            usage(stderr, argc, argv);
            exit(EXIT_FAILURE);
        }
    }

    fd = v4l2_open(dev_name, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        perror("Cannot open device");
        exit(EXIT_FAILURE);
    }

    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    xioctl(fd, VIDIOC_S_FMT, &fmt);
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_JPEG) {
        printf("Libv4l didn't accept JPEG format. Trying MJPEG format.\n");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        xioctl(fd, VIDIOC_S_FMT, &fmt);
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
            printf("Libv4l didn't accept MJPEG format. Can't proceed.\n");
            exit(EXIT_FAILURE);
        }
    }
    if ((fmt.fmt.pix.width != 640) || (fmt.fmt.pix.height != 480))
        printf("Warning: driver is sending image at %dx%d\n",
               fmt.fmt.pix.width, fmt.fmt.pix.height);

    CLEAR(req);
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(fd, VIDIOC_REQBUFS, &req);

    buffers = calloc(req.count, sizeof(*buffers));
    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;

        xioctl(fd, VIDIOC_QUERYBUF, &buf);

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = v4l2_mmap(NULL, buf.length,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, fd, buf.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start) {
            perror("mmap");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < n_buffers; ++i) {
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        xioctl(fd, VIDIOC_QBUF, &buf);
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    xioctl(fd, VIDIOC_STREAMON, &type);
    decoder = decoder_mjpeg_create();
    if (dry) {
        frame_count = INT_MAX;

        // Initialise everything.
        SDL_Init(SDL_INIT_VIDEO);
        IMG_Init(IMG_INIT_JPG);

        sdlWindow = SDL_CreateWindow("Video Show", SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED,
                                     fmt.fmt.pix.width,
                                     fmt.fmt.pix.height,
                                     0);
        sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, 0);
    }

    for (i = 0; i < frame_count; i++) {
        do {
            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout. */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(fd + 1, &fds, NULL, NULL, &tv);
        } while ((r == -1 && (errno = EINTR)));
        if (r == -1) {
            perror("select");
            decoder_destroy(decoder);
            if (dry) {
                SDL_DestroyRenderer(sdlRenderer);
                SDL_DestroyWindow(sdlWindow);
                IMG_Quit();
                SDL_Quit();
            }
            return errno;
        }

        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        xioctl(fd, VIDIOC_DQBUF, &buf);

        jpeg_size = 0;
        jpeg_size = decoder_decode(decoder, &out_buf,
                                   buffers[buf.index].start,
                                   buf.bytesused);
        if (dry) {
            // Create a stream based on our buffer.
            if ( jpeg_size > 0 )
                buffer_stream = SDL_RWFromMem(out_buf, jpeg_size);
            else
                buffer_stream = SDL_RWFromMem(buffers[buf.index].start,
                                              buf.bytesused);

            // Create a surface using the data coming out of the above stream.
            picture = IMG_Load_RW(buffer_stream, 1);
            texture = SDL_CreateTextureFromSurface(sdlRenderer, picture);
            SDL_FreeSurface(picture);
            SDL_RenderCopy(sdlRenderer, texture, NULL, NULL);
            SDL_DestroyTexture(texture);
            SDL_RenderPresent(sdlRenderer);
            SDL_Delay(TimeLeft());

            // event poll
            SDL_PollEvent(&event);
            switch (event.type) {
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (event.key.keysym.sym == SDLK_ESCAPE)
                    i = INT_MAX;
                break;
            case SDL_QUIT:
                i = INT_MAX;
                break;
            default:
                break;
            }
        } else {
            sprintf(out_name, "out%03d.jpg", i);
            fout = fopen(out_name, "w");
            if (!fout) {
                perror("Cannot open image");
                exit(EXIT_FAILURE);
            }
            if ( jpeg_size > 0 ) {
                fwrite(out_buf, jpeg_size, 1, fout);
                free(out_buf);
            } else
                fwrite(buffers[buf.index].start, buf.bytesused, 1, fout);
            fclose(fout);
        }

        xioctl(fd, VIDIOC_QBUF, &buf);
    }

    decoder_destroy(decoder);
    if (dry) {
        SDL_DestroyRenderer(sdlRenderer);
        SDL_DestroyWindow(sdlWindow);
        IMG_Quit();
        SDL_Quit();
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (i = 0; i < n_buffers; ++i)
        v4l2_munmap(buffers[i].start, buffers[i].length);
    v4l2_close(fd);

    return 0;
}
