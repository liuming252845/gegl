/* This file is an image processing operation for GEGL
 *
 * GEGL is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * GEGL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GEGL; if not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright 2003,2004,2007 Øyvind Kolås <pippin@gimp.org>
 */

#include "config.h"

#include <stdlib.h>

#include <glib/gi18n-lib.h>

#ifdef GEGL_PROPERTIES

property_string (path, _("File"), "/tmp/fnord.ogv")
    description (_("Target path and filename, use '-' for stdout."))
property_string (video_codec,   _("Audio codec"),   "auto")
property_int (video_bit_rate, _("video bitrate"), 810000)
    value_range (0.0, 500000000.0)
#if 0
property_double (video_bit_rate_tolerance, _("video bitrate"), 1000.0)
property_int    (video_global_quality, _("global quality"), 255)
property_int    (compression_level,    _("compression level"), 255)
property_int    (noise_reduction,      _("noise reduction strength"), 0)
property_int    (gop_size,             _("the number of frames in a group of pictures, 0 for keyframe only"), 16)
property_int    (key_int_min,          _("the minimum number of frames in a group of pictures, 0 for keyframe only"), 1)
property_int    (max_b_frames,         _("maximum number of consequetive b frames"), 3)
#endif
property_double (frame_rate, _("Frames/second"), 25.0)
    value_range (0.0, 100.0)

property_string (audio_codec, _("Audio codec"), "auto")
property_int (audio_sample_rate, _("Audio sample rate"), 48000)
property_int (audio_bit_rate, _("Audio bitrate"), 810000)

property_audio (audio, _("audio"), 0)

#else

#define GEGL_OP_SINK
#define GEGL_OP_C_SOURCE ff-save.c

/* bitrot cruft */
#define FF_API_OLD_ENCODE_AUDIO 1
#define FF_API_DUMP_FORMAT  1

#include "gegl-op.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#define MAX_AUDIO_CHANNELS  6
#define MAX_AUDIO_SAMPLES   2048

typedef struct AudioFrame {
  //int64_t          pts;
  float            data[MAX_AUDIO_CHANNELS][MAX_AUDIO_SAMPLES];
  int              channels;
  int              sample_rate;
  int              len;
  long             pos;
} AudioFrame;

typedef struct
{
  gdouble    frame;
  gdouble    frames;
  gdouble    width;
  gdouble    height;
  GeglBuffer *input;

  AVOutputFormat *fmt;
  AVFormatContext *oc;
  AVStream *video_st;

  AVFrame  *picture, *tmp_picture;
  uint8_t  *video_outbuf;
  int       frame_count, video_outbuf_size;

    /** the rest is for audio handling within oxide, note that the interface
     * used passes all used functions in the oxide api through the reg_sym api
     * of gggl, this means that the ops should be usable by other applications
     * using gggl directly,. without needing to link with the oxide library
     */
  AVStream *audio_st;

  void     *oxide_audio_instance;
  /*< non NULL audio_query,. means audio present */

  int32_t (*oxide_audio_query) (void *audio_instance,
                                uint32_t * sample_rate,
                                uint32_t * bits,
                                uint32_t * channels,
                                uint32_t * fragment_samples,
                                uint32_t * fragment_size);

  /* get audio samples for the current video frame, this should provide all
   * audiosamples associated with the frame, frame centering on audio stream is
   * undefined (FIXME:<<)
   */

  int32_t (*oxide_audio_get_fragment) (void *audio_instance, uint8_t * buf);

  uint32_t  sample_rate;
  uint32_t  bits;
  uint32_t  channels;
  uint32_t  fragment_samples;
  uint32_t  fragment_size;

  uint8_t  *fragment;

  int       buffer_size;
  int       buffer_read_pos;
  int       buffer_write_pos;
  uint8_t  *buffer; 
                   
  int       audio_outbuf_size;
  int       audio_input_frame_size;
  int16_t  *samples;
  uint8_t  *audio_outbuf;

  GList *audio_track;
  long   audio_pos;
  long   audio_read_pos;
} Priv;

