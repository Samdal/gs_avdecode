#ifndef GS_AVDECODE_H_
#define GS_AVDECODE_H_

/*
 * Header only style....
 * #define GS_AVDECODE_IMPL
 * before including header in one and only one source file
 * to implement declared functions.
 *
 * On windows you might want to #define GS_AVDECODE_NO_DECLARE_CLOCK_GETTIME
 * if you have problems with it trying to implement a windows version of clock_gettime
 *
 * Requirers linking with ffmpeg stuff.
 * On gcc/clang that would for example be
 *       -lavcodec -lavformat -lavcodec -lswresample -lswscale -lavutil
 *
 *
 * Requires at least Lavc55.38.100 for mutex locking.
 *       https://stackoverflow.com/a/39786484
 *
 *
 * +--------------------------------------------------------------------------+
 *
 * ffmpeg/av codec docs:
 *   https://ffmpeg.org/doxygen/trunk/
 *   This is a very simple implementation with little documentation.
 *   If you have questions, look up what the avcodec functions used do.
 *
 *
 * +--------------------------------------------------------------------------+
 *
 * Supports:
 * - Playing a video in a variety of container formats
 * - Automatic selection of suitable ffmpeg decoder
 * - Conversion from YUV etc... to RGB(A)
 * - Transparent VP9 and VP8 video
 * - Decoding videos on a seperate thread, with efficient atomic communication
 * - Texture creation for gunslinger (can be omitted)
 *
 * Does not support:
 * - Audio (FIXME)
 * - Variable sized videos
 * - Variable frame rate
 * - Multiple streams in one container
 * - Subtitles
 * - Hardware accelerated decoding (FIXME)
 *
 * Simlpe stuff (like changing logging levels) does not have a unique interface,
 * just use av codec functions.
 *
 * There are no current plans of implementing the mentioned stuff that isn't
 * supported currently. If someone wants to do them then feel free.
 *
 *
 * +--------------------------------------------------------------------------+
 *
 * Not all formats and so on have been well tested.
 * "Bugs" in some files has been experienced.
 *
 *
 * +--------------------------------------------------------------------------+
 *
 * BSD 3-Clause License
 *
 * Copyright (c) 2023, Halvard Samdal
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>

#include <libswscale/swscale.h>

#include <gs/gs.h>

#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

_Static_assert(LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 38, 100), "FFmpeg major version too low: " LIBAVCODEC_IDENT ", needs Lavc55.38.100");

typedef struct gs_avdecode_context_s {
        const char* src_filename;
        int width, height;
        int img_sz; // size of the allocated buffers, padded to double word
        int alpha;
        enum AVPixelFormat pix_fmt;
        float frametime;
        int video_stream_idx, audio_stream_idx;
        int frame_eof;
        int dont_send_new_frame;

        AVFormatContext *fmt_ctx;
        AVCodecContext *video_dec_ctx, *audio_dec_ctx;
        AVStream *video_stream, *audio_stream;
        AVFrame *frame;
        AVPacket *pkt;
        struct SwsContext * sws;

        void* img[1];
} gs_avdecode_context_t;

////////////
// returns zero on success
extern int gs_avdecode_init(const char* path, gs_avdecode_context_t* ctx, const gs_graphics_texture_desc_t* desc, gs_asset_texture_t* out);

//////////
// returns negative on error (including reaching stream end)
extern int gs_avdecode_next_frame(gs_avdecode_context_t* ctx);

//////////
// resets context
// this is *REQUIRED* if you hit EOF
extern void gs_avdecode_flush(gs_avdecode_context_t* ctx);

extern void gs_avdecode_destroy(gs_avdecode_context_t* ctx, gs_asset_texture_t* tex);

enum avdecode_seek_flags {
        AVDECODE_SEEK_BACKWARD = AVSEEK_FLAG_BACKWARD,
        AVDECODE_SEEK_BYTE = AVSEEK_FLAG_BYTE,
        AVDECODE_SEEK_ANY = AVSEEK_FLAG_ANY,
        AVDECODE_SEEK_FRAME = AVSEEK_FLAG_FRAME,
};

// >= 0 on success
extern int gs_avdecode_seek(gs_avdecode_context_t* ctx, int64_t timestamp,
                            enum avdecode_seek_flags flags);

extern void gs_avdecode_request_upload_to_texture(gs_command_buffer_t* cb, gs_avdecode_context_t* ctx, gs_asset_texture_t* tex);


//////////////////////////////////
// Multi-Thread stuff
//
// Atomic integers are used instead of locks (for performance reasons)
//

enum avdecode_pthread_states {
        AVDECODE_START,     // Worker thread will change to RUNNING asap
        AVDECODE_DONE,      // Worker thread will await state changes (and not touch anything else)

        AVDECODE_SINGLE,    // worker thread will change to RUNNING asap,
                            // and once a single frame is decoded, it will turn to DONE
                            // Unlike a normal start, single will NOT wait for the frame to be read.
                            //            (AVDECODE_FRAME_COMPLETE will still be set)

        AVDECODE_RUNNING,   // Worker thread is decoding, when the last frame is read it changes to DONE
        AVDECODE_STOP,      // Will froce decoding thread to switch to DONE asap

        AVDECODE_DIE = -1,  // Worker thread will exit on next state check
        AVDECODE_DEAD = -2, // Worker thread has exited
};
// Changing state from RUNNING does not guarantuee immidiate
// withdrawal from accessing common data.
// To ensure that, you must set it to STOP and then wait for DONE.
//
// Setting state to DEAD and RUNNING on main thread is illegal
//////////////////


enum avdecode_pthread_lock {
        AVDECODE_FRAME_COMPLETE = 1, // frame is complete and either thread can do a cmpxchg to aquire it.
        AVDECODE_DECODING = 0,       // the main thread shall not access anything
        AVDECODE_WAIT = -1,          // the decoder thread shall not access anything
};

typedef struct gs_avdecode_pthread_s {
        pthread_t thread;
        pthread_attr_t attr;

        gs_avdecode_context_t video;

        _Atomic int new_frame;
        _Atomic int loop; // 0 = don't loop, >0 loop x times, <0 loop forever
        _Atomic enum avdecode_pthread_states state;
} gs_avdecode_pthread_t;

////////////////////
// if init_decoder is true it will call gs_avdecode_init(ctxp->video)
// The thread is detached, doing pthread_join is not needed.
// returns 0 on success
//
// if you set state to DONE before calling this, it will not auto-start (otherwise START is state = 0)
extern int gs_avdecode_pthread_play_video(gs_avdecode_pthread_t* ctxp, const char* path, int init_decoder,
                                          const gs_graphics_texture_desc_t* desc, gs_asset_texture_t* out);

////////////
// ensures decoder thread has exited
extern void
gs_avdecode_pthread_destroy(gs_avdecode_pthread_t* ctxp, gs_asset_texture_t* tex);


#define gs_avdecode_try_aquire_m(_ctxp, ...)                            \
        do {                                                            \
                int __check = AVDECODE_FRAME_COMPLETE;                  \
                if (atomic_compare_exchange_strong(&(_ctxp)->new_frame, &__check, AVDECODE_WAIT)) { \
                        __VA_ARGS__                                     \
                                (_ctxp)->new_frame = AVDECODE_DECODING; \
                }                                                       \
        } while(0)
// returns 1 on succesfull upload request
// returns 0 on failed
extern int gs_avdecode_try_request_upload_to_texture(gs_command_buffer_t* cb, gs_avdecode_pthread_t* ctx, gs_asset_texture_t* tex);





//////////////////////////////////
// Implementation
//

#ifdef GS_AVDECODE_IMPL

static int
_gs_avdecode_decode_packet(gs_avdecode_context_t* ctx, AVCodecContext *dec, const AVPacket *pkt, int new_pkt)
{
        int ret = 0;

        if (new_pkt) {
        send_packet:
                ret = avcodec_send_packet(dec, pkt);
                if (ret < 0) {
                        if (ret == AVERROR_EOF)
                                return 0;
                        av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Error sending a packet for decoding (%s)\n", av_err2str(ret));
                        return ret;
                }
        }

        // get all the available frames from the decoder
        int valid_frame = 1;
        do {
                ret = avcodec_receive_frame(dec, ctx->frame);
                if (ret < 0) {
                        // those two return values are special and mean there is no output
                        // frame available, but there were no errors during decoding
                        if (ret == AVERROR(EAGAIN))
                                goto send_packet;
                        if (ret == AVERROR_EOF)
                                return 0;
                        av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Error during decoding (%s)\n", av_err2str(ret));
                        return ret;
                }
                if (dec->codec->type == AVMEDIA_TYPE_VIDEO) {
                        if (ctx->frame->width != ctx->width   ||
                            ctx->frame->height != ctx->height ||
                            ctx->frame->format != ctx->pix_fmt) {
                                // No support for variable size videos...
                                // To do so you would have to reallocate img
                                valid_frame = 0;
                        } else {
                                sws_scale(ctx->sws,
                                          (const uint8_t **)(ctx->frame->data), ctx->frame->linesize,
                                          0, ctx->height, (uint8_t**)ctx->img, (int[AV_NUM_DATA_POINTERS]){
                                                  ctx->width * (3 + ctx->alpha), 0
                                          }
                                        );
                        }
                }

                av_frame_unref(ctx->frame);
        } while (!valid_frame);

        return 1;
}

static int
_gs_avdecode_open_codec_context(
        gs_avdecode_context_t* ctx,
        int *stream_idx,
        AVCodecContext **dec_ctx,
        AVFormatContext *fmt_ctx,
        int* alpha,
        enum AVMediaType type
        ) {
        int ret, stream_index;
        AVStream *st;
        const AVCodec *dec = NULL;

        ret = av_find_best_stream(ctx->fmt_ctx, type, -1, -1, NULL, 0);
        if (ret < 0) {
                av_log(NULL, AV_LOG_INFO, "gs_avdecode.h: Could not find %s stream in input file '%s'\n",
                       av_get_media_type_string(type), ctx->src_filename);
                return ret;
        } else {
                stream_index = ret;
                st = ctx->fmt_ctx->streams[stream_index];

                int a = 0;
                if (alpha) {
                        AVDictionaryEntry* tag = NULL;
                        tag = av_dict_get(st->metadata, "ALPHA_MODE", tag, 0);
                        a = tag && atoi(tag->value);
                        *alpha = a;
                }

                // find decoder for the stream
                // use libvpx for transparent video
                dec = a ? (
                        AV_CODEC_ID_VP9 == st->codecpar->codec_id
                        ? avcodec_find_decoder_by_name("libvpx-vp9")
                        : avcodec_find_decoder_by_name("libvpx-vp8")
                        ) : avcodec_find_decoder(st->codecpar->codec_id);
                if (!dec) {
                        av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Failed to find %s codec\n",
                               av_get_media_type_string(type));
                        return AVERROR(EINVAL);
                }

                /* Allocate a codec context for the decoder */
                *dec_ctx = avcodec_alloc_context3(dec);
                if (!*dec_ctx) {
                        av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Failed to allocate the %s codec context\n",
                               av_get_media_type_string(type));
                        return AVERROR(ENOMEM);
                }

                /* Copy codec parameters from input stream to output codec context */
                if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
                        av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Failed to copy %s codec parameters to decoder context\n",
                               av_get_media_type_string(type));
                        return ret;
                }

                // set codec to automatically determine how many threads suits best for the decoding job
                dec_ctx[0]->thread_count = 0;

                if (dec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
                        dec_ctx[0]->thread_type = FF_THREAD_FRAME;
                else if (dec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
                        dec_ctx[0]->thread_type = FF_THREAD_SLICE;
                else
                        dec_ctx[0]->thread_count = 1; //don't use multithreading


                // Init the decoders
                if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
                        av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Failed to open %s codec\n",
                               av_get_media_type_string(type));
                        return ret;
                }
                *stream_idx = stream_index;
        }

        return 0;
}

