/* -*- c-file-style: "ruby"; indent-tabs-mode: nil -*- */
/************************************************

  rbgeglversion.h -

  This file was generated by mkmf-gnome2.rb.

************************************************/

#ifndef __RBGEGL_VERSION_H__
#define __RBGEGL_VERSION_H__

#define GEGL_MAJOR_VERSION (0)
#define GEGL_MINOR_VERSION (0)
#define GEGL_MICRO_VERSION (13)

#define GEGL_CHECK_VERSION(major,minor,micro)    \
    (GEGL_MAJOR_VERSION > (major) || \
     (GEGL_MAJOR_VERSION == (major) && GEGL_MINOR_VERSION > (minor)) || \
     (GEGL_MAJOR_VERSION == (major) && GEGL_MINOR_VERSION == (minor) && \
      GEGL_MICRO_VERSION >= (micro)))


#endif /* __RBGEGL_VERSION_H__ */
