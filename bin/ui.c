/* This file is part of GEGL editor -- an mrg frontend for GEGL
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2015, 2018 Øyvind Kolås pippin@gimp.org
 */

/* The code in this file is an image viewer/editor written using microraptor
 * gui and GEGL. It renders the UI directly from GEGLs data structures.
 */

#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include "config.h"

#if HAVE_MRG

#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <mrg.h>
#include <gegl.h>
#include <gexiv2/gexiv2.h>
#include <gegl-paramspecs.h>
#include <gegl-operation.h>
#include <gegl-audio-fragment.h>

/* set this to 1 to print the active gegl chain
 */
// #define DEBUG_OP_LIST  1

void mrg_gegl_blit (Mrg *mrg,
                    float x0, float y0,
                    float width, float height,
                    GeglNode *node,
                    float u, float v,
                    float scale,
                    float preview_multiplier);

void mrg_gegl_buffer_blit (Mrg *mrg,
                           float x0, float y0,
                           float width, float height,
                           GeglBuffer *buffer,
                           float u, float v,
                           float scale,
                           float preview_multiplier);

static GeglNode *gegl_node_get_consumer_no (GeglNode *node,
                                            const char *output_pad,
                                            const char **consumer_pad,
                                            int no)
{
  GeglNode *consumer = NULL;
  GeglNode **nodes = NULL;
  const gchar **consumer_names = NULL;
  int count;
  if (node == NULL)
    return NULL;

  count = gegl_node_get_consumers (node, "output", &nodes, &consumer_names);
  if (count > no){
     /* XXX : look into inverting list in get_consumers */
     consumer = nodes[count-no-1];
     if (consumer_pad)
       *consumer_pad = g_intern_string (consumer_names[count-no-1]);
  }
  g_free (nodes);
  g_free (consumer_names);
  return consumer;
}



//static int audio_start = 0; /* which sample no is at the start of our circular buffer */

static int audio_started = 0;

enum {
  GEGL_RENDERER_BLIT = 0,
  GEGL_RENDERER_BLIT_MIPMAP,
  GEGL_RENDERER_THREAD,
  GEGL_RENDERER_IDLE,
  //GEGL_RENDERER_IDLE_MIPMAP,
  //GEGL_RENDERER_THREAD_MIPMAP,
};
static int renderer = GEGL_RENDERER_BLIT;

/*  this structure contains the full application state, and is what
 *  re-renderings of the UI is directly based on.
 */
typedef struct _State State;
struct _State {
  void      (*ui) (Mrg *mrg, void *state);
  Mrg        *mrg;
  char       *path;
  char       *save_path;
  GList      *paths;
  GeglBuffer *buffer;
  GeglNode   *gegl;
  GeglNode   *sink;
  GeglNode   *source;
  GeglNode   *save;
  GeglNode   *active;
  GeglNode   *rotate;
  GThread    *thread;

  GeglNode      *processor_node; /* the node we have a processor for */
  GeglProcessor *processor;
  GeglBuffer    *processor_buffer;
  int            renderer_state;
  int            editing_op_name;
  char        new_opname[1024];
  int         rev;
  float       u, v;
  float       scale;
  int         show_graph;
  int         show_controls;
  float       render_quality;
  float       preview_quality;
  int         controls_timeout;
  char      **ops; // the operations part of the commandline, if any
  float       slide_pause;
  int         slide_enabled;
  int         slide_timeout;

  GeglNode   *gegl_decode;
  GeglNode   *decode_load;
  GeglNode   *decode_store;
  int         is_video;
  int         frame_no;
  int         prev_frame_played;
  double      prev_ms;
};

static char *suffix = "-gegl";


void   gegl_meta_set (const char *path, const char *meta_data);
char * gegl_meta_get (const char *path);
GExiv2Orientation path_get_orientation (const char *path);

static char *suffix_path (const char *path);
static char *unsuffix_path (const char *path);
static int is_gegl_path (const char *path);

static void contrasty_stroke (cairo_t *cr);

gchar *get_thumb_path (const char *path);
gchar *get_thumb_path (const char *path)
{
  gchar *ret;
  gchar *uri = g_strdup_printf ("file://%s", path);
  gchar *hex = g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
  int i;
  for (i = 0; hex[i]; i++)
    hex[i] = tolower (hex[i]);
  ret = g_strdup_printf ("%s/.cache/thumbnails/large/%s.png", g_get_home_dir(), hex);
  if (access (ret, F_OK) == -1)
  {
    g_free (ret);
    ret = g_strdup_printf ("%s/.cache/thumbnails/normal/%s.png", g_get_home_dir(), hex);
  }
  g_free (uri);
  g_free (hex);
  return ret;
}


static void load_path (State *o);

static void go_next (State *o);
static void go_prev (State *o);
static void go_parent                 (State *o);
static void get_coords                (State *o, float  screen_x, float screen_y,
                                                 float *gegl_x,   float *gegl_y);
static void drag_preview              (MrgEvent *e);
static void load_into_buffer          (State *o, const char *path);
static void zoom_to_fit               (State *o);
static void center                    (State *o);
static void zoom_to_fit_buffer        (State *o);
static void go_next_cb                (MrgEvent *event, void *data1, void *data2);
static void go_prev_cb                (MrgEvent *event, void *data1, void *data2);
static void go_parent_cb              (MrgEvent *event, void *data1, void *data2);
static void zoom_fit_cb               (MrgEvent *e, void *data1, void *data2);
static void pan_left_cb               (MrgEvent *event, void *data1, void *data2);
static void pan_right_cb              (MrgEvent *event, void *data1, void *data2);
static void pan_down_cb               (MrgEvent *event, void *data1, void *data2);
static void pan_up_cb                 (MrgEvent *event, void *data1, void *data2);
static void preview_more_cb           (MrgEvent *event, void *data1, void *data2);
static void preview_less_cb           (MrgEvent *event, void *data1, void *data2);
static void zoom_1_cb                 (MrgEvent *event, void *data1, void *data2);
static void zoom_in_cb                (MrgEvent *event, void *data1, void *data2);
static void zoom_out_cb               (MrgEvent *event, void *data1, void *data2);
static void toggle_actions_cb         (MrgEvent *event, void *data1, void *data2);
static void toggle_fullscreen_cb      (MrgEvent *event, void *data1, void *data2);
static void discard_cb                (MrgEvent *event, void *data1, void *data2);
static void save_cb                   (MrgEvent *event, void *data1, void *data2);
static void toggle_show_controls_cb   (MrgEvent *event, void *data1, void *data2);
static void gegl_ui                   (Mrg *mrg, void *data);
int         mrg_ui_main               (int argc, char **argv, char **ops);
void        gegl_meta_set             (const char *path, const char *meta_data);
char *      gegl_meta_get             (const char *path); 
static void on_viewer_motion          (MrgEvent *e, void *data1, void *data2);
int         gegl_str_has_image_suffix (char *path);
int         gegl_str_has_video_suffix (char *path);

static int str_has_visual_suffix (char *path)
{
  return gegl_str_has_image_suffix (path) || gegl_str_has_video_suffix (path);
}

static void populate_path_list (State *o)
{
  struct dirent **namelist;
  int i;
  struct stat stat_buf;
  char *path = strdup (o->path);
  int n;
  while (o->paths)
    {
      char *freed = o->paths->data;
      o->paths = g_list_remove (o->paths, freed);
      g_free (freed);
    }

  lstat (o->path, &stat_buf);
  if (S_ISREG (stat_buf.st_mode))
  {
    char *lastslash = strrchr (path, '/');
    if (lastslash)
      {
        if (lastslash == path)
          lastslash[1] = '\0';
        else
          lastslash[0] = '\0';
      }
  }
  n = scandir (path, &namelist, NULL, alphasort);

  for (i = 0; i < n; i++)
  {
    if (namelist[i]->d_name[0] != '.' &&
        str_has_visual_suffix (namelist[i]->d_name))
    {
      gchar *fpath = g_strdup_printf ("%s/%s", path, namelist[i]->d_name);

      lstat (fpath, &stat_buf);
      if (S_ISREG (stat_buf.st_mode))
      {
        if (is_gegl_path (fpath))
        {
          char *tmp = unsuffix_path (fpath);
          g_free (fpath);
          fpath = g_strdup (tmp);
          free (tmp);
        }
        if (!g_list_find_custom (o->paths, fpath, (void*)g_strcmp0))
        {
          o->paths = g_list_append (o->paths, fpath);
        }
      }
    }
  }
  free (namelist);
}

static State *hack_state = NULL;  // XXX: this shoudl be factored away

char **ops = NULL;

static void open_audio (Mrg *mrg, int frequency)
{
  mrg_pcm_set_sample_rate (mrg, frequency);
  mrg_pcm_set_format (mrg, MRG_s16S);
}

static void end_audio (void)
{
}

static gboolean renderer_task (gpointer data)
{
  State *o = data;
  static gdouble progress = 0.0;
  void *old_processor = o->processor;
  GeglBuffer *old_buffer = o->processor_buffer;
  switch (o->renderer_state)
  {
    case 0:
      {
      if (o->processor_node != o->sink)
      {
        o->processor = gegl_node_new_processor (o->sink, NULL);
        o->processor_buffer = g_object_ref (gegl_processor_get_buffer (o->processor));
        if (old_buffer)
          g_object_unref (old_buffer);
        if (old_processor)
          g_object_unref (old_processor);

        gegl_processor_set_rectangle (o->processor, NULL);
      }
      o->renderer_state = 1;
      }
      break; // fallthrough
    case 1:

       if (gegl_processor_work (o->processor, &progress))
         o->renderer_state = 1;
       else
         o->renderer_state = 2;
      break;
    case 2:
      switch (renderer)
      {
        case GEGL_RENDERER_IDLE:
          mrg_queue_draw (o->mrg, NULL);
          break;
        case GEGL_RENDERER_THREAD:
          mrg_queue_draw (o->mrg, NULL);
          g_usleep (40000);
          break;
      }
      o->renderer_state = 0;
      break;
  }
  return TRUE;
}

static int has_quit = 0;
static gpointer renderer_thread (gpointer data)
{
  while (!has_quit)
  {
    renderer_task (data);
  }
  return 0;
}



int mrg_ui_main (int argc, char **argv, char **ops)
{
  Mrg *mrg = mrg_new (1024, 768, NULL);
  const char *renderer_env = g_getenv ("GEGL_RENDERER");

  State o = {NULL,};

  if (renderer_env)
  {
    if (!strcmp (renderer_env, "blit")) renderer = GEGL_RENDERER_BLIT;
    if (!strcmp (renderer_env, "blit-mipmap")) renderer = GEGL_RENDERER_BLIT_MIPMAP;
    if (!strcmp (renderer_env, "mipmap")) renderer = GEGL_RENDERER_BLIT_MIPMAP;
    if (!strcmp (renderer_env, "thread")) renderer = GEGL_RENDERER_THREAD;
    if (!strcmp (renderer_env, "idle")) renderer = GEGL_RENDERER_IDLE;
  }

  mrg_set_title (mrg, "GEGL");
/* we want to see the speed gotten if the fastest babl conversions we have were more accurate */
  //g_setenv ("BABL_TOLERANCE", "0.1", TRUE);

  o.ops = ops;

  gegl_init (&argc, &argv);
  o.gegl            = gegl_node_new (); // so that we have an object to unref
  o.mrg             = mrg;
  o.scale           = 1.0;
  o.render_quality  = 1.0;
  o.preview_quality = 2.0;
  o.slide_pause     = 5.0;
  o.slide_enabled   = 0;

  if (access (argv[1], F_OK) != -1)
    o.path = realpath (argv[1], NULL);
  else
    {
      printf ("usage: %s <full-path-to-image>\n", argv[0]);
      return -1;
    }

  load_path (&o);
  mrg_set_ui (mrg, gegl_ui, &o);
  hack_state = &o;
  on_viewer_motion (NULL, &o, NULL);


  switch (renderer)
  {
    case GEGL_RENDERER_THREAD:
      o.thread = g_thread_new ("renderer", renderer_thread, &o);
      break;
    case GEGL_RENDERER_IDLE:
      g_idle_add (renderer_task, &o);
      break;
    case GEGL_RENDERER_BLIT:
    case GEGL_RENDERER_BLIT_MIPMAP:
      break;
  }

  if (o.ops)
  {
    o.active = gegl_node_get_producer (o.sink, "input", NULL);
    o.show_graph = 1;
  }
  else
  {
    o.active = gegl_node_get_producer (o.sink, "input", NULL);
  }

  mrg_main (mrg);
  has_quit = 1;
  if (renderer == GEGL_RENDERER_THREAD)
    g_thread_join (o.thread);


  g_clear_object (&o.gegl);
  g_clear_object (&o.processor);
  g_clear_object (&o.processor_buffer);
  g_clear_object (&o.buffer);
  gegl_exit ();

  end_audio ();
  return 0;
}

static int hide_controls_cb (Mrg *mrg, void *data)
{
  State *o = data;
  o->controls_timeout = 0;
  o->show_controls = 0;
  mrg_queue_draw (o->mrg, NULL);
  return 0;
}

static void on_viewer_motion (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  {
    if (!o->show_controls)
    {
      o->show_controls = 1;
      mrg_queue_draw (o->mrg, NULL);
    }
    if (o->controls_timeout)
    {
      mrg_remove_idle (o->mrg, o->controls_timeout);
      o->controls_timeout = 0;
    }
    o->controls_timeout = mrg_add_timeout (o->mrg, 1000, hide_controls_cb, o);
  }
}

static int node_select_hack = 0;
static void node_press (MrgEvent *e, void *data1, void *data2)
{
  State *o = data2;
  GeglNode *new_active = data1;

  o->active = new_active;
  mrg_event_stop_propagate (e);
  node_select_hack = 1;

  mrg_queue_draw (e->mrg, NULL);
}