static void
clear_audio_track (GeglProperties *o)
{
  Priv *p = (Priv*)o->user_data;
  while (p->audio_track)
    {
      g_free (p->audio_track->data);
      p->audio_track = g_list_remove (p->audio_track, p->audio_track->data);
    }
}

static void get_sample_data (Priv *p, long sample_no, float *left, float *right)
{
  int to_remove = 0;
  GList *l;
  l = p->audio_track;
  if (sample_no < 0)
    return;
  for (; l; l = l->next)
  {
    AudioFrame *af = l->data;
    if (sample_no > af->pos + af->len)
    {
      to_remove ++;
    }

    if (af->pos <= sample_no &&
        sample_no < af->pos + af->len)
      {
        int i = sample_no - af->pos;
        *left  = af->data[0][i];
        if (af->channels == 1)
          *right = af->data[0][i];
        else
          *right = af->data[1][i];

	if (to_remove)  /* consuming audiotrack */
        {
          again:
          for (l = p->audio_track; l; l = l->next)
          {
            AudioFrame *af = l->data;
            if (sample_no > af->pos + af->len)
            {
              p->audio_track = g_list_remove (p->audio_track, af);
              goto again;
            }
          }
        }
        return;
      }
  }
  //fprintf (stderr, "didn't find audio sample\n");
  *left  = 0;
  *right = 0;
}

#if 0
static void
buffer_open (GeglProperties *o, int size);
#endif

static void
init (GeglProperties *o)
{
  static gint inited = 0; /*< this is actually meant to be static, only to be done once */
  Priv       *p = (Priv*)o->user_data;

  if (p == NULL)
    {
      p = g_new0 (Priv, 1);
      o->user_data = (void*) p;
    }

  if (!inited)
    {
      av_register_all ();
      avcodec_register_all ();
      inited = 1;
    }

  clear_audio_track (o);
  p->audio_pos = 0;
  p->audio_read_pos = 0;

#ifndef DISABLE_AUDIO
  //p->oxide_audio_instance = gggl_op_sym (op, "oxide_audio_instance");
  //p->oxide_audio_query = gggl_op_sym (op, "oxide_audio_query()");
  //p->oxide_audio_get_fragment =
    //gggl_op_sym (op, "oxide_audio_get_fragment()");

  if (p->oxide_audio_instance && p->oxide_audio_query)
    {
      p->oxide_audio_query (p->oxide_audio_instance,
                            &p->sample_rate,
                            &p->bits,
                            &p->channels,
                            &p->fragment_samples, &p->fragment_size);

      /* FIXME: for now, the buffer is set to a size double that of a oxide
       * provided fragment,. should be enough no matter how things are handled,
       * but it should also be more than needed,. find out exact amount needed later
       */
#if 0
      if (!p->buffer)
        {
          int size =
            (p->sample_rate / o->frame_rate) * p->channels * (p->bits / 8) * 2;
          buffer_open (o, size);
        }
#endif
      if (!p->fragment)
        p->fragment = calloc (1, p->fragment_size);
    }
#endif
}

static void close_video       (Priv            *p,
                               AVFormatContext *oc,
                               AVStream        *st);
void        close_audio       (Priv            *p,
                               AVFormatContext *oc,
                               AVStream        *st);
static int  tfile             (GeglProperties  *o);
static void write_video_frame (GeglProperties  *o,
                               AVFormatContext *oc,
                               AVStream        *st);
static void write_audio_frame (GeglProperties      *o,
                               AVFormatContext *oc,
                               AVStream        *st);

#define STREAM_FRAME_RATE 25    /* 25 images/s */

