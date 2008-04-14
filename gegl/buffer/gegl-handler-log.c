/* This file is part of GEGL.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright 2006, 2007 Øyvind Kolås <pippin@gimp.org>
 */
#include <glib.h>
#include <glib-object.h>
#include <string.h>

#include "gegl-handler.h"
#include "gegl-handler-log.h"

G_DEFINE_TYPE (GeglHandlerLog, gegl_handler_log, GEGL_TYPE_HANDLER)


static char *commands[] =
{
  "idle",
  "set",
  "get",
  "is_cached",
  "exist",
  "void",
  "void tl", 
  "void tr", 
  "void bl",
  "void br",
  "undo start group",
  "last command",
  "eeek",
  NULL
};

static gpointer
command (GeglSource     *gegl_source,
         GeglTileCommand command,
         gint            x,
         gint            y,
         gint            z,
         gpointer        data)
{
  GeglHandler *handler = GEGL_HANDLER (gegl_source);
  gpointer     result = NULL;

  result = gegl_handler_chain_up (handler, command, x, y, z, data);

  switch (command)
    {
      case GEGL_TILE_IDLE:
        break;
      default:
        g_print ("(%s %p %p %i,%i,%i => %s)", 
          commands[command], (void *) gegl_source, data, x, y, z,
          result?"1":"0");
    }
  return result;
}

static void
gegl_handler_log_class_init (GeglHandlerLogClass *klass)
{
  GeglSourceClass *source_class = GEGL_SOURCE_CLASS (klass);

  source_class->command  = command;
}

static void
gegl_handler_log_init (GeglHandlerLog *self)
{
}