static void on_pan_drag (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;

  if (e->type == MRG_DRAG_RELEASE && node_select_hack == 0)
  {
    float x = (e->x + o->u) / o->scale;
    float y = (e->y + o->v) / o->scale;
    GeglNode *picked = NULL;
    picked = gegl_node_detect (o->sink, x, y);
    if (picked)
    {
      const char *picked_op = gegl_node_get_operation (picked);
      if (g_str_equal (picked_op, "gegl:png-load")||
          g_str_equal (picked_op, "gegl:jpg-load")||
          g_str_equal (picked_op, "gegl:tiff-load"))
      {
        GeglNode *parent = gegl_node_get_parent (picked);
        if (g_str_equal (gegl_node_get_operation (parent), "gegl:load"))
          picked = parent;
      }

      o->active = picked;
    }
  } else if (e->type == MRG_DRAG_MOTION)
  {
    o->u -= (e->delta_x );
    o->v -= (e->delta_y );
    mrg_queue_draw (e->mrg, NULL);
    mrg_event_stop_propagate (e);
  }
  node_select_hack = 0;
  drag_preview (e);
}

static GeglNode *add_output (State *o, GeglNode *active, const char *optype);
static GeglNode *add_aux (State *o, GeglNode *active, const char *optype);

GeglPath *path = NULL;

static void on_paint_drag (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  float x = (e->x + o->u) / o->scale;
  float y = (e->y + o->v) / o->scale;

  switch (e->type)
  {
    default: break;
    case MRG_DRAG_PRESS:
      o->active = add_output (o, o->active, "gegl:over");
      o->active = add_aux (o, o->active, "gegl:path");

      path = gegl_path_new ();
      gegl_path_append (path, 'M', x, y);
      gegl_node_set (o->active, "d", path, "stroke", gegl_color_new("blue"),
  "stroke-width", 42.0, "stroke-hardness", 0.000000001, "fill-opacity", 0, NULL);
      break;
    case MRG_DRAG_MOTION:
      gegl_path_append (path, 'L', x, y);
      break;
    case MRG_DRAG_RELEASE:
      o->active = gegl_node_get_consumer_no (o->active, "output", NULL, 0);
      break;
  }
  mrg_queue_draw (e->mrg, NULL);
  mrg_event_stop_propagate (e);
  //drag_preview (e);
}


static void on_move_drag (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  switch (e->type)
  {
    default: break;
    case MRG_DRAG_PRESS:
{
    float x = (e->x + o->u) / o->scale;
    float y = (e->y + o->v) / o->scale;
    GeglNode *picked = NULL;
    picked = gegl_node_detect (o->sink, x, y);
    if (picked)
    {
      const char *picked_op = gegl_node_get_operation (picked);
      if (g_str_equal (picked_op, "gegl:png-load")||
          g_str_equal (picked_op, "gegl:jpg-load")||
          g_str_equal (picked_op, "gegl:tiff-load"))
      {
        GeglNode *parent = gegl_node_get_parent (picked);
        if (g_str_equal (gegl_node_get_operation (parent), "gegl:load"))
          picked = parent;
      }

      o->active = picked;
    }
}

      if (g_str_equal (gegl_node_get_operation (o->active), "gegl:translate"))
      {
        // no changing of active
      }
      else
      {
        GeglNode *iter = o->active;
        GeglNode *last = o->active;
        while (iter)
        {
          const gchar *input_pad = NULL;
          GeglNode *consumer = gegl_node_get_consumer_no (iter, "output", &input_pad, 0);
          last = iter;
          if (consumer && g_str_equal (input_pad, "input"))
            iter = consumer;
          else
            iter = NULL;
        }
        if (g_str_equal (gegl_node_get_operation (last), "gegl:translate"))
        {
          o->active = last;
        }
        else
        {
          o->active = add_output (o, last, "gegl:translate");
        }
      }
      break;
    case MRG_DRAG_MOTION:
      {
        gdouble x, y;
        gegl_node_get (o->active, "x", &x, "y", &y, NULL);
        x += e->delta_x / o->scale;
        y += e->delta_y / o->scale;
        gegl_node_set (o->active, "x", x, "y", y, NULL);
      }
      break;
    case MRG_DRAG_RELEASE:
      break;
  }
  mrg_queue_draw (e->mrg, NULL);
  mrg_event_stop_propagate (e);
}

#if 0

static void prop_double_drag_cb (MrgEvent *e, void *data1, void *data2)
{
  GeglNode *node = data1;
  GParamSpec *pspec = data2;
  GeglParamSpecDouble *gspec = data2;
  gdouble value = 0.0;
  float range = gspec->ui_maximum - gspec->ui_minimum;

  value = e->x / mrg_width (e->mrg);
  value = value * range + gspec->ui_minimum;
  gegl_node_set (node, pspec->name, value, NULL);

  drag_preview (e);
  mrg_event_stop_propagate (e);

  mrg_queue_draw (e->mrg, NULL);
}

static void prop_int_drag_cb (MrgEvent *e, void *data1, void *data2)
{
  GeglNode *node = data1;
  GParamSpec *pspec = data2;
  GeglParamSpecInt *gspec = data2;
  gint value = 0.0;
  float range = gspec->ui_maximum - gspec->ui_minimum;

  value = e->x / mrg_width (e->mrg) * range + gspec->ui_minimum;
  gegl_node_set (node, pspec->name, value, NULL);

  drag_preview (e);
  mrg_event_stop_propagate (e);

  mrg_queue_draw (e->mrg, NULL);
}
#endif


static gchar *edited_prop = NULL;

static void update_prop (const char *new_string, void *node_p)
{
  GeglNode *node = node_p;
  gegl_node_set (node, edited_prop, new_string, NULL);
}


static void update_prop_double (const char *new_string, void *node_p)
{
  GeglNode *node = node_p;
  gdouble value = g_strtod (new_string, NULL);
  gegl_node_set (node, edited_prop, value, NULL);
}

static void update_prop_int (const char *new_string, void *node_p)
{
  GeglNode *node = node_p;
  gint value = g_strtod (new_string, NULL);
  gegl_node_set (node, edited_prop, value, NULL);
}


static void prop_toggle_boolean (MrgEvent *e, void *data1, void *data2)
{
  GeglNode *node = data1;
  const char *propname = data2;
  gboolean value;
  gegl_node_get (node, propname, &value, NULL);
  value = value ? FALSE : TRUE;
  gegl_node_set (node, propname, value, NULL);

}


static void set_edited_prop (MrgEvent *e, void *data1, void *data2)
{
  if (edited_prop)
    g_free (edited_prop);
  edited_prop = g_strdup (data2);
  mrg_event_stop_propagate (e);

  mrg_set_cursor_pos (e->mrg, 0);
  mrg_queue_draw (e->mrg, NULL);
}

static void unset_edited_prop (MrgEvent *e, void *data1, void *data2)
{
  if (edited_prop)
    g_free (edited_prop);
  edited_prop = NULL;
  mrg_event_stop_propagate (e);

  mrg_queue_draw (e->mrg, NULL);
}


#if 0
static void draw_gegl_generic (State *state, Mrg *mrg, cairo_t *cr, GeglNode *node)
{
  const gchar* op_name = gegl_node_get_operation (node);
  mrg_set_edge_left (mrg, mrg_width (mrg) * 0.1);
  mrg_set_edge_top  (mrg, mrg_height (mrg) * 0.1);
  mrg_set_font_size (mrg, mrg_height (mrg) * 0.07);
  mrg_set_style (mrg, "color:white; background-color: rgba(0,0,0,0.5)");

  cairo_save (cr);
  mrg_printf (mrg, "%s\n", op_name);
  {
    guint n_props;
    GParamSpec **pspecs = gegl_operation_list_properties (op_name, &n_props);

    if (pspecs)
    {
      int tot_pos = 0;
      int pos_no = 0;

      for (gint i = 0; i < n_props; i++)
      {
        if (g_type_is_a (pspecs[i]->value_type, G_TYPE_DOUBLE) ||
            g_type_is_a (pspecs[i]->value_type, G_TYPE_INT) ||
            g_type_is_a (pspecs[i]->value_type, G_TYPE_BOOLEAN))
          tot_pos ++;
        else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_STRING))
          tot_pos +=3;
      }

      for (gint i = 0; i < n_props; i++)
      {
        mrg_set_xy (mrg, mrg_em(mrg),
                    mrg_height (mrg) - mrg_em (mrg) * ((tot_pos-pos_no)));

        if (g_type_is_a (pspecs[i]->value_type, G_TYPE_DOUBLE))
        {
          float xpos;
          GeglParamSpecDouble *geglspec = (void*)pspecs[i];
          gdouble value;
          gegl_node_get (node, pspecs[i]->name, &value, NULL);

          cairo_rectangle (cr, 0,
             mrg_height (mrg) - mrg_em (mrg) * ((tot_pos-pos_no+1)),
             mrg_width (mrg), mrg_em(mrg));
          cairo_set_source_rgba (cr, 0,0,0, 0.5);

          mrg_listen (mrg, MRG_DRAG, prop_double_drag_cb, node,(void*)pspecs[i]);

          cairo_fill (cr);
          xpos = (value - geglspec->ui_minimum) / (geglspec->ui_maximum - geglspec->ui_minimum);
          cairo_rectangle (cr, xpos * mrg_width(mrg) - mrg_em(mrg)/4,
              mrg_height (mrg) - mrg_em (mrg) * ((tot_pos-pos_no+1)),
              mrg_em(mrg)/2, mrg_em(mrg));
          cairo_set_source_rgba (cr, 1,1,1, 0.5);
          cairo_fill (cr);

          mrg_printf (mrg, "%s:%f\n", pspecs[i]->name, value);
          pos_no ++;
        }
        else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_INT))
        {
          float xpos;
          GeglParamSpecInt *geglspec = (void*)pspecs[i];
          gint value;
          gegl_node_get (node, pspecs[i]->name, &value, NULL);

          cairo_rectangle (cr, 0,
             mrg_height (mrg) - mrg_em (mrg) * ((tot_pos-pos_no+1)),
             mrg_width (mrg), mrg_em(mrg));
          cairo_set_source_rgba (cr, 0,0,0, 0.5);

          mrg_listen (mrg, MRG_DRAG, prop_int_drag_cb, node,(void*)pspecs[i]);

          cairo_fill (cr);
          xpos = (value - geglspec->ui_minimum) / (1.0 + geglspec->ui_maximum - geglspec->ui_minimum);
          cairo_rectangle (cr, xpos * mrg_width(mrg) - mrg_em(mrg)/4,
              mrg_height (mrg) - mrg_em (mrg) * ((tot_pos-pos_no+1)),
              mrg_em(mrg)/2, mrg_em(mrg));
          cairo_set_source_rgba (cr, 1,1,1, 0.5);
          cairo_fill (cr);

          mrg_printf (mrg, "%s:%i\n", pspecs[i]->name, value);
          pos_no ++;
        }
        else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_STRING))
        {
          char *value = NULL;
          gegl_node_get (node, pspecs[i]->name, &value, NULL);
          pos_no +=3;

          if (edited_prop && !strcmp (edited_prop, pspecs[i]->name))
          {
            mrg_printf (mrg, "%s:", pspecs[i]->name);
            mrg_text_listen (mrg, MRG_CLICK, unset_edited_prop, node, (void*)pspecs[i]->name);
            mrg_edit_start (mrg, update_prop, node);
            mrg_printf (mrg, "%s\n", value);
            mrg_edit_end (mrg);
            mrg_text_listen_done (mrg);
          }
          else
          {
            mrg_text_listen (mrg, MRG_CLICK, set_edited_prop, node, (void*)pspecs[i]->name);
            mrg_printf (mrg, "%s:%s\n", pspecs[i]->name, value);
            mrg_text_listen_done (mrg);
          }

          if (value)
            g_free (value);
        }
        else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_BOOLEAN))
        {
          gboolean value = FALSE;
          gegl_node_get (node, pspecs[i]->name, &value, NULL);
          pos_no ++;
          mrg_printf (mrg, "%s:%i\n", pspecs[i]->name, value);
        }
      }
      g_free (pspecs);
    }
  }

  cairo_restore (cr);
  mrg_set_style (mrg, "color:yellow; background-color: transparent");
}
#endif
#if 0

static void crop_drag_ul (MrgEvent *e, void *data1, void *data2)
{
  GeglNode *node = data1;
  double x,y,width,height;
  double x0, y0, x1, y1;
  gegl_node_get (node, "x", &x, "y", &y, "width", &width, "height", &height, NULL);
  x0 = x; y0 = y; x1 = x0 + width; y1 = y + height;

  if (e->type == MRG_DRAG_MOTION)
  {
    x0 += e->delta_x;
    y0 += e->delta_y;

    x=x0;
    y=y0;
    width = x1 - x0;
    height = y1 - y0;
    gegl_node_set (node, "x", x, "y", y, "width", width, "height", height, NULL);

    mrg_queue_draw (e->mrg, NULL);
  }

  drag_preview (e);
}

static void crop_drag_lr (MrgEvent *e, void *data1, void *data2)
{
  GeglNode *node = data1;
  double x,y,width,height;
  double x0, y0, x1, y1;
  gegl_node_get (node, "x", &x, "y", &y, "width", &width, "height", &height, NULL);
  x0 = x; y0 = y; x1 = x0 + width; y1 = y + height;

  if (e->type == MRG_DRAG_MOTION)
  {
    x1 += e->delta_x;
    y1 += e->delta_y;

    x=x0;
    y=y0;
    width = x1 - x0;
    height = y1 - y0;
    gegl_node_set (node, "x", x, "y", y, "width", width, "height", height, NULL);

    mrg_queue_draw (e->mrg, NULL);
  }

  drag_preview (e);
}