#ifndef DISABLE_AUDIO
/* add an audio output stream */
static AVStream *
add_audio_stream (GeglProperties *o, AVFormatContext * oc, int codec_id)
{
  //Priv     *p = (Priv*)o->user_data;
  AVCodecContext *c;
  AVStream *st;

  //p = NULL;
  st = avformat_new_stream (oc, NULL);
  if (!st)
    {
      fprintf (stderr, "Could not alloc stream\n");
      exit (1);
    }

  c = st->codec;
  c->codec_id = codec_id;
  c->codec_type = AVMEDIA_TYPE_AUDIO;

  c->bit_rate = 64000;
  c->sample_rate = 44100;
  c->channels = 2;
  return st;
}
#endif

static void
open_audio (Priv * p, AVFormatContext * oc, AVStream * st)
{
  AVCodecContext *c;
  AVCodec  *codec;
  int i;

  c = st->codec;

  /* find the audio encoder */
  codec = avcodec_find_encoder (c->codec_id);
  if (!codec)
    {
      fprintf (stderr, "codec not found\n");
      exit (1);
    }
  c->bit_rate = 64000;
  c->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;

  if(1)if (codec->sample_fmts) {
    int i;
    for (i = 0; codec->sample_fmts[i]; i++)
    {
      if (codec->sample_fmts[i] == AV_SAMPLE_FMT_FLT)
        fprintf (stderr, "supports interleaved float!\n");
      if (codec->sample_fmts[i] == AV_SAMPLE_FMT_S16)
        fprintf (stderr, "supports interleaved s16!\n");
      if (codec->sample_fmts[i] == AV_SAMPLE_FMT_S16P)
        fprintf (stderr, "supports planar s16!\n");
      if (codec->sample_fmts[i] == AV_SAMPLE_FMT_FLTP)
        fprintf (stderr, "supports planar float!\n");
    }
  }

  c->sample_rate = 44100;
  c->channel_layout = AV_CH_LAYOUT_STEREO;
  c->channels = 2;


  if (codec->supported_samplerates)
  {
    c->sample_rate = codec->supported_samplerates[0];
    for (i = 0; codec->supported_samplerates[i]; i++)
      if (codec->supported_samplerates[i] == 44100)
         c->sample_rate = 44100;
  }
  st->time_base = (AVRational){1, c->sample_rate};

  c->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL; // ffmpeg AAC is not quite stable yet

  /* open it */
  if (avcodec_open2 (c, codec, NULL) < 0)
    {
      fprintf (stderr, "could not open codec\n");
      exit (1);
    }

  p->audio_outbuf_size = 10000;
  p->audio_outbuf = malloc (p->audio_outbuf_size);

  p->samples = malloc (p->audio_input_frame_size * 2 * c->channels);
}


/* XXX: rewrite  */
static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                  uint64_t channel_layout,
                                  int sample_rate, int nb_samples)
{
  AVFrame *frame = av_frame_alloc();
  int ret;

  if (!frame) {
      fprintf(stderr, "Error allocating an audio frame\n");
      exit(1);
  }

  frame->format         = sample_fmt;
  frame->channel_layout = channel_layout;
  frame->sample_rate    = sample_rate;
  frame->nb_samples     = nb_samples;

  if (nb_samples) {
      ret = av_frame_get_buffer(frame, 0);
      if (ret < 0) {
          fprintf(stderr, "Error allocating an audio buffer\n");
          exit(1);
      }
  }
  return frame;
}