int
gs_avdecode_init(const char* path, gs_avdecode_context_t* ctx, const gs_graphics_texture_desc_t* desc, gs_asset_texture_t* out)
{
        if (!ctx) return 2;
        ctx->video_stream_idx = -1;
        ctx->audio_stream_idx = -1;
        ctx->fmt_ctx = NULL;
        ctx->video_stream = NULL;
        ctx->audio_stream = NULL;
        ctx->frame = NULL;
        ctx->pkt = NULL;
        ctx->video_dec_ctx = NULL;
        ctx->audio_dec_ctx = NULL;
        ctx->src_filename = path;

        /* open input file, and allocate format context */
        if (avformat_open_input(&ctx->fmt_ctx, ctx->src_filename, NULL, NULL) < 0) {
                av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Could not open source file %s\n", ctx->src_filename);
                return 1;
        }

        /* retrieve stream information */
        if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0) {
                av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Could not find stream information\n");
                avformat_close_input(&ctx->fmt_ctx);
                return 1;
        }

        int ret = 0;
        if (_gs_avdecode_open_codec_context(ctx, &ctx->video_stream_idx, &ctx->video_dec_ctx, ctx->fmt_ctx, &ctx->alpha, AVMEDIA_TYPE_VIDEO) >= 0) {
                ctx->video_stream = ctx->fmt_ctx->streams[ctx->video_stream_idx];
                if (!ctx->frametime)
                        ctx->frametime = (float)ctx->video_stream->r_frame_rate.den / (float)ctx->video_stream->r_frame_rate.num;

                ctx->width = ctx->video_dec_ctx->width;
                ctx->height = ctx->video_dec_ctx->height;
                ctx->pix_fmt = ctx->video_dec_ctx->pix_fmt;
                if (ctx->alpha && ctx->pix_fmt == AV_PIX_FMT_YUV420P)
                        ctx->pix_fmt = AV_PIX_FMT_YUVA420P;
                ctx->sws = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt,
                                          ctx->width, ctx->height, ctx->alpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24,
                                          SWS_BICUBIC, 0, 0, 0);
        }

        if (_gs_avdecode_open_codec_context(ctx, &ctx->audio_stream_idx, &ctx->audio_dec_ctx, ctx->fmt_ctx, NULL, AVMEDIA_TYPE_AUDIO) >= 0) {
                ctx->audio_stream = ctx->fmt_ctx->streams[ctx->audio_stream_idx];
        }

        // dump input information to stderr
        av_dump_format(ctx->fmt_ctx, 0, ctx->src_filename, 0);

        if (!ctx->audio_stream && !ctx->video_stream) {
                av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Could not find audio or video stream in the input, aborting\n");
                ret = 1;
                goto end;
        }

        ctx->frame = av_frame_alloc();
        if (!ctx->frame) {
                av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Could not allocate frame\n");
                ret = AVERROR(ENOMEM);
                goto end;
        }

        ctx->pkt = av_packet_alloc();
        if (!ctx->pkt) {
                av_log(NULL, AV_LOG_ERROR, "gs_avdecode.h: Could not allocate packet\n");
                ret = AVERROR(ENOMEM);
                goto end;
        }


        ctx->img_sz = ctx->width * ctx->height * (3 + ctx->alpha);
        ctx->img_sz += ctx->img_sz % 32; // sws_scale REQUIRES padding to (double) word alignment
        *ctx->img = malloc(ctx->img_sz);
        memset(*ctx->img, 150, ctx->img_sz);

        ////////////////////////////////////
        // asset_texture

        gs_asset_texture_t* t = out;
        if (t) {
                if (desc) {
                        t->desc = *desc;
                } else {
                        t->desc.format = ctx->alpha ? GS_GRAPHICS_TEXTURE_FORMAT_RGBA8 : GS_GRAPHICS_TEXTURE_FORMAT_RGB8;
                        t->desc.min_filter = GS_GRAPHICS_TEXTURE_FILTER_LINEAR;
                        t->desc.mag_filter = GS_GRAPHICS_TEXTURE_FILTER_LINEAR;
                        t->desc.wrap_s = GS_GRAPHICS_TEXTURE_WRAP_REPEAT;
                        t->desc.wrap_t = GS_GRAPHICS_TEXTURE_WRAP_REPEAT;
                }
                t->desc.width = ctx->width;
                t->desc.height = ctx->height;

                *t->desc.data = malloc(ctx->img_sz);
                memset(*t->desc.data, 150, ctx->img_sz);
                t->hndl = gs_graphics_texture_create(&t->desc);
        }

        return 0;