static void crop_drag_rotate (MrgEvent *e, void *data1, void *data2)
{
  State *o = hack_state;
  double degrees;
  gegl_node_get (o->rotate, "degrees", &degrees, NULL);

  if (e->type == MRG_DRAG_MOTION)
  {
    degrees += e->delta_x / 100.0;

    gegl_node_set (o->rotate, "degrees", degrees, NULL);

    mrg_queue_draw (e->mrg, NULL);
  }

  drag_preview (e);
}

static void draw_gegl_crop (State *o, Mrg *mrg, cairo_t *cr, GeglNode *node)
{
  const gchar* op_name = gegl_node_get_operation (node);
  float dim = mrg_height (mrg) * 0.1 / o->scale;
  double x,y,width,height;
  double x0, y0, x1, y1;

  mrg_set_edge_left (mrg, mrg_width (mrg) * 0.1);
  mrg_set_edge_top  (mrg, mrg_height (mrg) * 0.1);
  mrg_set_font_size (mrg, mrg_height (mrg) * 0.07);
  mrg_set_style (mrg, "color:white; background-color: transparent");

  cairo_save (cr);
  mrg_printf (mrg, "%s\n", op_name);

  cairo_translate (cr, -o->u, -o->v);
  cairo_scale (cr, o->scale, o->scale);

  gegl_node_get (node, "x", &x, "y", &y, "width", &width, "height", &height, NULL);
  x0 = x; y0 = y; x1 = x0 + width; y1 = y + height;

  cairo_rectangle (cr, x0, y0, dim, dim);
  mrg_listen (mrg, MRG_DRAG, crop_drag_ul, node, NULL);
  contrasty_stroke (cr);

  cairo_rectangle (cr, x1-dim, y1-dim, dim, dim);
  mrg_listen (mrg, MRG_DRAG, crop_drag_lr, node, NULL);
  contrasty_stroke (cr);

  cairo_rectangle (cr, x0+dim, y0+dim, width-dim-dim, height-dim-dim);
  mrg_listen (mrg, MRG_DRAG, crop_drag_rotate, node, NULL);
  cairo_new_path (cr);

  cairo_restore (cr);
  mrg_set_style (mrg, "color:yellow; background-color: transparent");
}
#endif
#if DEBUG_OP_LIST

static void node_remove (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *node = o->active;
  GeglNode *new_active;
  GeglNode *next, *prev;

  const gchar *consumer_name = NULL;

  prev = gegl_node_get_producer (node, "input", NULL);
  next = gegl_node_get_consumer_no (node, "output", &consumer_name, 0);

  if (next && prev)
    gegl_node_connect_to (prev, "output", next, consumer_name);

  gegl_node_remove_child (o->gegl, node);
  new_active  = prev?prev:next;

  o->active = new_active;
  mrg_queue_draw (e->mrg, NULL);
}

static void node_add_aux_below (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref = o->active;
  GeglNode *producer = gegl_node_get_producer (o->active, "aux", NULL);

  if (!gegl_node_has_pad (ref, "aux"))
    return;

  o->active = gegl_node_new_child (o->gegl, "operation", "gegl:nop", NULL);

  if (producer)
  {
    gegl_node_connect_to (producer, "output", o->active, "input");
  }
  gegl_node_connect_to (o->active, "output", ref, "aux");

  o->editing_op_name = 1;
  mrg_set_cursor_pos (e->mrg, 0);
  o->new_opname[0]=0;
  fprintf (stderr, "add aux\n");
  mrg_queue_draw (e->mrg, NULL);
}

static void node_add_below (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref = o->active;
  GeglNode *producer = gegl_node_get_producer (o->active, "input", NULL);
  if (!gegl_node_has_pad (ref, "input"))
    return;

  o->active = gegl_node_new_child (o->gegl, "operation", "gegl:nop", NULL);

  if (producer)
  {
    gegl_node_connect_to (producer, "output", o->active, "input");
  }
  gegl_node_connect_to (o->active, "output", ref, "input");

  o->editing_op_name = 1;
  mrg_set_cursor_pos (e->mrg, 0);
  o->new_opname[0]=0;
  fprintf (stderr, "add input\n");
  mrg_queue_draw (e->mrg, NULL);
}


static GeglNode *add_aux (State *o, GeglNode *active, const char *optype)
{
  GeglNode *ref = active;
  GeglNode *ret = NULL;
  GeglNode *producer;
  if (!gegl_node_has_pad (ref, "aux"))
    return NULL;
  ret = gegl_node_new_child (o->gegl, "operation", optype, NULL);
  producer = gegl_node_get_producer (ref, "aux", NULL);
  if (producer)
  {
    gegl_node_link_many (producer, ret, NULL);
  }
  gegl_node_connect_to (ret, "output", ref, "aux");
  return ret;
}

static GeglNode *add_output (State *o, GeglNode *active, const char *optype)
{
  GeglNode *ref = active;
  GeglNode *ret = NULL;
  const char *consumer_name = NULL;
  GeglNode *consumer = gegl_node_get_consumer_no (ref, "output", &consumer_name, 0);
  if (!gegl_node_has_pad (ref, "output"))
    return NULL;

  if (consumer)
  {
    ret = gegl_node_new_child (o->gegl, "operation", optype, NULL);
    gegl_node_link_many (ref, ret, NULL);
    gegl_node_connect_to (ret, "output", consumer, consumer_name);
  }
  return ret;
}


static void node_add_above (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref = o->active;
  const char *consumer_name = NULL;
  GeglNode *consumer = gegl_node_get_consumer_no (o->active, "output", &consumer_name, 0);
  if (!gegl_node_has_pad (ref, "output"))
    return;

  if (consumer)
  {
  o->active = gegl_node_new_child (o->gegl, "operation", "gegl:nop", NULL);

  gegl_node_link_many (ref,
                       o->active,
                       NULL);

  gegl_node_connect_to (o->active, "output", consumer, consumer_name);

    o->editing_op_name = 1;
    mrg_set_cursor_pos (e->mrg, 0);
    o->new_opname[0]=0;
  }

  mrg_queue_draw (e->mrg, NULL);
}

#define INDENT_STR "   "

static void list_node_props (State *o, GeglNode *node, int indent)
{
  Mrg *mrg = o->mrg;
  guint n_props;
  int no = 0;
  //cairo_t *cr = mrg_cr (mrg);
  float x = mrg_x (mrg) + mrg_em (mrg) * 1;
  float y = mrg_y (mrg);
  const char *op_name = gegl_node_get_operation (node);
  GParamSpec **pspecs = gegl_operation_list_properties (op_name, &n_props);

  x = mrg_em (mrg) * 12;

  if (pspecs)
  {
    for (gint i = 0; i < n_props; i++)
    {
      mrg_start (mrg, ".propval", NULL);//
      mrg_set_style (mrg, "color:white; background-color: rgba(0,0,0,0.75)");
      mrg_set_font_size (mrg, mrg_height (mrg) * 0.022);
      mrg_set_edge_left (mrg, x + no * mrg_em (mrg) * 7);
      mrg_set_xy (mrg, x + no * mrg_em (mrg) * 7, y - mrg_em (mrg) * .5);

      if (g_type_is_a (pspecs[i]->value_type, G_TYPE_DOUBLE))
      {
        //float xpos;
        //GeglParamSpecDouble *geglspec = (void*)pspecs[i];
        gdouble value;
        gegl_node_get (node, pspecs[i]->name, &value, NULL);


        if (edited_prop && !strcmp (edited_prop, pspecs[i]->name))
        {
          mrg_printf (mrg, "%s\n", pspecs[i]->name);
          mrg_text_listen (mrg, MRG_CLICK, unset_edited_prop, node, (void*)pspecs[i]->name);
          mrg_edit_start (mrg, update_prop_double, node);
          mrg_printf (mrg, "%.3f", value);
          mrg_edit_end (mrg);
          mrg_printf (mrg, "\n");
          mrg_text_listen_done (mrg);
        }
        else
        {
          mrg_text_listen (mrg, MRG_CLICK, set_edited_prop, node, (void*)pspecs[i]->name);
          mrg_printf (mrg, "%s\n%.3f\n", pspecs[i]->name, value);
          mrg_text_listen_done (mrg);
        }

        no++;
      }
      else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_INT))
      {
        //float xpos;
        //GeglParamSpecInt *geglspec = (void*)pspecs[i];
        gint value;
        gegl_node_get (node, pspecs[i]->name, &value, NULL);

        //mrg_printf (mrg, "%s\n%i\n", pspecs[i]->name, value);

        if (edited_prop && !strcmp (edited_prop, pspecs[i]->name))
        {
          mrg_printf (mrg, "%s\n", pspecs[i]->name);
          mrg_text_listen (mrg, MRG_CLICK, unset_edited_prop, node, (void*)pspecs[i]->name);
          mrg_edit_start (mrg, update_prop_int, node);
          mrg_printf (mrg, "%i", value);
          mrg_edit_end (mrg);
          mrg_printf (mrg, "\n");
          mrg_text_listen_done (mrg);
        }
        else
        {
          mrg_text_listen (mrg, MRG_CLICK, set_edited_prop, node, (void*)pspecs[i]->name);
          mrg_printf (mrg, "%s\n%i\n", pspecs[i]->name, value);
          mrg_text_listen_done (mrg);
        }
        no++;
      }
      else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_STRING) ||
               g_type_is_a (pspecs[i]->value_type, GEGL_TYPE_PARAM_FILE_PATH))
      {
        char *value = NULL;
        gegl_node_get (node, pspecs[i]->name, &value, NULL);

        if (edited_prop && !strcmp (edited_prop, pspecs[i]->name))
        {
          mrg_printf (mrg, "%s\n", pspecs[i]->name);
          mrg_text_listen (mrg, MRG_CLICK, unset_edited_prop, node, (void*)pspecs[i]->name);
          mrg_edit_start (mrg, update_prop, node);
          mrg_printf (mrg, "%s", value);
          mrg_edit_end (mrg);
          mrg_text_listen_done (mrg);
          mrg_printf (mrg, "\n");
        }
        else
        {
          mrg_text_listen (mrg, MRG_CLICK, set_edited_prop, node, (void*)pspecs[i]->name);
          mrg_printf (mrg, "%s\n%s\n", pspecs[i]->name, value);
          mrg_text_listen_done (mrg);
        }

        if (value)
          g_free (value);
        no++;
      }
      else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_BOOLEAN))
      {
        gboolean value = FALSE;
        gegl_node_get (node, pspecs[i]->name, &value, NULL);

        mrg_text_listen (mrg, MRG_CLICK, prop_toggle_boolean, node, (void*)pspecs[i]->name);
        mrg_printf (mrg, "%s\n%s\n", pspecs[i]->name, value?"true":"false");
        mrg_text_listen_done (mrg);
        no++;
      }
      else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_ENUM))
      {
        GEnumClass *eclass = g_type_class_peek (pspecs[i]->value_type);
        GEnumValue *evalue;
        gint value;

        gegl_node_get (node, pspecs[i]->name, &value, NULL);
        evalue  = g_enum_get_value (eclass, value);
        mrg_printf (mrg, "%s:\n%s\n", pspecs[i]->name, evalue->value_nick);
        no++;
      }
      else
      {
        mrg_printf (mrg, "%s:\n", pspecs[i]->name);
        no++;
      }


      mrg_end (mrg);
    }
    g_free (pspecs);
  }

  mrg_set_xy (mrg, x, y);
}

static void update_string (const char *new_text, void *data)
{
  char *str = data;
  strcpy (str, new_text);
}

static void edit_op (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  o->editing_op_name = 1;
  o->new_opname[0]=0;
  mrg_set_cursor_pos (event->mrg, 0);
}


static void node_up (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref;
  if (o->active == NULL)
    return;
  ref = gegl_node_get_consumer_no (o->active, "output", NULL, 0);
  if (ref && ref != o->sink)
    o->active = ref;
  mrg_queue_draw (event->mrg, NULL);
}

static void node_down (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref;
  if (o->active == NULL)
    return;
  ref = gegl_node_get_producer (o->active, "input", NULL);
  if (ref && ref != o->source)
    o->active = ref;
  mrg_queue_draw (event->mrg, NULL);
}

static void node_right (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref;
  if (o->active == NULL)
    return;
  ref = gegl_node_get_producer (o->active, "aux", NULL);
  if (ref && ref != o->source)
    o->active = ref;
  mrg_queue_draw (event->mrg, NULL);
}

static void set_op (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;

  {
    if (strchr (o->new_opname, ':'))
    {
      gegl_node_set (o->active, "operation", o->new_opname, NULL);
    }
    else
    {
      char temp[1024];
      g_snprintf (temp, 1023, "gegl:%s", o->new_opname);
      gegl_node_set (o->active, "operation", temp, NULL);

    }
  }

  o->new_opname[0]=0;
  o->editing_op_name=0;
  mrg_event_stop_propagate (event);
  mrg_queue_draw (o->mrg, NULL);
}
static void run_command (MrgEvent *event, void *data1, void *data2);

