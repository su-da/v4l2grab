/**
 * File: decoder_mjpeg.c
 * Author: Joe Shang (shangchuanren@gmail.com)
 * Brief: The decoder of mjpeg format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "huffman.h"

#include "decoder.h"
#include "decoder_mjpeg.h"

static int is_huffman(unsigned char *buf)
{
    int i = 0;
    unsigned char *pbuf = buf;

    while (((pbuf[0] << 8) | pbuf[1]) != 0xffda)
    {
        if (i++ > 2048)
        {
            return 0;
        }
        
        if (((pbuf[0] << 8) | pbuf[1]) == 0xffc4)
        {
            return 1;
        }

        pbuf++;
    }

    return 0;
}

static int decoder_mjpeg_decode(Decoder *thiz, 
        unsigned char **out_buf, 
        unsigned char *in_buf,
        int buf_size)
{
    int pos = 0;
    int size_start = 0;
    unsigned char *pdeb = in_buf;
    unsigned char *pcur = in_buf;
    unsigned char *plimit = in_buf + buf_size;
    unsigned char *jpeg_buf;

    if (is_huffman(in_buf))
    {
#ifdef DECODER_DEBUG
        printf("huffman\n");
#endif
    }
    else
    {
#ifdef DECODER_DEBUG
        printf("no huffman\n");
#endif

        /* find the SOF0(Start Of Frame 0) of JPEG */
        while ( (((pcur[0] << 8) | pcur[1]) != 0xffc0) && (pcur < plimit) )
        {
            pcur++;
        }

        /* SOF0 of JPEG exist */
        if (pcur < plimit)
        {
#ifdef DECODER_DEBUG
            printf("SOF0 existed at position\n");
#endif
            jpeg_buf = malloc(buf_size + sizeof(dht_data) + 10);

            if (jpeg_buf != NULL)
            {
                /* insert huffman table after SOF0 */
                size_start = pcur - pdeb;
                memcpy(jpeg_buf, in_buf, size_start);
                pos += size_start;

                memcpy(jpeg_buf + pos, dht_data, sizeof(dht_data));
                pos += sizeof(dht_data);

                memcpy(jpeg_buf + pos, pcur, buf_size - size_start);
                pos += buf_size - size_start;

                *out_buf = jpeg_buf;

                /* Caller must free the buffer. */
                /* free(jpeg_buf);
                jpeg_buf = NULL; */
            }
        }
    }

    return pos;
}

static void decoder_mjpeg_destroy(Decoder *thiz)
{
    if (thiz != NULL)
    {
        free(thiz);
    }
}

Decoder *decoder_mjpeg_create(int mjpeg_size)
{
    Decoder *thiz = malloc(sizeof(Decoder));
    
    if (thiz != NULL)
    {
        thiz->decode = decoder_mjpeg_decode;
        thiz->destroy = decoder_mjpeg_destroy;
    }

    return thiz;
}