end:
        gs_avdecode_destroy(ctx, NULL);
        return ret;
}

int
gs_avdecode_next_frame(gs_avdecode_context_t* ctx)
{
        int ret;
read_packet_again:
        ret = 0;
        int new = 0;
        if (!ctx->frame_eof) {
                int frame_res = av_read_frame(ctx->fmt_ctx, ctx->pkt);
                if (frame_res == AVERROR_EOF) {
                        ctx->frame_eof = 1;
                } else if (frame_res < 0) {
                        return -1;
                } else {
                        new = 1;
                        ctx->dont_send_new_frame = 0;
                }
        }
        // check if the packet belongs to a stream we are interested in, otherwise skip it
        if (ctx->pkt->stream_index == ctx->video_stream_idx) {
                ret = _gs_avdecode_decode_packet(ctx, ctx->video_dec_ctx, ctx->frame_eof ? NULL : ctx->pkt, !ctx->dont_send_new_frame);
        } else if (ctx->pkt->stream_index == ctx->audio_stream_idx) {
                //ret = _gs_avdecode_decode_packet(ctx, ctx->audio_dec_ctx, ctx->frame_eof ? NULL : ctx->pkt, new);
                if (new) av_packet_unref(ctx->pkt);
                goto read_packet_again;
        } else {
                if (new) av_packet_unref(ctx->pkt);
                goto read_packet_again;
        }

        if (new)
                av_packet_unref(ctx->pkt);

        if (ret < 0)
                return ret;

        if (ret == 0) {
                if (ctx->frame_eof)
                        return -1;
        } else {
                ctx->dont_send_new_frame = 1;
        }

        return ret;
}

