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
 * Copyright 2013 Carlos Zubieta <czubieta.dev@gmail.com>
 */

static const char* dot_cl_source =
"__kernel void cl_calc_block_colors (__global const float4 *in,                \n"
"                                    __global       float4 *block_colors,      \n"
"                                                   int     cx0,               \n"
"                                                   int     cy0,               \n"
"                                                   int     size,              \n"
"                                                   float   weight,            \n"
"                                                   int     roi_x,             \n"
"                                                   int     roi_y,             \n"
"                                                   int     line_width)        \n"
"{                                                                             \n"
"  int    cx   = cx0 + get_global_id(0);                                       \n"
"  int    cy   = cy0 + get_global_id(1);                                       \n"
"  int    px   = (cx * size) - roi_x + size;                                   \n"
"  int    py   = (cy * size) - roi_y + size;                                   \n"
"  float4 mean = (float4)(0.0f);                                               \n"
"  float4 tmp;                                                                 \n"
"  int    i, j;                                                                \n"
"                                                                              \n"
"  for( j = py; j < py+size; ++j)                                              \n"
"    {                                                                         \n"
"      for (i = px; i < px+size; ++i)                                          \n"
"        {                                                                     \n"
"          mean += in[j * line_width + i];                                     \n"
"        }                                                                     \n"
"    }                                                                         \n"
"  block_colors[(cx-cx0) + get_global_size(0) * (cy-cy0)] = mean * weight;     \n"
"}                                                                             \n"
"                                                                              \n"
"__kernel void cl_dot (__global const float4 *block_colors,                    \n"
"                      __global       float4 *out,                             \n"
"                                     int     cx0,                             \n"
"                                     int     cy0,                             \n"
"                                     int     size,                            \n"
"                                     float   radius2,                         \n"
"                                     int     roi_x,                           \n"
"                                     int     roi_y,                           \n"
"                                     int     block_count_x)                   \n"
"{                                                                             \n"
"  int    gidx  = get_global_id(0);                                            \n"
"  int    gidy  = get_global_id(1);                                            \n"
"  int    x     = gidx + roi_x;                                                \n"
"  int    y     = gidy + roi_y;                                                \n"
"  int    cy    = y/size;                                                      \n"
"  int    cx    = x/size;                                                      \n"
"  float  cellx = convert_float(x - cx * size) - convert_float(size) / 2.0;    \n"
"  float  celly = convert_float(y - cy * size) - convert_float(size) / 2.0;    \n"
"  float4 tmp  = (float4)(0.0);                                                \n"
"                                                                              \n"
"  if((cellx * cellx + celly * celly) <= radius2)                              \n"
"    tmp = block_colors[(cx-cx0) + block_count_x * (cy-cy0)];                  \n"
"                                                                              \n"
"  out[gidx + get_global_size(0) * gidy] = tmp;                                \n"
"}                                                                             \n"
;
