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
 * Copyright 2006 Øyvind Kolås <pippin@gimp.org>
 */
#include "config.h"

#ifdef GEGL_CHANT_PROPERTIES

gegl_chant_path (path, "File", "/tmp/test.raw", "Path of file to load.")

#else

#define GEGL_CHANT_TYPE_SOURCE
#define GEGL_CHANT_C_FILE       "raw-load.c"

#include "gegl-chant.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define popen(n,m) _popen(n,m)
#define pclose(f) _pclose(f)
#endif
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PIPE_MODE "r"
#define MAX_SAMPLE 65535
#define ERROR -1

static void
load_buffer (GeglChantO *op_raw_load)
{
  if (!op_raw_load->chant_data)
    {
      FILE  *pfp;
      gchar *command;
      gint   width, height, val_max;
      gchar  newline;

      command = g_strdup_printf ("dcraw -4 -c '%s'\n", op_raw_load->path);
      pfp = popen (command, PIPE_MODE);
      g_free (command);

      if (fscanf (pfp, "P6 %d %d %d %c",
         &width, &height, &val_max, &newline) != 4)
        {
          pclose (pfp);
          g_warning ("not able to aquire raw data");
          return;
        }

        {
          GeglRectangle extent = { 0, 0, width, height };
          op_raw_load->chant_data = (gpointer) gegl_buffer_new (&extent,
                                                   babl_format_new (
                                                     babl_model ("RGB"),
                                                     babl_type ("u16"),
                                                     babl_component ("G"),
                                                     babl_component ("B"),
                                                     babl_component ("R"),
                                                     NULL));
        }
         {

           guint16 *buf = g_new (guint16, width * height * 3);
           fread (buf, 1, width * height * 3 * 2, pfp);
           gegl_buffer_set (GEGL_BUFFER (op_raw_load->chant_data),
                            NULL,
                            babl_format_new (
                                        babl_model ("RGB"),
                                        babl_type ("u16"),
                                        babl_component ("G"),
                                        babl_component ("B"),
                                        babl_component ("R"),
                                        NULL),
                            buf,
                            GEGL_AUTO_ROWSTRIDE
                           );
           g_free (buf);
         }
       fclose (pfp);
    }
}

static GeglRectangle
get_bounding_box (GeglOperation *operation)
{
  GeglRectangle result = {0,0,0,0};
  GeglChantO   *o = GEGL_CHANT_PROPERTIES (operation);

  load_buffer (o);

  if (o->chant_data)
    {
      result.width  = gegl_buffer_get_width  (GEGL_BUFFER (o->chant_data));
      result.height = gegl_buffer_get_height (GEGL_BUFFER (o->chant_data));
    }

  return result;
}

static gboolean
process (GeglOperation       *operation,
         GeglNodeContext     *context,
         GeglBuffer          *output_ignored,
         const GeglRectangle *result)
{
  GeglChantO *o = GEGL_CHANT_PROPERTIES (operation);
  GeglBuffer *output;

  g_assert (o->chant_data);
  output = GEGL_BUFFER (o->chant_data);
  gegl_node_context_set_object (context, "output", G_OBJECT (output));

  o->chant_data = NULL;
  return TRUE;
}


static void
operation_class_init (GeglChantClass *klass)
{
  GeglOperationClass       *operation_class;
  GeglOperationSourceClass *source_class;

  operation_class = GEGL_OPERATION_CLASS (klass);
  source_class    = GEGL_OPERATION_SOURCE_CLASS (klass);

  source_class->process = process;
  operation_class->get_bounding_box = get_bounding_box;

  operation_class->name        = "raw-load";
  operation_class->categories  = "hidden";
  operation_class->description =
        "Raw image loader, wrapping dcraw with pipes.";

  gegl_extension_handler_register (".raw", "raw-load");
  gegl_extension_handler_register (".RAW", "raw-load");
  gegl_extension_handler_register (".raf", "raw-load");
  gegl_extension_handler_register (".RAF", "raw-load");
  gegl_extension_handler_register (".nef", "raw-load");
  gegl_extension_handler_register (".NEF", "raw-load");
}

#endif