static void list_ops (State *o, GeglNode *iter, int indent)
{
  Mrg *mrg = o->mrg;
  
  while (iter && iter != o->source)
   {
     char *opname = NULL;


     g_object_get (iter, "operation", &opname, NULL);

     if (iter == o->active)
     {
       mrg_start_with_style (mrg, ".item", NULL, "color:rgb(197,197,197);background-color: rgba(0,0,0,0.5);padding-right:.5em;");

       for (int i = 0; i < indent; i ++)
         mrg_printf (mrg, INDENT_STR);
       mrg_text_listen (mrg, MRG_CLICK, node_add_above, o, o);
       mrg_printf (mrg, "+    ");
       mrg_text_listen_done (mrg);
       mrg_text_listen (mrg, MRG_CLICK, run_command, o, "remove");
       mrg_printf (mrg, " X ");
       mrg_text_listen_done (mrg);

       mrg_end (mrg);
       mrg_printf (mrg, "\n");
       mrg_start_with_style (mrg, ".item", NULL, "color:white;background-color: rgba(0,0,0,0.5);padding-right:.5em;");
     }
     else
     {
       mrg_start_with_style (mrg, ".item", NULL, "color:rgb(197,197,197);background-color: rgba(0,0,0,0.5);padding-right:.5em;");
     }
     for (int i = 0; i < indent; i ++)
       mrg_printf (mrg, INDENT_STR);

     if (iter != o->active)
     {
       mrg_text_listen (mrg, MRG_CLICK, node_press, iter, o);
     }

     if (o->active == iter && o->editing_op_name)
     {
       mrg_edit_start (mrg, update_string, &o->new_opname[0]);
       mrg_printf (mrg, "%s", o->new_opname);
       mrg_edit_end (mrg);
       mrg_add_binding (mrg, "return", NULL, NULL, set_op, o);
     }
     else
     {
       mrg_printf (mrg, "%s", opname);
     }


     if (iter != o->active)
     {
       mrg_text_listen_done (mrg);
     }
     mrg_end (mrg);

     if(0){
       const Babl *format = gegl_operation_get_format (gegl_node_get_gegl_operation (iter), "output");
       if (format)
       {
#if 0
       BablModelFlag flags = babl_get_model_flags (format);
       const Babl *type = babl_format_get_type (format, 0);

       mrg_set_xy (mrg, mrg_em (mrg) * 15, mrg_y (mrg));

       if (flags & BABL_MODEL_FLAG_GRAY)
         mrg_printf (mrg, "Y");
       else if (flags & BABL_MODEL_FLAG_RGB)
         mrg_printf (mrg, "RGB");
       else if (flags & BABL_MODEL_FLAG_CMYK)
         mrg_printf (mrg, "CMYK");

       if ((flags & BABL_MODEL_FLAG_ALPHA) &&
           (flags & BABL_MODEL_FLAG_PREMULTIPLIED))
         mrg_printf (mrg, " AA");
       else if ((flags & BABL_MODEL_FLAG_ALPHA))
         mrg_printf (mrg, " A");
       mrg_printf (mrg, "%s\n", babl_get_name (type));
#endif
       mrg_printf (mrg, " %s", babl_format_get_encoding (format));

       }

     }

     g_free (opname);

     if (iter == o->active)
       list_node_props (o, iter, indent + 1);

     mrg_printf (mrg, "\n");

     if (iter == o->active && gegl_node_has_pad (iter, "aux"))
     {
       mrg_start_with_style (mrg, ".item", NULL, "color:rgb(197,197,197);background-color: rgba(0,0,0,0.5);");
       for (int i = 0; i < indent + 1; i ++)
         mrg_printf (mrg, INDENT_STR);
       mrg_text_listen (mrg, MRG_CLICK, node_add_aux_below, o, o);
       mrg_printf (mrg, "+\n");
       mrg_text_listen_done (mrg);
       mrg_end (mrg);
     }

     if (gegl_node_get_producer (iter, "aux", NULL))
     {

     {
       GeglNode *producer = gegl_node_get_producer (iter, "aux", NULL);
       GeglNode *producers_consumer;

       producers_consumer = gegl_node_get_consumer_no (producer, "output", NULL, 0);

       if (producers_consumer == iter)
         {
           list_ops (o, gegl_node_get_producer (iter, "aux", NULL), indent + 1);
         }
        else
        {
          if (producer)
          {
            for (int i = 0; i < indent + 1; i ++)
              mrg_printf (mrg, INDENT_STR);
            mrg_printf (mrg, "[clone]\n");
          }
        }
     }




     }
     if (iter == o->active && gegl_node_has_pad (iter, "input"))
     {
       mrg_start_with_style (mrg, ".item", NULL, "color:rgb(197,197,197);background-color: rgba(0,0,0,0.5);");
       for (int i = 0; i < indent; i ++)
         mrg_printf (mrg, INDENT_STR);
       mrg_text_listen (mrg, MRG_CLICK, node_add_below, o, o);
       mrg_printf (mrg, "+  \n");
       mrg_text_listen_done (mrg);
       mrg_end (mrg);
     }

     {
       GeglNode *producer = gegl_node_get_producer (iter, "input", NULL);
       GeglNode *producers_consumer;

       producers_consumer = gegl_node_get_consumer_no (producer, "output", NULL, 0);

       if (producers_consumer == iter)
         iter = producer;
        else
        {
          if (producer)
          {
            for (int i = 0; i < indent; i ++)
              mrg_printf (mrg, INDENT_STR);
            mrg_printf (mrg, "[clone]\n");
          }
          iter = NULL;
        }
     }
   }
}

static void ui_debug_op_chain (State *o)
{
  Mrg *mrg = o->mrg;
  GeglNode *iter;
  mrg_set_edge_top  (mrg, mrg_height (mrg) * 0.2);
  mrg_set_font_size (mrg, mrg_height (mrg) * 0.03);
  mrg_set_style (mrg, "color:white; background-color: rgba(0,0,0,0.5)");

  iter = o->sink;

  /* skip nop-node */
  iter = gegl_node_get_producer (iter, "input", NULL);

  list_ops (o, iter, 1);
}
#endif

static void dir_pgdn_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  o->u -= mrg_width (o->mrg) * 0.6;
  mrg_queue_draw (o->mrg, NULL);
  mrg_event_stop_propagate (event);
}

static void dir_pgup_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  o->u += mrg_width (o->mrg) * 0.6;
  mrg_queue_draw (o->mrg, NULL);
  mrg_event_stop_propagate (event);
}

static void entry_pressed (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  free (o->path);
  o->path = strdup (data2);
  load_path (o);
  mrg_queue_draw (event->mrg, NULL);
}

static void ui_dir_viewer (State *o)
{
  Mrg *mrg = o->mrg;
  cairo_t *cr = mrg_cr (mrg);
  GList *iter;
  float x = 0;
  float y = 0;
  float dim = mrg_height (mrg) * 0.25;

  cairo_rectangle (cr, 0,0, mrg_width(mrg), mrg_height(mrg));
  mrg_listen (mrg, MRG_MOTION, on_viewer_motion, o, NULL);
  cairo_new_path (cr);

  mrg_set_edge_right (mrg, 4095);
  cairo_save (cr);
  cairo_translate (cr, o->u, 0);//o->v);

  for (iter = o->paths; iter; iter=iter->next)
  {
      int w, h;
      gchar *path = iter->data;
      char *lastslash = strrchr (path, '/');

      gchar *p2 = suffix_path (path);

      gchar *thumbpath = get_thumb_path (p2);
      free (p2);
      if (access (thumbpath, F_OK) == -1)
      {
        g_free (thumbpath);
        thumbpath = get_thumb_path (path);
      }

      if (
         access (thumbpath, F_OK) != -1 && //XXX: query image should suffice
         mrg_query_image (mrg, thumbpath, &w, &h))
      {
        float wdim = dim;
        float hdim = dim;
        if (w > h)
          hdim = dim / (1.0 * w / h);
        else
          wdim = dim * (1.0 * w / h);

        mrg_image (mrg, x + (dim-wdim)/2, y + (dim-hdim)/2, wdim, hdim, 1.0, thumbpath, NULL, NULL);
      }
      g_free (thumbpath);

      mrg_set_xy (mrg, x, y + dim - mrg_em(mrg));
      mrg_printf (mrg, "%s\n", lastslash+1);
      cairo_new_path (mrg_cr(mrg));
      cairo_rectangle (mrg_cr(mrg), x, y, dim, dim);
      mrg_listen_full (mrg, MRG_CLICK, entry_pressed, o, path, NULL, NULL);
      cairo_new_path (mrg_cr(mrg));

      y += dim;
      if (y+dim > mrg_height (mrg))
      {
        y = 0;
        x += dim;
      }
  }
  cairo_restore (cr);

  cairo_scale (cr, mrg_width(mrg), mrg_height(mrg));
  cairo_new_path (cr);
  cairo_move_to (cr, 0.2, 0.8);
  cairo_line_to (cr, 0.2, 1.0);
  cairo_line_to (cr, 0.0, 0.9);
  cairo_close_path (cr);
  if (o->show_controls)
    contrasty_stroke (cr);
  else
    cairo_new_path (cr);
  cairo_rectangle (cr, 0.0, 0.8, 0.2, 0.2);
  mrg_listen (mrg, MRG_PRESS, dir_pgup_cb, o, NULL);

  cairo_new_path (cr);

  cairo_move_to (cr, 0.8, 0.8);
  cairo_line_to (cr, 0.8, 1.0);
  cairo_line_to (cr, 1.0, 0.9);
  cairo_close_path (cr);

  if (o->show_controls)
    contrasty_stroke (cr);
  else
    cairo_new_path (cr);
  cairo_rectangle (cr, 0.8, 0.8, 0.2, 0.2);
  mrg_listen (mrg, MRG_PRESS, dir_pgdn_cb, o, NULL);
  cairo_new_path (cr);

  mrg_add_binding (mrg, "left", NULL, NULL, dir_pgup_cb, o);
  mrg_add_binding (mrg, "right", NULL, NULL, dir_pgdn_cb, o);
}

static int slide_cb (Mrg *mrg, void *data)
{
  State *o = data;
  o->slide_timeout = 0;
  go_next (data);
  return 0;
}

static void scroll_cb (MrgEvent *event, void *data1, void *data2);

static void ui_viewer (State *o)
{
  Mrg *mrg = o->mrg;
  cairo_t *cr = mrg_cr (mrg);
  cairo_rectangle (cr, 0,0, mrg_width(mrg), mrg_height(mrg));
#if 0
  mrg_listen (mrg, MRG_DRAG, on_pan_drag, o, NULL);
  mrg_listen (mrg, MRG_MOTION, on_viewer_motion, o, NULL);
  mrg_listen (mrg, MRG_SCROLL, scroll_cb, o, NULL);
#endif

  cairo_scale (cr, mrg_width(mrg), mrg_height(mrg));
  cairo_new_path (cr);
  cairo_rectangle (cr, 0.05, 0.05, 0.05, 0.05);
  cairo_rectangle (cr, 0.15, 0.05, 0.05, 0.05);
  cairo_rectangle (cr, 0.05, 0.15, 0.05, 0.05);
  cairo_rectangle (cr, 0.15, 0.15, 0.05, 0.05);
  if (o->show_controls)
    contrasty_stroke (cr);
  else
    cairo_new_path (cr);
  cairo_rectangle (cr, 0.0, 0.0, 0.2, 0.2);
  mrg_listen (mrg, MRG_PRESS, go_parent_cb, o, NULL);


  cairo_new_path (cr);
  cairo_move_to (cr, 0.2, 0.8);
  cairo_line_to (cr, 0.2, 1.0);
  cairo_line_to (cr, 0.0, 0.9);
  cairo_close_path (cr);
  if (o->show_controls)
    contrasty_stroke (cr);
  else
    cairo_new_path (cr);
  cairo_rectangle (cr, 0.0, 0.8, 0.2, 0.2);
  mrg_listen (mrg, MRG_PRESS, go_prev_cb, o, NULL);
  cairo_new_path (cr);

  cairo_move_to (cr, 0.8, 0.8);
  cairo_line_to (cr, 0.8, 1.0);
  cairo_line_to (cr, 1.0, 0.9);
  cairo_close_path (cr);

  if (o->show_controls)
    contrasty_stroke (cr);
  else
    cairo_new_path (cr);
  cairo_rectangle (cr, 0.8, 0.8, 0.2, 0.2);
  mrg_listen (mrg, MRG_PRESS, go_next_cb, o, NULL);
  cairo_new_path (cr);

  cairo_arc (cr, 0.9, 0.1, 0.1, 0.0, G_PI * 2);

  if (o->show_controls)
    contrasty_stroke (cr);
  else
    cairo_new_path (cr);
  cairo_rectangle (cr, 0.8, 0.0, 0.2, 0.2);
  mrg_listen (mrg, MRG_PRESS, toggle_actions_cb, o, NULL);
  cairo_new_path (cr);


  mrg_add_binding (mrg, "left", NULL, NULL,  pan_left_cb, o);
  mrg_add_binding (mrg, "right", NULL, NULL, pan_right_cb, o);
  mrg_add_binding (mrg, "up", NULL, NULL,    pan_up_cb, o);
  mrg_add_binding (mrg, "down", NULL, NULL,  pan_down_cb, o);

  if (!edited_prop && !o->editing_op_name)
  {
    mrg_add_binding (mrg, "control-m", NULL, NULL,     zoom_fit_cb, o);
    mrg_add_binding (mrg, "control-delete", NULL, NULL,     discard_cb, o);
    mrg_add_binding (mrg, "space", NULL, NULL,     go_next_cb , o);
    mrg_add_binding (mrg, "n", NULL, NULL,         go_next_cb, o);
    mrg_add_binding (mrg, "p", NULL, NULL,         go_prev_cb, o);
  }

  if (o->slide_enabled && o->slide_timeout == 0)
  {
    o->slide_timeout =
       mrg_add_timeout (o->mrg, o->slide_pause * 1000, slide_cb, o);
  }
}

static void toggle_show_controls_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  o->show_controls = !o->show_controls;
  mrg_queue_draw (o->mrg, NULL);
}

static void toggle_slideshow_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  o->slide_enabled = !o->slide_enabled;
  if (o->slide_timeout)
      mrg_remove_idle (o->mrg, o->slide_timeout);
  o->slide_timeout = 0;
  mrg_queue_draw (o->mrg, NULL);
}

static int deferred_redraw_action (Mrg *mrg, void *data)
{
  mrg_queue_draw (mrg, NULL);
  return 0;
}