void
write_audio_frame (GeglProperties *o, AVFormatContext * oc, AVStream * st)
{
  Priv *p = (Priv*)o->user_data;

  AVCodecContext *c;
  AVPacket  pkt;
  av_init_packet (&pkt);

  /* first we add incoming frames audio samples */
  {
    int i;
    AudioFrame *af = g_malloc0 (sizeof (AudioFrame));
    af->channels = 2; //o->audio->channels;
    af->len = o->audio->samples;
    for (i = 0; i < af->len; i++)
      {
        af->data[0][i] = o->audio->left[i];
        af->data[1][i] = o->audio->right[i];
      }
    af->pos = p->audio_pos;
    p->audio_pos += af->len;
    p->audio_track = g_list_append (p->audio_track, af);
  }

  /* then we encode as much as we can in a loop using the codec frame size */
  c = st->codec;
  
  fprintf (stderr, "%li %i %i\n",
           p->audio_pos, p->audio_read_pos, c->frame_size);
  while (p->audio_pos - p->audio_read_pos > c->frame_size)
  {
    long i;
    AVFrame *frame = alloc_audio_frame (c->sample_fmt, c->channel_layout,
                                        c->sample_rate, c->frame_size);

    for (i = 0; i < c->frame_size; i++)
    {
      float left = 0, right = 0;
      get_sample_data (p, i + p->audio_read_pos, &left, &right);
//    fprintf (stderr, "[%f %f]\n", left, right);
    }

    av_frame_free (&frame);
    p->audio_read_pos += c->frame_size;
  }

#if 0
  if(1)switch (c->sample_fmt)
  {
    case AV_SAMPLE_FMT_FLT:
      fprintf (stderr, "f\n");
      break;
    case AV_SAMPLE_FMT_FLTP:
      fprintf (stderr, "fp\n");
      break;
    case AV_SAMPLE_FMT_S16:
      fprintf (stderr, "s16\n");
      break;
    case AV_SAMPLE_FMT_S16P:
      fprintf (stderr, "s16p\n");
      break;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
      fprintf (stderr, "s32\n");
      break;
  }
#endif
  //fprintf (stderr, "asdfasdfadf\n");

  fprintf (stderr, "audio codec wants: %i samples\n", c->frame_size);

  p->audio_input_frame_size = c->frame_size;
  //fprintf (stderr, "going to grab %i %i\n", p->fragment_size, o->audio->samples);
#if 0
  if (p->oxide_audio_get_fragment (p->oxide_audio_instance,
                                   p->fragment) == (signed) p->fragment_size)
    {
      buffer_write (p, p->fragment, p->fragment_size);
    }
#endif

#if 0
  while (buffer_used (p) >= p->audio_input_frame_size * 2 * c->channels)
    {
      buffer_read (p, (uint8_t *) p->samples,
                   p->audio_input_frame_size * 2 * c->channels);

      pkt.size = avcodec_encode_audio (c, p->audio_outbuf,
                                       p->audio_outbuf_size, p->samples);

      pkt.pts = c->coded_frame->pts;
      pkt.flags |= AV_PKT_FLAG_KEY;
      pkt.stream_index = st->index;
      pkt.data = p->audio_outbuf;

      if (av_write_frame (oc, &pkt) != 0)
        {
          fprintf (stderr, "Error while writing audio frame\n");
          exit (1);
        }
    }
#endif
}

/*p->audio_get_frame (samples, audio_input_frame_size, c->channels);*/

void
close_audio (Priv * p, AVFormatContext * oc, AVStream * st)
{
  avcodec_close (st->codec);

  av_free (p->samples);
  av_free (p->audio_outbuf);
}