void
gs_avdecode_flush(gs_avdecode_context_t* ctx)
{
        int ret;
        if (ctx->video_dec_ctx) {
                int once = 1;
                do {
                        ret = _gs_avdecode_decode_packet(ctx, ctx->video_dec_ctx, NULL, once);
                        once = 0;
                } while (ret > 0);
                avcodec_flush_buffers(ctx->video_dec_ctx);
        }
        if (ctx->audio_dec_ctx) {
                int once = 1;
                do {
                        ret = _gs_avdecode_decode_packet(ctx, ctx->audio_dec_ctx, NULL, 1);
                        once = 0;
                } while (ret > 0);
                avcodec_flush_buffers(ctx->audio_dec_ctx);
        }
}

void
gs_avdecode_destroy(gs_avdecode_context_t* ctx, gs_asset_texture_t* tex)
{
        if (!ctx) return;
        gs_avdecode_flush(ctx);

        avcodec_free_context(&ctx->video_dec_ctx);
        avcodec_free_context(&ctx->audio_dec_ctx);
        avformat_close_input(&ctx->fmt_ctx);
        av_packet_free(&ctx->pkt);
        sws_freeContext(ctx->sws);
        av_frame_free(&ctx->frame);

        if (tex) {
                gs_graphics_texture_destroy(tex->hndl);
                free(*tex->desc.data);
        }
}