static void deferred_redraw (Mrg *mrg, MrgRectangle *rect)
{
  MrgRectangle r; /* copy in call stack of dereference rectangle if pointer
                     is passed in */
  if (rect)
    r = *rect;
  mrg_add_timeout (mrg, 0, deferred_redraw_action, rect?&r:NULL);
}

enum {
  TOOL_PAN=0,
  TOOL_PICK,
  TOOL_PAINT,
  TOOL_MOVE,
};

static int tool = TOOL_PAN;

static void ui_canvas_handling (Mrg *mrg, State *o)
{
  switch (tool)
  {
    case TOOL_PAN:
      cairo_rectangle (mrg_cr (mrg), 0,0, mrg_width(mrg), mrg_height(mrg));
      mrg_listen (mrg, MRG_DRAG, on_pan_drag, o, NULL);
      mrg_listen (mrg, MRG_MOTION, on_viewer_motion, o, NULL);
      mrg_listen (mrg, MRG_SCROLL, scroll_cb, o, NULL);
      cairo_new_path (mrg_cr (mrg));
      break;
    case TOOL_PAINT:
      cairo_rectangle (mrg_cr (mrg), 0,0, mrg_width(mrg), mrg_height(mrg));
      mrg_listen (mrg, MRG_DRAG, on_paint_drag, o, NULL);
      mrg_listen (mrg, MRG_SCROLL, scroll_cb, o, NULL);
      cairo_new_path (mrg_cr (mrg));
      break;
    case TOOL_MOVE:
      cairo_rectangle (mrg_cr (mrg), 0,0, mrg_width(mrg), mrg_height(mrg));
      mrg_listen (mrg, MRG_DRAG, on_move_drag, o, NULL);
      mrg_listen (mrg, MRG_SCROLL, scroll_cb, o, NULL);
      cairo_new_path (mrg_cr (mrg));
      break;
  }
}


static void node_remove (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *node = o->active;
  GeglNode *new_active;
  GeglNode *next, *prev;

  const gchar *consumer_name = NULL;

  prev = gegl_node_get_producer (node, "input", NULL);
  next = gegl_node_get_consumer_no (node, "output", &consumer_name, 0);

  if (next && prev)
    gegl_node_connect_to (prev, "output", next, consumer_name);

  gegl_node_remove_child (o->gegl, node);
  new_active  = prev?prev:next;

  o->active = new_active;
  mrg_queue_draw (e->mrg, NULL);
}

static void node_add_aux_below (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref = o->active;
  GeglNode *producer = gegl_node_get_producer (o->active, "aux", NULL);

  if (!gegl_node_has_pad (ref, "aux"))
    return;

  o->active = gegl_node_new_child (o->gegl, "operation", "gegl:nop", NULL);

  if (producer)
  {
    gegl_node_connect_to (producer, "output", o->active, "input");
  }
  gegl_node_connect_to (o->active, "output", ref, "aux");

  o->editing_op_name = 1;
  mrg_set_cursor_pos (e->mrg, 0);
  o->new_opname[0]=0;
  fprintf (stderr, "add aux\n");
  mrg_queue_draw (e->mrg, NULL);
}


static void node_add_below (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref = o->active;
  GeglNode *producer = gegl_node_get_producer (o->active, "input", NULL);
  if (!gegl_node_has_pad (ref, "input"))
    return;

  o->active = gegl_node_new_child (o->gegl, "operation", "gegl:nop", NULL);

  if (producer)
  {
    gegl_node_connect_to (producer, "output", o->active, "input");
  }
  gegl_node_connect_to (o->active, "output", ref, "input");

  o->editing_op_name = 1;
  mrg_set_cursor_pos (e->mrg, 0);
  o->new_opname[0]=0;
  fprintf (stderr, "add input\n");
  mrg_queue_draw (e->mrg, NULL);
}


static GeglNode *add_aux (State *o, GeglNode *active, const char *optype)
{
  GeglNode *ref = active;
  GeglNode *ret = NULL;
  GeglNode *producer;
  if (!gegl_node_has_pad (ref, "aux"))
    return NULL;
  ret = gegl_node_new_child (o->gegl, "operation", optype, NULL);
  producer = gegl_node_get_producer (ref, "aux", NULL);
  if (producer)
  {
    gegl_node_link_many (producer, ret, NULL);
  }
  gegl_node_connect_to (ret, "output", ref, "aux");
  return ret;
}

static GeglNode *add_output (State *o, GeglNode *active, const char *optype)
{
  GeglNode *ref = active;
  GeglNode *ret = NULL;
  const char *consumer_name = NULL;
  GeglNode *consumer = gegl_node_get_consumer_no (ref, "output", &consumer_name, 0);
  if (!gegl_node_has_pad (ref, "output"))
    return NULL;

  if (consumer)
  {
    ret = gegl_node_new_child (o->gegl, "operation", optype, NULL);
    gegl_node_link_many (ref, ret, NULL);
    gegl_node_connect_to (ret, "output", consumer, consumer_name);
  }
  return ret;
}

static void node_add_above (MrgEvent *e, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref = o->active;
  const char *consumer_name = NULL;
  GeglNode *consumer = gegl_node_get_consumer_no (o->active, "output", &consumer_name, 0);
  if (!gegl_node_has_pad (ref, "output"))
    return;

  if (consumer)
  {
  o->active = gegl_node_new_child (o->gegl, "operation", "gegl:nop", NULL);

  gegl_node_link_many (ref,
                       o->active,
                       NULL);

  gegl_node_connect_to (o->active, "output", consumer, consumer_name);

    o->editing_op_name = 1;
    mrg_set_cursor_pos (e->mrg, 0);
    o->new_opname[0]=0;
  }

  mrg_queue_draw (e->mrg, NULL);
}

#define INDENT_STR "   "

static void list_node_props (State *o, GeglNode *node, int indent)
{
  Mrg *mrg = o->mrg;
  guint n_props;
  int no = 0;
  //cairo_t *cr = mrg_cr (mrg);
  float x = mrg_x (mrg) + mrg_em (mrg) * 1;
  float y = mrg_y (mrg);
  const char *op_name = gegl_node_get_operation (node);
  GParamSpec **pspecs = gegl_operation_list_properties (op_name, &n_props);

  x = mrg_em (mrg) * 12;

  if (pspecs)
  {
    for (gint i = 0; i < n_props; i++)
    {
      mrg_start (mrg, ".propval", NULL);//
      mrg_set_style (mrg, "color:white; background-color: rgba(0,0,0,0.75)");
      mrg_set_font_size (mrg, mrg_height (mrg) * 0.022);
      mrg_set_edge_left (mrg, x + no * mrg_em (mrg) * 7);
      mrg_set_xy (mrg, x + no * mrg_em (mrg) * 7, y - mrg_em (mrg) * .5);

      if (g_type_is_a (pspecs[i]->value_type, G_TYPE_DOUBLE))
      {
        //float xpos;
        //GeglParamSpecDouble *geglspec = (void*)pspecs[i];
        gdouble value;
        gegl_node_get (node, pspecs[i]->name, &value, NULL);


        if (edited_prop && !strcmp (edited_prop, pspecs[i]->name))
        {
          mrg_printf (mrg, "%s\n", pspecs[i]->name);
          mrg_text_listen (mrg, MRG_CLICK, unset_edited_prop, node, (void*)pspecs[i]->name);
          mrg_edit_start (mrg, update_prop_double, node);
          mrg_printf (mrg, "%.3f", value);
          mrg_edit_end (mrg);
          mrg_printf (mrg, "\n");
          mrg_text_listen_done (mrg);
        }
        else
        {
          mrg_text_listen (mrg, MRG_CLICK, set_edited_prop, node, (void*)pspecs[i]->name);
          mrg_printf (mrg, "%s\n%.3f\n", pspecs[i]->name, value);
          mrg_text_listen_done (mrg);
        }

        no++;
      }
      else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_INT))
      {
        //float xpos;
        //GeglParamSpecInt *geglspec = (void*)pspecs[i];
        gint value;
        gegl_node_get (node, pspecs[i]->name, &value, NULL);

        //mrg_printf (mrg, "%s\n%i\n", pspecs[i]->name, value);

        if (edited_prop && !strcmp (edited_prop, pspecs[i]->name))
        {
          mrg_printf (mrg, "%s\n", pspecs[i]->name);
          mrg_text_listen (mrg, MRG_CLICK, unset_edited_prop, node, (void*)pspecs[i]->name);
          mrg_edit_start (mrg, update_prop_int, node);
          mrg_printf (mrg, "%i", value);
          mrg_edit_end (mrg);
          mrg_printf (mrg, "\n");
          mrg_text_listen_done (mrg);
        }
        else
        {
          mrg_text_listen (mrg, MRG_CLICK, set_edited_prop, node, (void*)pspecs[i]->name);
          mrg_printf (mrg, "%s\n%i\n", pspecs[i]->name, value);
          mrg_text_listen_done (mrg);
        }
        no++;
      }
      else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_STRING) ||
               g_type_is_a (pspecs[i]->value_type, GEGL_TYPE_PARAM_FILE_PATH))
      {
        char *value = NULL;
        gegl_node_get (node, pspecs[i]->name, &value, NULL);

        if (edited_prop && !strcmp (edited_prop, pspecs[i]->name))
        {
          mrg_printf (mrg, "%s\n", pspecs[i]->name);
          mrg_text_listen (mrg, MRG_CLICK, unset_edited_prop, node, (void*)pspecs[i]->name);
          mrg_edit_start (mrg, update_prop, node);
          mrg_printf (mrg, "%s", value);
          mrg_edit_end (mrg);
          mrg_text_listen_done (mrg);
          mrg_printf (mrg, "\n");
        }
        else
        {
          mrg_text_listen (mrg, MRG_CLICK, set_edited_prop, node, (void*)pspecs[i]->name);
          mrg_printf (mrg, "%s\n%s\n", pspecs[i]->name, value);
          mrg_text_listen_done (mrg);
        }

        if (value)
          g_free (value);
        no++;
      }
      else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_BOOLEAN))
      {
        gboolean value = FALSE;
        gegl_node_get (node, pspecs[i]->name, &value, NULL);

        mrg_text_listen (mrg, MRG_CLICK, prop_toggle_boolean, node, (void*)pspecs[i]->name);
        mrg_printf (mrg, "%s\n%s\n", pspecs[i]->name, value?"true":"false");
        mrg_text_listen_done (mrg);
        no++;
      }
      else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_ENUM))
      {
        GEnumClass *eclass = g_type_class_peek (pspecs[i]->value_type);
        GEnumValue *evalue;
        gint value;

        gegl_node_get (node, pspecs[i]->name, &value, NULL);
        evalue  = g_enum_get_value (eclass, value);
        mrg_printf (mrg, "%s:\n%s\n", pspecs[i]->name, evalue->value_nick);
        no++;
      }
      else
      {
        mrg_printf (mrg, "%s:\n", pspecs[i]->name);
        no++;
      }


      mrg_end (mrg);
    }
    g_free (pspecs);
  }

  mrg_set_xy (mrg, x, y);
}

static void update_string (const char *new_text, void *data)
{
  char *str = data;
  strcpy (str, new_text);
}

#if 0
static void edit_op (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  o->editing_op_name = 1;
  o->new_opname[0]=0;
  mrg_set_cursor_pos (event->mrg, 0);
}
#endif

static void node_up (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref;
  if (o->active == NULL)
    return;
  ref = gegl_node_get_consumer_no (o->active, "output", NULL, 0);
  if (ref && ref != o->sink)
    o->active = ref;
  mrg_queue_draw (event->mrg, NULL);
}

static void node_down (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref;
  if (o->active == NULL)
    return;
  ref = gegl_node_get_producer (o->active, "input", NULL);
  if (ref) // && ref != o->source)
    o->active = ref;
  mrg_queue_draw (event->mrg, NULL);
}

static void node_right (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  GeglNode *ref;
  if (o->active == NULL)
    return;
  ref = gegl_node_get_producer (o->active, "aux", NULL);
  if (ref && ref != o->source)
    o->active = ref;
  mrg_queue_draw (event->mrg, NULL);
}

static void set_op (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;

  {
    if (strchr (o->new_opname, ':'))
    {
      gegl_node_set (o->active, "operation", o->new_opname, NULL);
    }
    else
    {
      char temp[1024];
      g_snprintf (temp, 1023, "gegl:%s", o->new_opname);
      gegl_node_set (o->active, "operation", temp, NULL);

    }
  }

  o->new_opname[0]=0;
  o->editing_op_name=0;
  mrg_event_stop_propagate (event);
  mrg_queue_draw (o->mrg, NULL);
}

static void run_command (MrgEvent *event, void *data1, void *data2);

