/* V4L2 video picture grabber
   Copyright (C) 2009 Mauro Carvalho Chehab <mchehab@infradead.org>
   Copyright (C) 2015 Soochow University.

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

#define TICK_INTERVAL    50
#define IMG_DEFAULT_W   640
#define IMG_DEFAULT_H   480

struct buffer {
    void *start;
    size_t length;
};

/* configuration struct */
struct v4l2grabber {
    char *dev_name;
    int frame_count;
    int dry; /* 1 for display */
    int pix_width;
    int pix_height;

    int fd;
    Decoder *decoder; /* MJPEG to JPEG converter */
    SDL_Window *sdlWindow;
    SDL_Renderer *sdlRenderer;
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

static void uninit(struct v4l2grabber *grabber)
{
    decoder_destroy(grabber->decoder);
    if (grabber->dry) {
        SDL_DestroyRenderer(grabber->sdlRenderer);
        SDL_DestroyWindow(grabber->sdlWindow);
        IMG_Quit();
        SDL_Quit();
    }
}

static void process_image(const void *ptr, size_t size, int i)
{
    char out_name[256];
    FILE *fout;

    sprintf(out_name, "out%03d.jpg", i);
    fout = fopen(out_name, "w");
    if (!fout) {
        perror("Cannot open image");
        exit(EXIT_FAILURE);
    }
    fwrite(ptr, size, 1, fout);
    fclose(fout);
}

static int display_image(SDL_RWops *buffer_stream, SDL_Renderer *sdlRenderer)
{
    SDL_Surface* picture;
    SDL_Texture *texture;
    SDL_Event event;
    int quit = 0;

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
            quit = 1;
        break;
    case SDL_QUIT:
        quit = 1;
        break;
    default:
        break;
    }

    return quit;
}

static int read_frame(struct v4l2grabber *grabber,
                      struct buffer *buffers, int i)
{
    struct v4l2_buffer buf;
    int jpeg_size;
    unsigned char *out_buf = NULL;
    SDL_RWops* buffer_stream;
    int quit = 0;

    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    xioctl(grabber->fd, VIDIOC_DQBUF, &buf);

    jpeg_size = 0;
    jpeg_size = decoder_decode(grabber->decoder, &out_buf,
                               buffers[buf.index].start,
                               buf.bytesused);
    if (grabber->dry) {
        // Create a stream based on our buffer.
        if ( jpeg_size > 0 )
            buffer_stream = SDL_RWFromMem(out_buf, jpeg_size);
        else
            buffer_stream = SDL_RWFromMem(buffers[buf.index].start,
                                          buf.bytesused);

        if (display_image(buffer_stream, grabber->sdlRenderer))
            quit = 1;
    } else {
        if ( jpeg_size > 0 ) {
            process_image(out_buf, jpeg_size, i);
            free(out_buf);
        } else
            process_image(buffers[buf.index].start, buf.bytesused, i);
    }

    xioctl(grabber->fd, VIDIOC_QBUF, &buf);

    return quit;
}

static int mainloop(struct v4l2grabber *grabber, struct buffer *buffers)
{
    unsigned int i;
    fd_set fds;
    struct timeval tv;
    int r;

    /* main loop */
    for (i = 0; i < grabber->frame_count; i++) {
        do {
            FD_ZERO(&fds);
            FD_SET(grabber->fd, &fds);

            /* Timeout. */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(grabber->fd + 1, &fds, NULL, NULL, &tv);
        } while ((r == -1 && (errno = EINTR)));
        if (r == -1) {
            perror("select");
            return errno;
        }

        if (read_frame(grabber, buffers, i))
            i = INT_MAX;
    }

    return 0;
}