int
gs_avdecode_seek(gs_avdecode_context_t* ctx, int64_t timestamp,
                 enum avdecode_seek_flags flags)
{
        if (!ctx) return -1;
        if (!ctx->fmt_ctx) return -1;

        int ret = 0;
        while (!ctx->dont_send_new_frame && ret >= 0)
                ret = gs_avdecode_next_frame(ctx);
        ctx->frame_eof = 0;
        ctx->dont_send_new_frame = 0;
        ctx->pkt->pos = -1;
        gs_avdecode_flush(ctx);

        return av_seek_frame(ctx->fmt_ctx, ctx->video_stream_idx, timestamp, flags);
}

#ifdef _WIN32
#ifndef GS_AVDECODE_NO_DECLARE_CLOCK_GETTIME
// untested
struct timespec { long tv_sec; long tv_nsec; };
static int
clock_gettime(int, struct timespec *spec)
{
        __int64 wintime;
        GetSystemTimeAsFileTime((FILETIME*)&wintime);
        wintime      -= 116444736000000000i64;       // 1jan1601 to 1jan1970
        spec->tv_sec  = wintime / 10000000i64;       // seconds
        spec->tv_nsec = wintime % 10000000i64 * 100; // nano-seconds
        return 0;
}
#endif // GS_AVDECODE_NO_DECLARE_CLOCK_GETTIME
#endif // _WIN32