static void list_ops (State *o, GeglNode *iter, int indent)
{
  Mrg *mrg = o->mrg;
  
  while (iter) // && iter != o->source)
   {
     char *opname = NULL;


     g_object_get (iter, "operation", &opname, NULL);

     if (iter == o->active)
     {
       mrg_start_with_style (mrg, ".item", NULL, "color:rgb(197,197,197);background-color: rgba(0,0,0,0.5);padding-right:.5em;");

       for (int i = 0; i < indent; i ++)
         mrg_printf (mrg, INDENT_STR);
       mrg_text_listen (mrg, MRG_CLICK, node_add_above, o, o);
       mrg_printf (mrg, "+    ");
       mrg_text_listen_done (mrg);
       mrg_text_listen (mrg, MRG_CLICK, run_command, o, "remove");
       mrg_printf (mrg, " X ");
       mrg_text_listen_done (mrg);

       mrg_end (mrg);
       mrg_printf (mrg, "\n");
       mrg_start_with_style (mrg, ".item", NULL, "color:white;background-color: rgba(0,0,0,0.5);padding-right:.5em;");
     }
     else
     {
       mrg_start_with_style (mrg, ".item", NULL, "color:rgb(197,197,197);background-color: rgba(0,0,0,0.5);padding-right:.5em;");
     }
     for (int i = 0; i < indent; i ++)
       mrg_printf (mrg, INDENT_STR);

     if (iter != o->active)
     {
       mrg_text_listen (mrg, MRG_CLICK, node_press, iter, o);
     }

     if (o->active == iter && o->editing_op_name)
     {
       mrg_edit_start (mrg, update_string, &o->new_opname[0]);
       mrg_printf (mrg, "%s", o->new_opname);
       mrg_edit_end (mrg);
       mrg_add_binding (mrg, "return", NULL, NULL, set_op, o);

     }
     else
     {
       mrg_printf (mrg, "%s", opname);
     }


     if (iter != o->active)
     {
       mrg_text_listen_done (mrg);
     }
     mrg_end (mrg);

     if(0){
       const Babl *format = gegl_operation_get_format (gegl_node_get_gegl_operation (iter), "output");
       if (format)
       {
#if 0
       BablModelFlag flags = babl_get_model_flags (format);
       const Babl *type = babl_format_get_type (format, 0);

       mrg_set_xy (mrg, mrg_em (mrg) * 15, mrg_y (mrg));

       if (flags & BABL_MODEL_FLAG_GRAY)
         mrg_printf (mrg, "Y");
       else if (flags & BABL_MODEL_FLAG_RGB)
         mrg_printf (mrg, "RGB");
       else if (flags & BABL_MODEL_FLAG_CMYK)
         mrg_printf (mrg, "CMYK");

       if ((flags & BABL_MODEL_FLAG_ALPHA) &&
           (flags & BABL_MODEL_FLAG_PREMULTIPLIED))
         mrg_printf (mrg, " AA");
       else if ((flags & BABL_MODEL_FLAG_ALPHA))
         mrg_printf (mrg, " A");
       mrg_printf (mrg, "%s\n", babl_get_name (type));
#endif
       mrg_printf (mrg, " %s", babl_format_get_encoding (format));

       }

     }

     g_free (opname);

     if (iter == o->active)
       list_node_props (o, iter, indent + 1);

     mrg_printf (mrg, "\n");

     if (iter == o->active && gegl_node_has_pad (iter, "aux"))
     {
       mrg_start_with_style (mrg, ".item", NULL, "color:rgb(197,197,197);background-color: rgba(0,0,0,0.5);");
       for (int i = 0; i < indent + 1; i ++)
         mrg_printf (mrg, INDENT_STR);
       mrg_text_listen (mrg, MRG_CLICK, node_add_aux_below, o, o);
       mrg_printf (mrg, "+\n");
       mrg_text_listen_done (mrg);
       mrg_end (mrg);
     }

     if (gegl_node_get_producer (iter, "aux", NULL))
     {

     {
       GeglNode *producer = gegl_node_get_producer (iter, "aux", NULL);
       GeglNode *producers_consumer;

       producers_consumer = gegl_node_get_consumer_no (producer, "output", NULL, 0);

       if (producers_consumer == iter)
         {
           list_ops (o, gegl_node_get_producer (iter, "aux", NULL), indent + 1);
         }
        else
        {
          if (producer)
          {
            for (int i = 0; i < indent + 1; i ++)
              mrg_printf (mrg, INDENT_STR);
            mrg_printf (mrg, "[clone]\n");
          }
        }
     }





     }
     if (iter == o->active && gegl_node_has_pad (iter, "input"))
     {
       mrg_start_with_style (mrg, ".item", NULL, "color:rgb(197,197,197);background-color: rgba(0,0,0,0.5);");
       for (int i = 0; i < indent; i ++)
         mrg_printf (mrg, INDENT_STR);
       mrg_text_listen (mrg, MRG_CLICK, node_add_below, o, o);
       mrg_printf (mrg, "+  \n");
       mrg_text_listen_done (mrg);
       mrg_end (mrg);
     }

     {
       GeglNode *producer = gegl_node_get_producer (iter, "input", NULL);
       GeglNode *producers_consumer;

       producers_consumer = gegl_node_get_consumer_no (producer, "output", NULL, 0);

       if (producers_consumer == iter)
         iter = producer;
        else
        {
          if (producer)
          {
            for (int i = 0; i < indent; i ++)
              mrg_printf (mrg, INDENT_STR);
            mrg_printf (mrg, "[clone]\n");
          }
          iter = NULL;
        }
     }
   }
}

static void ui_debug_op_chain (State *o)
{
  Mrg *mrg = o->mrg;
  GeglNode *iter;
  mrg_set_edge_top  (mrg, mrg_height (mrg) * 0.2);
  mrg_set_font_size (mrg, mrg_height (mrg) * 0.03);
  mrg_set_style (mrg, "color:white; background-color: rgba(0,0,0,0.5)");

  iter = o->sink;

  /* skip nop-node */
  iter = gegl_node_get_producer (iter, "input", NULL);

  list_ops (o, iter, 1);
}


static char commandline[1024] = "";

static void update_commandline (const char *new_commandline, void *data)
{
  //State *o = data;
  strcpy (commandline, new_commandline);
}

static void run_command (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  Mrg *mrg = event->mrg;
  const char *commandline = data2;

  if (strchr (commandline, '='))
  {
    GType target_type = 0;
    GParamSpec *pspec = NULL;
    GParamSpec **pspecs = NULL;
    char *key = g_strdup (commandline);
    char *value = strchr (key, '=') + 1;
    unsigned int n_props = 0;
    value[-1]='\0';
    pspecs = gegl_operation_list_properties (gegl_node_get_operation (o->active), &n_props);
    for (int i = 0; i < n_props; i++)
    {
      if (!strcmp (pspecs[i]->name, key))
      {
        target_type = pspecs[i]->value_type;
        pspec = pspecs[i];
        break;
      }
    }

    if (pspec)
    {
      if (g_type_is_a (target_type, G_TYPE_DOUBLE) ||
          g_type_is_a (target_type, G_TYPE_FLOAT)  ||
          g_type_is_a (target_type, G_TYPE_INT)    ||
          g_type_is_a (target_type, G_TYPE_UINT))
      {
         if (g_type_is_a (target_type, G_TYPE_INT))
           gegl_node_set (o->active, key,
           (int)g_strtod (value, NULL), NULL);
         else if (g_type_is_a (target_type, G_TYPE_UINT))
           gegl_node_set (o->active, key,
           (guint)g_strtod (value, NULL), NULL);
         else
           gegl_node_set (o->active, key,
                          g_strtod (value, NULL), NULL);
      }
      else if (g_type_is_a (target_type, G_TYPE_BOOLEAN))
      {
        if (!strcmp (value, "true") || !strcmp (value, "TRUE") ||
            !strcmp (value, "YES") || !strcmp (value, "yes") ||
            !strcmp (value, "y") || !strcmp (value, "Y") ||
            !strcmp (value, "1") || !strcmp (value, "on"))
        {
          gegl_node_set (o->active, key, TRUE, NULL);
        }
          else
        {
          gegl_node_set (o->active, key, FALSE, NULL);
        }
      }
      else if (target_type == GEGL_TYPE_COLOR)
      {
        GeglColor *color = g_object_new (GEGL_TYPE_COLOR,
                                         "string", value, NULL);
        gegl_node_set (o->active, key, color, NULL);
      }
      else if (target_type == GEGL_TYPE_PATH)
      {
        GeglPath *path = gegl_path_new ();
        gegl_path_parse_string (path, value);
        gegl_node_set (o->active, key, path, NULL);
      }
      else if (target_type == G_TYPE_POINTER &&
               GEGL_IS_PARAM_SPEC_FORMAT (pspec))
      {
        const Babl *format = NULL;

        if (value[0] && babl_format_exists (value))
          format = babl_format (value);
#if 0
        else if (error)
        {
          char *error_str = g_strdup_printf (
                                  _("BablFormat \"%s\" does not exist."),
                                  value);
          *error = g_error_new_literal (g_quark_from_static_string ( "gegl"),
                                        0, error_str);
          g_free (error_str);
        }
#endif

        gegl_node_set (o->active, key, format, NULL);
      }
      else if (g_type_is_a (G_PARAM_SPEC_TYPE (pspec),
               GEGL_TYPE_PARAM_FILE_PATH))
      {
        gchar *buf;
        if (g_path_is_absolute (value))
          {
            gegl_node_set (o->active, key, value, NULL);
          }
        else
          {
            gchar *absolute_path;
#if 0
            if (path_root)
              buf = g_strdup_printf ("%s/%s", path_root, value);
            else
#endif
              buf = g_strdup_printf ("./%s", value);
            absolute_path = realpath (buf, NULL);
            g_free (buf);
            if (absolute_path)
              {
                gegl_node_set (o->active, key, absolute_path, NULL);
                free (absolute_path);
              }
            gegl_node_set (o->active, key, value, NULL);
          }
      }
      else if (g_type_is_a (target_type, G_TYPE_STRING))
      {
        gegl_node_set (o->active, key, value, NULL);
      }
      else if (g_type_is_a (target_type, G_TYPE_ENUM))
      {
        GEnumClass *eclass = g_type_class_peek (target_type);
        GEnumValue *evalue = g_enum_get_value_by_nick (eclass,
                                                       value);
        if (evalue)
          {
            gegl_node_set (o->active, key, evalue->value, NULL);
          }
        else
          {
            /* warn, but try to get a valid nick out of the
               old-style value
             * name
             */
            gchar *nick;
            gchar *c;
            g_printerr (
              "gegl (param_set %s): enum %s has no value '%s'\n",
              key,
              g_type_name (target_type),
              value);
            nick = g_strdup (value);
            for (c = nick; *c; c++)
              {
                *c = g_ascii_tolower (*c);
                if (*c == ' ')
                  *c = '-';
              }
            evalue = g_enum_get_value_by_nick (eclass, nick);
            if (evalue)
              gegl_node_set (o->active, key, evalue->value,
                             NULL);
            g_free (nick);
          }
      }
      else
      {
        GValue gvalue_transformed = {0,};
        GValue gvalue = {0,};
        g_value_init (&gvalue, G_TYPE_STRING);
        g_value_set_string (&gvalue, value);
        g_value_init (&gvalue_transformed, target_type);
        g_value_transform (&gvalue, &gvalue_transformed);
        gegl_node_set_property (o->active, key,
                                &gvalue_transformed);
        g_value_unset (&gvalue);
        g_value_unset (&gvalue_transformed);
      }
    }
    else
    {
       fprintf (stderr, "wanted to set %s to %s\n", key, value);
    }
    g_free (key);
  }
  else
  {
    if (g_str_equal  (commandline, "remove"))
    {
      node_remove (event, data1, data2);
    }
    else if (g_str_equal  (commandline, "pick"))
    {
      tool = TOOL_PICK;
    }
    else if (g_str_equal  (commandline, "pan"))
    {
      tool = TOOL_PAN;
    }
    else if (g_str_equal  (commandline, "paint"))
    {
      tool = TOOL_PAINT;
    }
    else if (g_str_equal  (commandline, "move"))
    {
      tool = TOOL_MOVE;
    }
    else if (g_str_equal  (commandline, "q"))
    {
      mrg_quit (mrg);
    } else if (g_str_equal (commandline, "n"))
    {
      ////
    } else
    {
      node_add_above (event, data1, data2);
      if (strchr (commandline, ':'))
      {
        gegl_node_set (o->active, "operation", commandline, NULL);
      }
      else
      {
        char temp[1024];
        g_snprintf (temp, 1023, "gegl:%s", commandline);
        gegl_node_set (o->active, "operation", temp, NULL);
      }
      o->editing_op_name=0;
    }
  }

}

static void commandline_run (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  if (commandline[0])
    run_command (event, data1, commandline);
  else
    {
      o->show_graph = !o->show_graph;
    }

  commandline[0]=0;
  mrg_set_cursor_pos (event->mrg, 0);
  mrg_queue_draw (o->mrg, NULL);

  mrg_event_stop_propagate (event);
}