/* add a video output stream */
static AVStream *
add_video_stream (GeglProperties *o, AVFormatContext * oc, int codec_id)
{
  Priv *p = (Priv*)o->user_data;

  AVCodecContext *c;
  AVStream *st;

  st = avformat_new_stream (oc, NULL);
  if (!st)
    {
      fprintf (stderr, "Could not alloc stream %p %p %i\n", o, oc, codec_id);
      exit (1);
    }

  c = st->codec;
  c->codec_id = codec_id;
  c->codec_type = AVMEDIA_TYPE_VIDEO;

  /* put sample propeters */
  c->bit_rate = o->video_bit_rate;
  /* resolution must be a multiple of two */
  c->width = p->width;
  c->height = p->height;
  /* frames per second */
  st->time_base =(AVRational){1, o->frame_rate};
  c->time_base = st->time_base;
  c->pix_fmt = AV_PIX_FMT_YUV420P;

  c->gop_size = 12;             /* emit one intra frame every twelve frames at most */
  if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO)
    {
      c->max_b_frames = 2;
    }
  if (c->codec_id == AV_CODEC_ID_H264)
   {
#if 1
     c->qcompress = 0.6;  // qcomp=0.6
     c->me_range = 16;    // me_range=16
     c->gop_size = 250;   // g=250

     c->max_b_frames = 3; // bf=3
#if 0
     c->coder_type = 1;  // coder = 1
     c->flags|=CODEC_FLAG_LOOP_FILTER;   // flags=+loop
     c->me_cmp|= 1;  // cmp=+chroma, where CHROMA = 1
     //c->partitions|=X264_PART_I8X8+X264_PART_I4X4+X264_PART_P8X8+X264_PART_B8X8; // partitions=+parti8x8+parti4x4+partp8x8+partb8x8
     c->me_subpel_quality = 7;   // subq=7
     c->keyint_min = 25; // keyint_min=25
     c->scenechange_threshold = 40;  // sc_threshold=40
     c->i_quant_factor = 0.71; // i_qfactor=0.71
     c->b_frame_strategy = 1;  // b_strategy=1
     c->qmin = 10;   // qmin=10
     c->qmax = 51;   // qmax=51
     c->max_qdiff = 4;   // qdiff=4
     c->refs = 3;    // refs=3
     //c->directpred = 1;  // directpred=1
     c->flags |= CODEC_FLAG_GLOBAL_HEADER;
     c->trellis = 1; // trellis=1
     //c->flags2|=AV_CODEC_FLAG2_BPYRAMID|AV_CODEC_FLAG2_MIXED_REFS|AV_CODEC_FLAG2_WPRED+CODEC_FLAG2_8X8DCT+CODEC_FLAG2_FASTPSKIP;  // flags2=+bpyramid+mixed_refs+wpred+dct8x8+fastpskip
     //c->weighted_p_pred = 2; // wpredp=2

// libx264-main.ffpreset preset
     //c->flags2|=CODEC_FLAG2_8X8DCT;c->flags2^=CODEC_FLAG2_8X8DCT;
#endif
#endif
   }

   if (oc->oformat->flags & AVFMT_GLOBALHEADER)
     c->flags |= CODEC_FLAG_GLOBAL_HEADER;


/*    if (!strcmp (oc->oformat->name, "mp4") ||
          !strcmp (oc->oformat->name, "3gp"))
    c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    */
  return st;
}


static AVFrame *
alloc_picture (int pix_fmt, int width, int height)
{
  AVFrame  *picture;
  uint8_t  *picture_buf;
  int       size;

  picture = avcodec_alloc_frame ();
  if (!picture)
    return NULL;
  size = avpicture_get_size (pix_fmt, width, height + 1);
  picture_buf = malloc (size);
  if (!picture_buf)
    {
      av_free (picture);
      return NULL;
    }
  avpicture_fill ((AVPicture *) picture, picture_buf, pix_fmt, width, height);
  return picture;
}

static void
open_video (Priv * p, AVFormatContext * oc, AVStream * st)
{
  AVCodec  *codec;
  AVCodecContext *c;

  c = st->codec;

  /* find the video encoder */
  codec = avcodec_find_encoder (c->codec_id);
  if (!codec)
    {
      fprintf (stderr, "codec not found\n");
      exit (1);
    }


  /* open the codec */
  if (avcodec_open2 (c, codec, NULL) < 0)
    {
      fprintf (stderr, "could not open codec\n");
      exit (1);
    }

  p->video_outbuf = NULL;
  if (!(oc->oformat->flags & AVFMT_RAWPICTURE))
    {
      /* allocate output buffer */
      /* XXX: API change will be done */
      p->video_outbuf_size = 200000;
      p->video_outbuf = malloc (p->video_outbuf_size);
    }

  /* allocate the encoded raw picture */
  p->picture = alloc_picture (c->pix_fmt, c->width, c->height);
  if (!p->picture)
    {
      fprintf (stderr, "Could not allocate picture\n");
      exit (1);
    }

  /* if the output format is not YUV420P, then a temporary YUV420P
     picture is needed too. It is then converted to the required
     output format */
  p->tmp_picture = NULL;
  if (c->pix_fmt != PIX_FMT_RGB24)
    {
      p->tmp_picture = alloc_picture (PIX_FMT_RGB24, c->width, c->height);
      if (!p->tmp_picture)
        {
          fprintf (stderr, "Could not allocate temporary picture\n");
          exit (1);
        }
    }
}