////////////////////
// NOTE(Halvard):
// The thread uses the gs_platform function for sleep.
// In general it should be safe to assume that the platofrm implementation
// of this functions is MT-safe. However that is not a guarantee.
static void*
_gs_avdecode_pthread_player(void* data)
{
        gs_avdecode_pthread_t* ctxp = data;
        gs_avdecode_context_t* ctx = &ctxp->video;
        const double dt = 0.01;


pthread_decoder_start: ;
        int single = 0;
        // check for state changes
        for (; ; gs_platform_sleep(dt)) {
                int check;
                int exit;

                check = AVDECODE_STOP;
                atomic_compare_exchange_strong(&ctxp->state, &check, AVDECODE_DONE);

                check = AVDECODE_START;
                exit = atomic_compare_exchange_strong(&ctxp->state, &check, AVDECODE_RUNNING);
                if (exit) break;

                check = AVDECODE_SINGLE;
                exit = atomic_compare_exchange_strong(&ctxp->state, &check, AVDECODE_RUNNING);
                if (exit) {
                        single = 1;
                        break;
                }

                check = AVDECODE_DIE;
                exit = atomic_compare_exchange_strong(&ctxp->state, &check, AVDECODE_DEAD);
                if (exit) return NULL;
        }

        // wait to get lock
        for (int has_lock = 0; ; gs_platform_sleep(dt)) {
                int check = AVDECODE_FRAME_COMPLETE;
                if (ctxp->new_frame == AVDECODE_DECODING)
                        has_lock = 1;
                else
                        has_lock = atomic_compare_exchange_strong(&ctxp->new_frame, &check, AVDECODE_DECODING);
                if (has_lock) break;
        }

        const double frametime = ctx->frametime * 1e3;
        int frames = 0;

        struct timespec ts_start;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        int res = 0;
        int prerendered = 0;
        for (;;) {
                if (ctxp->state != AVDECODE_RUNNING)
                        goto pthread_decoder_start;

                if (frames == 1 && ctxp->new_frame == AVDECODE_FRAME_COMPLETE)
                        continue;

                if (!prerendered && ctxp->new_frame == AVDECODE_DECODING) {
                        res = gs_avdecode_next_frame(ctx);
                        prerendered = 1;
                }

                double diff = 0.0;
                struct timespec ts_check;
                clock_gettime(CLOCK_MONOTONIC, &ts_check);
                diff  = (ts_check.tv_sec  - ts_start.tv_sec)  * 1e3;
                diff += (ts_check.tv_nsec - ts_start.tv_nsec) * 1e-6;

                if (diff + dt < frametime * (double)frames) {
                        gs_platform_sleep(dt);
                        continue;
                }
                int locked = 0;
                if (ctxp->new_frame == AVDECODE_DECODING) {
                        locked = 1;
                } else {
                        int check = AVDECODE_FRAME_COMPLETE;
                        locked = atomic_compare_exchange_strong(&ctxp->new_frame, &check, AVDECODE_DECODING);
                }
                if (!locked)
                        continue;

                if (!prerendered) res = gs_avdecode_next_frame(ctx);
                else prerendered = 0;
                ctxp->new_frame = AVDECODE_FRAME_COMPLETE;
                frames++;

                if (res < 0) break;
                if (single) break;
        }

        if (!single)
                while (ctxp->new_frame != AVDECODE_DECODING) gs_platform_sleep(dt);


        if (ctxp->loop) {
                gs_avdecode_seek(&ctxp->video, INT64_MIN, AVDECODE_SEEK_BACKWARD);
                ctxp->state = AVDECODE_START;
                ctxp->loop--;
                goto pthread_decoder_start;
        }

        ctxp->state = AVDECODE_DONE;
        goto pthread_decoder_start;
}