static int init_mmap(int fd, struct buffer **buffers)
{
    struct v4l2_requestbuffers req;
    unsigned int i, n_buffers;
    struct v4l2_buffer buf;

    CLEAR(req);
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(fd, VIDIOC_REQBUFS, &req);

    *buffers = calloc(req.count, sizeof(**buffers));
    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;

        xioctl(fd, VIDIOC_QUERYBUF, &buf);

        (*buffers)[n_buffers].length = buf.length;
        (*buffers)[n_buffers].start = v4l2_mmap(NULL, buf.length,
                                                PROT_READ | PROT_WRITE,
                                                MAP_SHARED, fd, buf.m.offset);

        if (MAP_FAILED == (*buffers)[n_buffers].start) {
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

    return n_buffers;
}

static void init_device(struct v4l2grabber *grabber)
{
    struct v4l2_format fmt;

    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = grabber->pix_width;
    fmt.fmt.pix.height = grabber->pix_height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    xioctl(grabber->fd, VIDIOC_S_FMT, &fmt);
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_JPEG) {
        printf("Libv4l didn't accept JPEG format. Trying MJPEG format.\n");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        xioctl(grabber->fd, VIDIOC_S_FMT, &fmt);
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
            printf("Libv4l didn't accept MJPEG format. Can't proceed.\n");
            exit(EXIT_FAILURE);
        }
    }
    if ((fmt.fmt.pix.width != grabber->pix_width)
        || (fmt.fmt.pix.height != grabber->pix_height)) {
        printf("Warning: driver is sending image at %dx%d\n",
               fmt.fmt.pix.width, fmt.fmt.pix.height);
        grabber->pix_width = fmt.fmt.pix.width;
        grabber->pix_height = fmt.fmt.pix.height;
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

static void parse_options(int argc, char **argv, struct v4l2grabber *grabber)
{
    int idx;
    int c;

    grabber->dev_name = "/dev/video0";
    grabber->frame_count = 3;
    grabber->dry = 0;
    grabber->pix_width = IMG_DEFAULT_W;
    grabber->pix_height = IMG_DEFAULT_H;

    for (;;) {

        c = getopt_long(argc, argv,
                        short_options, long_options, &idx);

        if (-1 == c)
            break;

        switch (c) {
        case 0: /* getopt_long() flag */
            break;

        case 'd':
            grabber->dev_name = optarg;
            break;

        case 'h':
            usage(stdout, argc, argv);
            exit(EXIT_SUCCESS);

        case 'c':
            errno = 0;
            grabber->frame_count = strtol(optarg, NULL, 0);
            if (errno)
                errno_exit(optarg);
            break;

        case 'n':
            grabber->dry = 1;
            break;

        default:
            usage(stderr, argc, argv);
            exit(EXIT_FAILURE);
        }
    }
}

int main(int argc, char **argv)
{
    enum v4l2_buf_type type;
    int r = 0;
    unsigned int i, n_buffers;
    struct buffer *buffers;

    struct v4l2grabber grabber;

    grabber.fd = -1;
    parse_options(argc, argv, &grabber);

    grabber.fd = v4l2_open(grabber.dev_name, O_RDWR | O_NONBLOCK, 0);
    if (grabber.fd < 0) {
        perror("Cannot open device");
        exit(EXIT_FAILURE);
    }

    init_device(&grabber);
    n_buffers = init_mmap(grabber.fd, &buffers);

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(grabber.fd, VIDIOC_STREAMON, &type);

    grabber.decoder = decoder_mjpeg_create();

    /* initiate display */
    if (grabber.dry) {
        grabber.frame_count = INT_MAX;

        // Initialise everything.
        SDL_Init(SDL_INIT_VIDEO);
        IMG_Init(IMG_INIT_JPG);

        grabber.sdlWindow = SDL_CreateWindow("Video Show",
                                             SDL_WINDOWPOS_UNDEFINED,
                                             SDL_WINDOWPOS_UNDEFINED,
                                             grabber.pix_width,
                                             grabber.pix_height, 0);
        grabber.sdlRenderer = SDL_CreateRenderer(grabber.sdlWindow, -1, 0);
    }

    r = mainloop(&grabber, buffers);

    uninit(&grabber);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(grabber.fd, VIDIOC_STREAMOFF, &type);
    for (i = 0; i < n_buffers; ++i)
        v4l2_munmap(buffers[i].start, buffers[i].length);
    v4l2_close(grabber.fd);

    return r;
}