static void gegl_ui (Mrg *mrg, void *data)
{
  State *o = data;

  switch (renderer)
  {
     case GEGL_RENDERER_BLIT:
     case GEGL_RENDERER_BLIT_MIPMAP:
       mrg_gegl_blit (mrg,
                      0, 0,
                      mrg_width (mrg), mrg_height (mrg),
                      o->sink,
                      o->u, o->v,
                      o->scale,
                      o->render_quality);
     break;
     case GEGL_RENDERER_THREAD:
     case GEGL_RENDERER_IDLE:
       if (o->processor_buffer)
       {
         GeglBuffer *buffer = g_object_ref (o->processor_buffer);
         mrg_gegl_buffer_blit (mrg,
                               0, 0,
                               mrg_width (mrg), mrg_height (mrg),
                               buffer,
                               o->u, o->v,
                               o->scale,
                               o->render_quality);
         g_object_unref (buffer);
       }
       break;
  }

  if (g_str_has_suffix (o->path, ".gif") ||
      g_str_has_suffix (o->path, ".GIF"))
   {
     int frames = 0;
     int frame_delay = 0;
     gegl_node_get (o->source, "frames", &frames, "frame-delay", &frame_delay, NULL);
     if (o->prev_ms + frame_delay  < mrg_ms (mrg))
     {
       o->frame_no++;
       fprintf (stderr, "\r%i/%i", o->frame_no, frames);   /* */
       if (o->frame_no >= frames)
         o->frame_no = 0;
       gegl_node_set (o->source, "frame", o->frame_no, NULL);
       o->prev_ms = mrg_ms (mrg);
    }
       mrg_queue_draw (o->mrg, NULL);
   }
  else if (o->is_video)
   {
     int frames = 0;
     o->frame_no++;
     gegl_node_get (o->source, "frames", &frames, NULL);
     fprintf (stderr, "\r%i/%i", o->frame_no, frames);   /* */
     if (o->frame_no >= frames)
       o->frame_no = 0;
     gegl_node_set (o->source, "frame", o->frame_no, NULL);
     mrg_queue_draw (o->mrg, NULL);
   }

  if (o->is_video)
  {
    GeglAudioFragment *audio = NULL;
    gdouble fps;
    gegl_node_get (o->source, "audio", &audio, "frame-rate", &fps, NULL);
    if (audio)
    {
       int sample_count = gegl_audio_fragment_get_sample_count (audio);
       if (sample_count > 0)
       {
         int i;
         if (!audio_started)
         {
           open_audio (mrg, gegl_audio_fragment_get_sample_rate (audio));
           audio_started = 1;
         }
         {
         uint16_t temp_buf[sample_count * 2];
         for (i = 0; i < sample_count; i++)
         {
           temp_buf[i*2] = audio->data[0][i] * 32767.0 * 0.46;
           temp_buf[i*2+1] = audio->data[1][i] * 32767.0 * 0.46;
         }
         mrg_pcm_queue (mrg, (void*)&temp_buf[0], sample_count);
         }

         while (mrg_pcm_get_queued (mrg) > 2000)
            g_usleep (50);
         o->prev_frame_played = o->frame_no;
         deferred_redraw (mrg, NULL);
       }
       g_object_unref (audio);
    }
  }

  if (o->show_controls)
  {
    mrg_printf (mrg, "%s\n", o->path);
  }

  ui_canvas_handling (mrg, o);

  if (o->show_graph)
  {
    ui_debug_op_chain (o);
  }
  else
  {
    struct stat stat_buf;

    if (edited_prop)
      g_free (edited_prop);
    edited_prop = NULL;

    lstat (o->path, &stat_buf);
    if (S_ISREG (stat_buf.st_mode))
    {
      ui_viewer (o);

    }
    else if (S_ISDIR (stat_buf.st_mode))
    {
      ui_dir_viewer (o);
    }

    mrg_add_binding (mrg, "escape", NULL, NULL, go_parent_cb, o);
    mrg_add_binding (mrg, "return", NULL, NULL, toggle_actions_cb, o);
  }

  mrg_add_binding (mrg, "control-q", NULL, NULL, mrg_quit_cb, o);
  mrg_add_binding (mrg, "F11", NULL, NULL,       toggle_fullscreen_cb, o);

  if (!edited_prop && !o->editing_op_name)
  {
    //mrg_add_binding (mrg, "return", NULL, NULL, edit_op, o);
    mrg_add_binding (mrg, "up", NULL, NULL, node_up, o);
    mrg_add_binding (mrg, "down", NULL, NULL, node_down, o);
    mrg_add_binding (mrg, "right", NULL, NULL, node_right, o);


    mrg_add_binding (mrg, "control-o", NULL, NULL, node_add_above, o);
    mrg_add_binding (mrg, "control-i", NULL, NULL, node_add_below, o);
    mrg_add_binding (mrg, "control-a", NULL, NULL, node_add_aux_below, o);
    mrg_add_binding (mrg, "control-x", NULL, NULL, node_remove, o);

  }

  if (!edited_prop && !o->editing_op_name)
  {
    mrg_add_binding (mrg, "tab", NULL, NULL,       toggle_show_controls_cb, o);
    //mrg_add_binding (mrg, "q", NULL, NULL,         mrg_quit_cb, o);
    mrg_add_binding (mrg, "control-f", NULL, NULL,  toggle_fullscreen_cb, o);
    if(1)mrg_add_binding (mrg, "control-a", NULL, NULL,   toggle_slideshow_cb, o);
    mrg_add_binding (mrg, "control-r", NULL, NULL,     preview_less_cb, o);
    mrg_add_binding (mrg, "control-t", NULL, NULL,     preview_more_cb, o);

    mrg_add_binding (mrg, "control-n", NULL, NULL, go_next_cb, o);
    mrg_add_binding (mrg, "control-p", NULL, NULL, go_prev_cb, o);

    if (commandline[0]==0)
    {
      mrg_add_binding (mrg, "+", NULL, NULL,     zoom_in_cb, o);
      mrg_add_binding (mrg, "=", NULL, NULL,     zoom_in_cb, o);
      mrg_add_binding (mrg, "-", NULL, NULL,     zoom_out_cb, o);
      mrg_add_binding (mrg, "1", NULL, NULL,     zoom_1_cb, o);
    }
  }

  if (!edited_prop && !o->editing_op_name)
  {
    mrg_set_edge_left (mrg, mrg_em (mrg) * 2);
    mrg_start_with_style (mrg, ".item", NULL, "color:rgb(255,255,255);background-color: rgba(0,0,0,0.5);");
    mrg_set_xy (mrg, mrg_em (mrg) *2, mrg_height (mrg) - mrg_em (mrg) * 2);
    mrg_edit_start (mrg, update_commandline, o);
    mrg_printf (mrg, "%s", commandline);
    mrg_edit_end (mrg);
    mrg_end (mrg);
    mrg_add_binding (mrg, "return", NULL, NULL, commandline_run, o);


    //mrg_add_binding (mrg, "up", NULL, NULL, node_up, o);
    //mrg_add_binding (mrg, "down", NULL, NULL, node_down, o);
    if (commandline[0]==0)
      mrg_add_binding (mrg, "right", NULL, NULL, node_right, o);


  }

}

/***********************************************/

static char *get_path_parent (const char *path)
{
  char *ret = strdup (path);
  char *lastslash = strrchr (ret, '/');
  if (lastslash)
  {
    if (lastslash == ret)
      lastslash[1] = '\0';
    else
      lastslash[0] = '\0';
  }
  return ret;
}

static char *suffix_path (const char *path)
{
  char *ret;
  if (!path)
    return NULL;
  ret  = malloc (strlen (path) + strlen (suffix) + 3);
  strcpy (ret, path);
  sprintf (ret, "%s%s", path, ".gegl");
  return ret;
}

static char *unsuffix_path (const char *path)
{
  char *ret = NULL, *last_dot;

  if (!path)
    return NULL;
  ret = malloc (strlen (path) + 4);
  strcpy (ret, path);
  last_dot = strrchr (ret, '.');
  *last_dot = '\0';
  return ret;
}

static int is_gegl_path (const char *path)
{
  if (g_str_has_suffix (path, ".gegl"))
    return 1;
  return 0;
}

static void contrasty_stroke (cairo_t *cr)
{
  double x0 = 6.0, y0 = 6.0;
  double x1 = 4.0, y1 = 4.0;

  cairo_device_to_user_distance (cr, &x0, &y0);
  cairo_device_to_user_distance (cr, &x1, &y1);
  cairo_set_source_rgba (cr, 0,0,0,0.5);
  cairo_set_line_width (cr, y0);
  cairo_stroke_preserve (cr);
  cairo_set_source_rgba (cr, 1,1,1,0.5);
  cairo_set_line_width (cr, y1);
  cairo_stroke (cr);
}

static void load_path (State *o)
{
  char *path;
  char *meta;
  populate_path_list (o);
  if (is_gegl_path (o->path))
  {
    if (o->save_path)
      free (o->save_path);
    o->save_path = o->path;
    o->path = unsuffix_path (o->save_path);
  }
  else
  {
    if (o->save_path)
      free (o->save_path);
    o->save_path = suffix_path (o->path);
  }
  path  = o->path;

  if (access (o->save_path, F_OK) != -1)
  {
    /* XXX : fix this in the fuse layer of zn! XXX XXX XXX XXX */
    if (!strstr (o->save_path, ".zn.fs"))
      path = o->save_path;
  }

  g_object_unref (o->gegl);
  o->gegl = NULL;
  o->sink = NULL;
  o->source = NULL;
  o->rev = 0;
  o->u = 0;
  o->v = 0;
  o->is_video = 0;
  o->frame_no = 0;
  o->prev_frame_played = 0;

  if (g_str_has_suffix (path, ".gif"))
  {
    o->gegl = gegl_node_new ();
    o->sink = gegl_node_new_child (o->gegl,
                       "operation", "gegl:nop", NULL);
    o->source = gegl_node_new_child (o->gegl,
         "operation", "gegl:gif-load", "path", path, "frame", o->frame_no, NULL);
    gegl_node_link_many (o->source, o->sink, NULL);
  }
  else if (gegl_str_has_video_suffix (path))
  {
    o->is_video = 1;
    o->gegl = gegl_node_new ();
    o->sink = gegl_node_new_child (o->gegl,
                       "operation", "gegl:nop", NULL);
    o->source = gegl_node_new_child (o->gegl,
         "operation", "gegl:ff-load", "path", path, "frame", o->frame_no, NULL);
    gegl_node_link_many (o->source, o->sink, NULL);
  }
  else
  {
    meta = NULL;
    if (is_gegl_path (path))
      g_file_get_contents (path, &meta, NULL, NULL);
    //meta = gegl_meta_get (path);
    if (meta)
    {
      GSList *nodes, *n;
      char *containing_path = get_path_parent (o->path);
      o->gegl = gegl_node_new_from_serialized (meta, containing_path);
      free (containing_path);
      o->sink = gegl_node_new_child (o->gegl,
                       "operation", "gegl:nop", NULL);
      o->source = NULL;
      gegl_node_link_many (
        gegl_node_get_producer (o->gegl, "input", NULL), o->sink, NULL);
      nodes = gegl_node_get_children (o->gegl);
      for (n = nodes; n; n=n->next)
      {
        const char *op_name = gegl_node_get_operation (n->data);
        if (!strcmp (op_name, "gegl:load"))
        {
          GeglNode *load;
          gchar *path;
          gegl_node_get (n->data, "path", &path, NULL);
          load_into_buffer (o, path);
          gegl_node_set (n->data, "operation", "gegl:nop", NULL);
          o->source = n->data;
          load = gegl_node_new_child (o->gegl, "operation", "gegl:buffer-source",
                                               "buffer", o->buffer, NULL);
          gegl_node_link_many (load, o->source, NULL);
          g_free (path);
          break;
        }
      }
      o->save = gegl_node_new_child (o->gegl, "operation", "gegl:save",
                                              "path", path,
                                              NULL);
    }
    else
    {
      o->gegl = gegl_node_new ();
      o->sink = gegl_node_new_child (o->gegl,
                         "operation", "gegl:nop", NULL);
      load_into_buffer (o, path);
      o->source = gegl_node_new_child (o->gegl,
                                     "operation", "gegl:buffer-source",
                                     NULL);
      o->save = gegl_node_new_child (o->gegl,
                                     "operation", "gegl:save",
                                     "path", o->save_path,
                                     NULL);
    gegl_node_link_many (o->source, o->sink, NULL);
    gegl_node_set (o->source, "buffer", o->buffer, NULL);
  }
  }
  {
    struct stat stat_buf;
    lstat (o->path, &stat_buf);
    if (S_ISREG (stat_buf.st_mode))
    {
      if (o->is_video)
        center (o);
      else
        zoom_to_fit (o);
    }
  }
  if (o->ops)
  {
    GeglNode *ret_sink = NULL;
    GError *error = (void*)(&ret_sink);

    char *containing_path = get_path_parent (o->path);
    gegl_create_chain_argv (o->ops,
                    gegl_node_get_producer (o->sink, "input", NULL),
                    o->sink, 0, gegl_node_get_bounding_box (o->sink).height,
                    containing_path,
                    &error);
    free (containing_path);
    if (error)
    {
      fprintf (stderr, "Error: %s\n", error->message);
    }
    if (ret_sink)
    {
      gegl_node_process (ret_sink);
      exit(0);
    }

    zoom_to_fit (o);
  }
  if (o->processor)
      g_object_unref (o->processor);
  o->processor = gegl_node_new_processor (o->sink, NULL);

  mrg_queue_draw (o->mrg, NULL);
}

static void go_parent (State *o)
{
  char *lastslash = strrchr (o->path, '/');
  if (lastslash)
  {
    if (lastslash == o->path)
      lastslash[1] = '\0';
    else
      lastslash[0] = '\0';
    load_path (o);
    mrg_queue_draw (o->mrg, NULL);
  }
}

static void go_next (State *o)
{
  GList *curr = g_list_find_custom (o->paths, o->path, (void*)g_strcmp0);

  if (curr && curr->next)
  {
    free (o->path);
    o->path = strdup (curr->next->data);
    load_path (o);
    mrg_queue_draw (o->mrg, NULL);
  }
}

static void go_prev (State *o)
{
  GList *curr = g_list_find_custom (o->paths, o->path, (void*)g_strcmp0);

  if (curr && curr->prev)
  {
    free (o->path);
    o->path = strdup (curr->prev->data);
    load_path (o);
    mrg_queue_draw (o->mrg, NULL);
  }
}

static void go_next_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  if (o->rev)
    save_cb (event, data1, data2);
  go_next (data1);
  o->active = NULL;
  mrg_event_stop_propagate (event);
}

static void go_parent_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  if (o->rev)
    save_cb (event, data1, data2);
  go_parent (data1);
  o->active = NULL;
  mrg_event_stop_propagate (event);
}

static void go_prev_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  if (o->rev)
    save_cb (event, data1, data2);
  go_prev (data1);
  o->active = NULL;
  mrg_event_stop_propagate (event);
}

static void drag_preview (MrgEvent *e)
{
  State *o = hack_state;
  static float old_factor = 1;
  switch (e->type)
  {
    case MRG_DRAG_PRESS:
      old_factor = o->render_quality;
      if (o->render_quality < o->preview_quality)
        o->render_quality = o->preview_quality;
      break;
    case MRG_DRAG_RELEASE:
      o->render_quality = old_factor;
      mrg_queue_draw (e->mrg, NULL);
      break;
    default:
    break;
  }
}