static void
close_video (Priv * p, AVFormatContext * oc, AVStream * st)
{
  avcodec_close (st->codec);
  av_free (p->picture->data[0]);
  av_free (p->picture);
  if (p->tmp_picture)
    {
      av_free (p->tmp_picture->data[0]);
      av_free (p->tmp_picture);
    }
  av_free (p->video_outbuf);
}

#include "string.h"

/* prepare a dummy image */
static void
fill_rgb_image (GeglProperties *o,
                AVFrame *pict, int frame_index, int width, int height)
{
  Priv     *p = (Priv*)o->user_data;
  GeglRectangle rect={0,0,width,height};
  gegl_buffer_get (p->input, &rect, 1.0, babl_format ("R'G'B' u8"), pict->data[0], GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
}

static void
write_video_frame (GeglProperties *o,
                   AVFormatContext *oc, AVStream *st)
{
  Priv     *p = (Priv*)o->user_data;
  int       out_size, ret;
  AVCodecContext *c;
  AVFrame  *picture_ptr;

  c = st->codec;

  if (c->pix_fmt != PIX_FMT_RGB24)
    {
      struct SwsContext *img_convert_ctx;
      fill_rgb_image (o, p->tmp_picture, p->frame_count, c->width,
                      c->height);

      img_convert_ctx = sws_getContext(c->width, c->height, PIX_FMT_RGB24,
                                       c->width, c->height, c->pix_fmt,
                                       SWS_BICUBIC, NULL, NULL, NULL);

      if (img_convert_ctx == NULL)
        {
          fprintf(stderr, "ff_save: Cannot initialize conversion context.");
        }
      else
        {
          sws_scale(img_convert_ctx,
                    (void*)p->tmp_picture->data,
                    p->tmp_picture->linesize,
                    0,
                    c->height,
                    p->picture->data,
                    p->picture->linesize);
         p->picture->format = c->pix_fmt;
         p->picture->width = c->width;
         p->picture->height = c->height;
        }
    }
  else
    {
      fill_rgb_image (o, p->picture, p->frame_count, c->width, c->height);
    }

  picture_ptr = p->picture;
  picture_ptr->pts = p->frame_count;

  if (oc->oformat->flags & AVFMT_RAWPICTURE)
    {
      /* raw video case. The API will change slightly in the near
         future for that */
      AVPacket  pkt;
      av_init_packet (&pkt);

      pkt.flags |= AV_PKT_FLAG_KEY;
      pkt.stream_index = st->index;
      pkt.data = (uint8_t *) picture_ptr;
      pkt.size = sizeof (AVPicture);
      pkt.pts = picture_ptr->pts;
      av_packet_rescale_ts (&pkt, c->time_base, st->time_base);

      ret = av_write_frame (oc, &pkt);
    }
  else
    {
      /* encode the image */
      out_size =
        avcodec_encode_video (c,
                              p->video_outbuf,
                              p->video_outbuf_size, picture_ptr);

      /* if zero size, it means the image was buffered */
      if (out_size != 0)
        {
          AVPacket  pkt;
          av_init_packet (&pkt);
          if (c->coded_frame->key_frame)
            pkt.flags |= AV_PKT_FLAG_KEY;
          pkt.stream_index = st->index;
          pkt.data = p->video_outbuf;
          pkt.size = out_size;
          pkt.pts = picture_ptr->pts;
          av_packet_rescale_ts (&pkt, c->time_base, st->time_base);
          /* write the compressed frame in the media file */
          ret = av_write_frame (oc, &pkt);
        }
      else
        {
          ret = 0;
        }
    }
  if (ret != 0)
    {
      fprintf (stderr, "Error while writing video frame\n");
      exit (1);
    }
  p->frame_count++;
}

static int
tfile (GeglProperties *o)
{
  Priv *p = (Priv*)o->user_data;

  p->fmt = av_guess_format (NULL, o->path, NULL);
  if (!p->fmt)
    {
      fprintf (stderr,
               "ff_save couldn't deduce outputformat from file extension: using MPEG.\n%s",
               "");
      p->fmt = av_guess_format ("mpeg", NULL, NULL);
    }
  p->oc = avformat_alloc_context ();
  if (!p->oc)
    {
      fprintf (stderr, "memory error\n%s", "");
      return -1;
    }

  p->oc->oformat = p->fmt;

  snprintf (p->oc->filename, sizeof (p->oc->filename), "%s", o->path);

  p->video_st = NULL;
  p->audio_st = NULL;

  if (p->fmt->video_codec != CODEC_ID_NONE)
    {
      p->video_st = add_video_stream (o, p->oc, p->fmt->video_codec);
    }
  if (p->fmt->audio_codec != CODEC_ID_NONE)
    {
     p->audio_st = add_audio_stream (o, p->oc, p->fmt->audio_codec);
    }

  av_dump_format (p->oc, 0, o->path, 1);

  if (p->video_st)
    open_video (p, p->oc, p->video_st);
  if (p->audio_st)
    open_audio (p, p->oc, p->audio_st);

  if (avio_open (&p->oc->pb, o->path, AVIO_FLAG_WRITE) < 0)
    {
      fprintf (stderr, "couldn't open '%s'\n", o->path);
      return -1;
    }

  avformat_write_header (p->oc, NULL);
  return 0;
}

#if 0
static int
filechanged (GeglOpOperation *op, const char *att)
{
  init (op);
  return 0;
}
#endif

static gboolean
process (GeglOperation       *operation,
         GeglBuffer          *input,
         const GeglRectangle *result,
         gint                 level)
{
  static gint inited = 0;
  GeglProperties *o = GEGL_PROPERTIES (operation);
  Priv       *p = (Priv*)o->user_data;

  g_assert (input);

  if (p == NULL)
    init (o);
  p = (Priv*)o->user_data;

  p->width = result->width;
  p->height = result->height;
  p->input = input;

  if (!inited)
    {
      tfile (o);
      inited = 1;
    }

  write_video_frame (o, p->oc, p->video_st);
  if (p->audio_st)
    write_audio_frame (o, p->oc, p->audio_st);

  return  TRUE;
}

static void
finalize (GObject *object)
{
  GeglProperties *o = GEGL_PROPERTIES (object);
  if (o->user_data)
    {
      Priv *p = (Priv*)o->user_data;
#if 0
    buffer_close (o);
#endif

    if (p->oc)
      {
        if (p->video_st)
          close_video (p, p->oc, p->video_st);
        if (p->audio_st)
          close_audio (p, p->oc, p->audio_st);

        av_write_trailer (p->oc);
#if 0
{
        //gint i;
        for (i = 0; i < p->oc->nb_streams; i++)
          {
            av_freep (&p->oc->streams[i]);
          }
}
#endif

        avio_closep (&p->oc->pb);
        avformat_free_context (p->oc);
      }

      g_free (o->user_data);
      o->user_data = NULL;
    }

  G_OBJECT_CLASS (g_type_class_peek_parent (G_OBJECT_GET_CLASS (object)))->finalize (object);
}


static void
gegl_op_class_init (GeglOpClass *klass)
{
  GeglOperationClass     *operation_class;
  GeglOperationSinkClass *sink_class;

  G_OBJECT_CLASS (klass)->finalize = finalize;

  operation_class = GEGL_OPERATION_CLASS (klass);
  sink_class      = GEGL_OPERATION_SINK_CLASS (klass);

  sink_class->process = process;
  sink_class->needs_full = TRUE;

  gegl_operation_class_set_keys (operation_class,
    "name"        , "gegl:ff-save",
    "categories"  , "output:video",
    "description" , _("FFmpeg video output sink"),
    NULL);
}

#endif