void
gs_avdecode_request_upload_to_texture(gs_command_buffer_t* cb, gs_avdecode_context_t* ctx, gs_asset_texture_t* tex)
{
        memcpy(*tex->desc.data, *ctx->img, ctx->img_sz);
        gs_graphics_texture_request_update(cb, tex->hndl, &tex->desc);
}

int
gs_avdecode_pthread_play_video(gs_avdecode_pthread_t* ctxp, const char* path, int init_decoder,
                               const gs_graphics_texture_desc_t* desc, gs_asset_texture_t* out)
{
        int res = 0;
        if (!ctxp) return 2;

        if (init_decoder) {
                res = gs_avdecode_init(path, &ctxp->video, desc, out);
                if (res) return res;
        }

        int pares = pthread_attr_init(&ctxp->attr);
        if (pares) {
                if (init_decoder) gs_avdecode_destroy(&ctxp->video, out);
                return pares;
        }
        pthread_attr_setdetachstate(&ctxp->attr, PTHREAD_CREATE_DETACHED);

        int pres = pthread_create(&ctxp->thread, &ctxp->attr, &_gs_avdecode_pthread_player, ctxp);
        pthread_attr_destroy(&ctxp->attr);
        if (pres) {
                if (init_decoder) gs_avdecode_destroy(&ctxp->video, out);
                return pres;
        }

        return res;
}

void
gs_avdecode_pthread_destroy(gs_avdecode_pthread_t* ctxp, gs_asset_texture_t* tex)
{
        if (!ctxp) return;
        if (ctxp->state != AVDECODE_DEAD)
                ctxp->state = AVDECODE_DIE;
        while (ctxp->state != AVDECODE_DEAD) ;

        gs_avdecode_destroy(&ctxp->video, tex);
}

int
gs_avdecode_try_request_upload_to_texture(gs_command_buffer_t* cb, gs_avdecode_pthread_t* ctxp, gs_asset_texture_t* tex)
{
        int check = AVDECODE_FRAME_COMPLETE;
        int ret = atomic_compare_exchange_strong(&ctxp->new_frame, &check, AVDECODE_WAIT);
        if (ret) {
                gs_avdecode_request_upload_to_texture(cb, &ctxp->video, tex);
                ctxp->new_frame = AVDECODE_DECODING;
        }
        return ret;
}

#endif // GS_AVDECODE_IMPL

#endif // GS_AVDECODE_H_