static void load_into_buffer (State *o, const char *path)
{
  GeglNode *gegl, *load, *sink;
  struct stat stat_buf;

  if (o->buffer)
  {
    g_object_unref (o->buffer);
    o->buffer = NULL;
  }

  lstat (path, &stat_buf);
  if (S_ISREG (stat_buf.st_mode))
  {


  gegl = gegl_node_new ();
  load = gegl_node_new_child (gegl, "operation", "gegl:load",
                                    "path", path,
                                    NULL);
  sink = gegl_node_new_child (gegl, "operation", "gegl:buffer-sink",
                                    "buffer", &(o->buffer),
                                    NULL);
  gegl_node_link_many (load, sink, NULL);
  gegl_node_process (sink);
  g_object_unref (gegl);

  {
    GExiv2Orientation orientation = path_get_orientation (path);
    gboolean hflip = FALSE;
    gboolean vflip = FALSE;
    double degrees = 0.0;
    switch (orientation)
    {
      case GEXIV2_ORIENTATION_UNSPECIFIED:
      case GEXIV2_ORIENTATION_NORMAL:
        break;
      case GEXIV2_ORIENTATION_HFLIP: hflip=TRUE; break;
      case GEXIV2_ORIENTATION_VFLIP: vflip=TRUE; break;
      case GEXIV2_ORIENTATION_ROT_90: degrees = 90.0; break;
      case GEXIV2_ORIENTATION_ROT_90_HFLIP: degrees = 90.0; hflip=TRUE; break;
      case GEXIV2_ORIENTATION_ROT_90_VFLIP: degrees = 90.0; vflip=TRUE; break;
      case GEXIV2_ORIENTATION_ROT_180: degrees = 180.0; break;
      case GEXIV2_ORIENTATION_ROT_270: degrees = 270.0; break;
    }

    if (degrees != 0.0 || vflip || hflip)
     {
       /* XXX: deal with vflip/hflip */
       GeglBuffer *new_buffer = NULL;
       GeglNode *rotate;
       gegl = gegl_node_new ();
       load = gegl_node_new_child (gegl, "operation", "gegl:buffer-source",
                                   "buffer", o->buffer,
                                   NULL);
       sink = gegl_node_new_child (gegl, "operation", "gegl:buffer-sink",
                                   "buffer", &(new_buffer),
                                   NULL);
       rotate = gegl_node_new_child (gegl, "operation", "gegl:rotate",
                                   "degrees", -degrees,
                                   "sampler", GEGL_SAMPLER_NEAREST,
                                   NULL);
       gegl_node_link_many (load, rotate, sink, NULL);
       gegl_node_process (sink);
       g_object_unref (gegl);
       g_object_unref (o->buffer);
       o->buffer = new_buffer;
     }
  }

#if 0 /* hack to see if having the data in some formats already is faster */
  {
  GeglBuffer *tempbuf;
  tempbuf = gegl_buffer_new (gegl_buffer_get_extent (o->buffer),
                                         babl_format ("RGBA float"));

  gegl_buffer_copy (o->buffer, NULL, GEGL_ABYSS_NONE, tempbuf, NULL);
  g_object_unref (o->buffer);
  o->buffer = tempbuf;
  }
#endif
    }
  else
    {
      GeglRectangle extent = {0,0,1,1}; /* segfaults with NULL / 0,0,0,0*/
      o->buffer = gegl_buffer_new (&extent, babl_format("R'G'B' u8"));
    }
}

#if 0
static GeglNode *locate_node (State *o, const char *op_name)
{
  GeglNode *iter = o->sink;
  while (iter)
   {
     char *opname = NULL;
     g_object_get (iter, "operation", &opname, NULL);
     if (!strcmp (opname, op_name))
       return iter;
     g_free (opname);
     iter = gegl_node_get_producer (iter, "input", NULL);
   }
  return NULL;
}
#endif

static void zoom_to_fit (State *o)
{
  Mrg *mrg = o->mrg;
  GeglRectangle rect = gegl_node_get_bounding_box (o->sink);
  float scale, scale2;

  if (rect.width == 0 || rect.height == 0)
  {
    o->scale = 1.0;
    o->u = 0.0;
    o->v = 0.0;
    return;
  }

  scale = 1.0 * mrg_width (mrg) / rect.width;
  scale2 = 1.0 * mrg_height (mrg) / rect.height;

  if (scale2 < scale) scale = scale2;

  o->scale = scale;

  o->u = -(mrg_width (mrg) - rect.width * o->scale) / 2;
  o->v = -(mrg_height (mrg) - rect.height * o->scale) / 2;
  o->u += rect.x * o->scale;
  o->v += rect.y * o->scale;

  mrg_queue_draw (mrg, NULL);
}
static void center (State *o)
{
  Mrg *mrg = o->mrg;
  GeglRectangle rect = gegl_node_get_bounding_box (o->sink);
  o->scale = 1.0;

  o->u = -(mrg_width (mrg) - rect.width * o->scale) / 2;
  o->v = -(mrg_height (mrg) - rect.height * o->scale) / 2;
  o->u += rect.x * o->scale;
  o->v += rect.y * o->scale;

  mrg_queue_draw (mrg, NULL);
}

static inline void zoom_to_fit_buffer (State *o)
{
  Mrg *mrg = o->mrg;
  GeglRectangle rect = *gegl_buffer_get_extent (o->buffer);
  float scale, scale2;

  scale = 1.0 * mrg_width (mrg) / rect.width;
  scale2 = 1.0 * mrg_height (mrg) / rect.height;

  if (scale2 < scale) scale = scale2;

  o->scale = scale;
  o->u = -(mrg_width (mrg) - rect.width * o->scale) / 2;
  o->v = -(mrg_height (mrg) - rect.height * o->scale) / 2;
  o->u += rect.x * o->scale;
  o->v += rect.y * o->scale;


  mrg_queue_draw (mrg, NULL);
}

static void zoom_fit_cb (MrgEvent *e, void *data1, void *data2)
{
  zoom_to_fit (data1);
}


static int deferred_zoom_to_fit (Mrg *mrg, void *data)
{
  zoom_to_fit (data);
  return 0;
}

static void pan_left_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  float amount = mrg_width (event->mrg) * 0.1;
  o->u = o->u - amount;
  mrg_queue_draw (o->mrg, NULL);
}

static void pan_right_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  float amount = mrg_width (event->mrg) * 0.1;
  o->u = o->u + amount;
  mrg_queue_draw (o->mrg, NULL);
}

static void pan_down_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  float amount = mrg_width (event->mrg) * 0.1;
  o->v = o->v + amount;
  mrg_queue_draw (o->mrg, NULL);
}

static void pan_up_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  float amount = mrg_width (event->mrg) * 0.1;
  o->v = o->v - amount;
  mrg_queue_draw (o->mrg, NULL);
}

static void get_coords (State *o, float screen_x, float screen_y, float *gegl_x, float *gegl_y)
{
  float scale = o->scale;
  *gegl_x = (o->u + screen_x) / scale;
  *gegl_y = (o->v + screen_y) / scale;
}

static void preview_more_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  o->render_quality *= 2;
  mrg_queue_draw (o->mrg, NULL);
}

static void preview_less_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  o->render_quality /= 2;
  if (o->render_quality <= 1.0)
    o->render_quality = 1.0;
  mrg_queue_draw (o->mrg, NULL);
}

static void zoom_1_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  float x, y;
  get_coords (o, mrg_width(o->mrg)/2, mrg_height(o->mrg)/2, &x, &y);
  o->scale = 1.0;
  o->u = x * o->scale - mrg_width(o->mrg)/2;
  o->v = y * o->scale - mrg_height(o->mrg)/2;
  mrg_queue_draw (o->mrg, NULL);
}

static void zoom_at (State *o, float screen_cx, float screen_cy, float factor)
{
  float x, y;
  get_coords (o, screen_cx, screen_cy, &x, &y);
  o->scale *= factor;
  o->u = x * o->scale - screen_cx;
  o->v = y * o->scale - screen_cy;
  mrg_queue_draw (o->mrg, NULL);
}

static void zoom_in_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  zoom_at (data1, mrg_width(o->mrg)/2, mrg_height(o->mrg)/2, 1.1);
}

static void zoom_out_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  zoom_at (data1, mrg_width(o->mrg)/2, mrg_height(o->mrg)/2, 1.0/1.1);
}

static void scroll_cb (MrgEvent *event, void *data1, void *data2)
{
  switch (event->scroll_direction)
  {
     case MRG_SCROLL_DIRECTION_DOWN:
       zoom_at (data1, event->device_x, event->device_y, 1.0/1.05);
       break;
     case MRG_SCROLL_DIRECTION_UP:
       zoom_at (data1, event->device_x, event->device_y, 1.05);
       break;
     default:
       break;
  }
}

static void toggle_actions_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  o->show_graph = !o->show_graph;
  fprintf (stderr, "!\n");
  mrg_queue_draw (o->mrg, NULL);
}

static void toggle_fullscreen_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  mrg_set_fullscreen (event->mrg, !mrg_is_fullscreen (event->mrg));
  mrg_event_stop_propagate (event);
  mrg_add_timeout (event->mrg, 250, deferred_zoom_to_fit, o);
}

static void discard_cb (MrgEvent *event, void *data1, void *data2)
{
  State *o = data1;
  char *old_path = strdup (o->path);
  char *tmp;
  char *lastslash;
  go_next_cb (event, data1, data2);
  if (!strcmp (old_path, o->path))
   {
     go_prev_cb (event, data1, data2);
   }
  tmp = strdup (old_path);
  lastslash  = strrchr (tmp, '/');
  if (lastslash)
  {
    char command[2048];
    char *suffixed = suffix_path (old_path);
    if (lastslash == tmp)
      lastslash[1] = '\0';
    else
      lastslash[0] = '\0';

    sprintf (command, "mkdir %s/.discard > /dev/null 2>&1", tmp);
    system (command);
    sprintf (command, "mv %s %s/.discard", old_path, tmp);
    sprintf (command, "mv %s %s/.discard", suffixed, tmp);
    system (command);
    free (suffixed);
  }
  free (tmp);
  free (old_path);
}

static void save_cb (MrgEvent *event, void *data1, void *data2)
{
  GeglNode *load;
  State *o = data1;
  gchar *path;
  char *serialized;

  gegl_node_link_many (o->sink, o->save, NULL);
  gegl_node_process (o->save);
  gegl_node_get (o->save, "path", &path, NULL);
  fprintf (stderr, "saved to %s\n", path);

  load = gegl_node_new_child (o->gegl, "operation", "gegl:load",
                                    "path", o->path,
                                    NULL);
  gegl_node_link_many (load, o->source, NULL);
  {
    char *containing_path = get_path_parent (o->path);
    serialized = gegl_serialize (NULL, o->sink, containing_path, GEGL_SERIALIZE_TRIM_DEFAULTS|GEGL_SERIALIZE_VERSION|GEGL_SERIALIZE_INDENT);
    free (containing_path);
  }
  gegl_node_remove_child (o->gegl, load);

  g_file_set_contents (path, serialized, -1, NULL);
  g_free (serialized);
  o->rev = 0;
}

#if 0
void gegl_node_defaults (GeglNode *node)
{
  const gchar* op_name = gegl_node_get_operation (node);
  {
    guint n_props;
    GParamSpec **pspecs = gegl_operation_list_properties (op_name, &n_props);
    if (pspecs)
    {
      for (gint i = 0; i < n_props; i++)
      {
        if (g_type_is_a (pspecs[i]->value_type, G_TYPE_DOUBLE))
        {
          GParamSpecDouble    *pspec = (void*)pspecs[i];
          gegl_node_set (node, pspecs[i]->name, pspec->default_value, NULL);
        }
        else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_INT))
        {
          GParamSpecInt *pspec = (void*)pspecs[i];
          gegl_node_set (node, pspecs[i]->name, pspec->default_value, NULL);
        }
        else if (g_type_is_a (pspecs[i]->value_type, G_TYPE_STRING))
        {
          GParamSpecString *pspec = (void*)pspecs[i];
          gegl_node_set (node, pspecs[i]->name, pspec->default_value, NULL);
        }
      }
      g_free (pspecs);
    }
  }
}
#endif

/* loads the source image corresponding to o->path into o->buffer and
 * creates live gegl pipeline, or nops.. rigs up o->save_path to be
 * the location where default saves ends up.
 */
void
gegl_meta_set (const char *path,
               const char *meta_data)
{
  GError *error = NULL;
  GExiv2Metadata *e2m = gexiv2_metadata_new ();
  gexiv2_metadata_open_path (e2m, path, &error);
  if (error)
  {
    g_warning ("%s", error->message);
  }
  else
  {
    if (gexiv2_metadata_has_tag (e2m, "Xmp.xmp.GEGL"))
      gexiv2_metadata_clear_tag (e2m, "Xmp.xmp.GEGL");

    gexiv2_metadata_set_tag_string (e2m, "Xmp.xmp.GEGL", meta_data);
    gexiv2_metadata_save_file (e2m, path, &error);
    if (error)
      g_warning ("%s", error->message);
  }
  g_object_unref (e2m);
}

char *
gegl_meta_get (const char *path)
{
  gchar  *ret   = NULL;
  GError *error = NULL;
  GExiv2Metadata *e2m = gexiv2_metadata_new ();
  gexiv2_metadata_open_path (e2m, path, &error);
  if (!error)
    ret = gexiv2_metadata_get_tag_string (e2m, "Xmp.xmp.GEGL");
  /*else
    g_warning ("%s", error->message);*/
  g_object_unref (e2m);
  return ret;
}

GExiv2Orientation path_get_orientation (const char *path)
{
  GExiv2Orientation ret = 0;
  GError *error = NULL;
  GExiv2Metadata *e2m = gexiv2_metadata_new ();
  gexiv2_metadata_open_path (e2m, path, &error);
  if (!error)
    ret = gexiv2_metadata_get_orientation (e2m);
  /*else
    g_warning ("%s", error->message);*/
  g_object_unref (e2m);
  return ret;
}

#endif
