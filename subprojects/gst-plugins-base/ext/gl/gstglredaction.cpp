/*
 * GStreamer
 * Copyright (C) 2008 Filippo Argiolas <filippo.argiolas@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-glredaction
 * @title: glredaction
 *
 * Redaction GL video texture with a PNG image
 *
 * ## Examples
 * |[
 * gst-launch-1.0 videotestsrc ! glredaction location=image.jpg ! glimagesink
 * ]|
 * FBO (Frame Buffer Object) is required.
 *
 */

#include <cstring>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/base/gsttypefindhelper.h>
#include <gst/gl/gstglconfig.h>

#include "gstglelements.h"
#include "gstglredaction.hh"
#include "effects/gstgleffectssources.h"
#include "gstglutils.h"

#include <stdio.h>
#include <stdlib.h>
#if defined(_MSC_VER) || (defined (__MINGW64_VERSION_MAJOR) && __MINGW64_VERSION_MAJOR >= 6)
#define HAVE_BOOLEAN
#endif
#include <jpeglib.h>
#include <png.h>

#if PNG_LIBPNG_VER >= 10400
#define int_p_NULL         NULL
#define png_infopp_NULL    NULL
#endif

#define GST_CAT_DEFAULT gst_gl_redaction_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_gl_redaction_debug, "glredaction", 0, "glredaction element");

#define gst_gl_redaction_parent_class parent_class

G_DEFINE_TYPE_WITH_CODE (GstGLRedaction, gst_gl_redaction, GST_TYPE_GL_FILTER,
    DEBUG_INIT);
GST_ELEMENT_REGISTER_DEFINE_WITH_CODE (glredaction, "glredaction",
    GST_RANK_NONE, GST_TYPE_GL_REDACTION, gl_element_init (plugin));

static gboolean gst_gl_redaction_set_caps (GstGLFilter * filter,
    GstCaps * incaps, GstCaps * outcaps);

static void gst_gl_redaction_finalize (GObject * object);
static void gst_gl_redaction_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gl_redaction_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static void gst_gl_redaction_before_transform (GstBaseTransform * trans,
    GstBuffer * outbuf);
static gboolean gst_gl_redaction_filter_texture (GstGLFilter * filter,
    GstGLMemory * in_tex, GstGLMemory * out_tex);

static gboolean gst_gl_redaction_load_png (GstGLRedaction * redaction, FILE * fp);
static gboolean gst_gl_redaction_load_jpeg (GstGLRedaction * redaction, FILE * fp);

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_OFFSET_X,
  PROP_OFFSET_Y,
  PROP_RELATIVE_X,
  PROP_RELATIVE_Y,
  PROP_REDACTION_WIDTH,
  PROP_REDACTION_HEIGHT,
  PROP_ALPHA
};

/* *INDENT-OFF* */
/* vertex source */
static const gchar *redaction_v_src =
    "attribute vec4 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main()\n"
    "{\n"
    "   gl_Position = a_position;\n"
    "   v_texcoord = a_texcoord;\n"
    "}";

/* fragment source */
static const gchar *redaction_f_src =
    "uniform sampler2D texture;\n"
    "uniform float alpha;\n"
    "varying vec2 v_texcoord;\n"
    "void main()\n"
    "{\n"
    "  vec4 rgba = texture2D( texture, v_texcoord );\n"
    "  gl_FragColor = vec4(rgba.rgb, rgba.a * alpha);\n"
    "}\n";
/* *INDENT-ON* */

/* init resources that need a gl context */
static gboolean
gst_gl_redaction_gl_start (GstGLBaseFilter * base_filter)
{
  GstGLRedaction *redaction = GST_GL_REDACTION (base_filter);
  gchar *frag_str;
  gboolean ret;

  if (!GST_GL_BASE_FILTER_CLASS (parent_class)->gl_start (base_filter))
    return FALSE;

  frag_str =
      g_strdup_printf ("%s%s",
      gst_gl_shader_string_get_highest_precision (base_filter->context,
          GST_GLSL_VERSION_NONE, (GstGLSLProfile)
          (GST_GLSL_PROFILE_ES | GST_GLSL_PROFILE_COMPATIBILITY)), redaction_f_src);

  /* blocking call, wait the opengl thread has compiled the shader */
  ret = gst_gl_context_gen_shader (base_filter->context, redaction_v_src,
      frag_str, &redaction->shader);
  g_free (frag_str);

  for (int i = 0; i < _GstGLRedaction::NUM_REDACTIONS; ++i) 
  {
    int x = -500 + rand()%2000;
    int y = -500 + rand()%1000;
    redaction->redactions[i] = {x, y, x - 50 + rand()%100,  y - 50 + rand()%100};
    x = -100 + rand()%700;
    y = -100 + rand()%400;
    redaction->prev_redactions[i] = {x, y, x - 50 + rand()%100,  y - 50 + rand()%100};
  }
 
  return ret;
}

/* free resources that need a gl context */
static void
gst_gl_redaction_gl_stop (GstGLBaseFilter * base_filter)
{
  GstGLRedaction *redaction = GST_GL_REDACTION (base_filter);
  const GstGLFuncs *gl = base_filter->context->gl_vtable;

  if (redaction->shader) {
    gst_object_unref (redaction->shader);
    redaction->shader = NULL;
  }

  if (redaction->image_memory) {
    gst_memory_unref ((GstMemory *) redaction->image_memory);
    redaction->image_memory = NULL;
  }

  if (redaction->vao) {
    gl->DeleteVertexArrays (1, &redaction->vao);
    redaction->vao = 0;
  }

  if (redaction->vbo) {
    gl->DeleteBuffers (1, &redaction->vbo);
    redaction->vbo = 0;
  }

  if (redaction->vbo_indices) {
    gl->DeleteBuffers (1, &redaction->vbo_indices);
    redaction->vbo_indices = 0;
  }

  if (redaction->redaction_vao) {
    gl->DeleteVertexArrays (1, &redaction->redaction_vao);
    redaction->redaction_vao = 0;
  }

  if (redaction->redaction_vbo) {
    gl->DeleteBuffers (1, &redaction->redaction_vbo);
    redaction->redaction_vbo = 0;
  }

  GST_GL_BASE_FILTER_CLASS (parent_class)->gl_stop (base_filter);
}

static void
gst_gl_redaction_class_init (GstGLRedactionClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);

  gst_gl_filter_add_rgba_pad_templates (GST_GL_FILTER_CLASS (klass));

  gobject_class->finalize = gst_gl_redaction_finalize;
  gobject_class->set_property = gst_gl_redaction_set_property;
  gobject_class->get_property = gst_gl_redaction_get_property;

  GST_GL_BASE_FILTER_CLASS (klass)->gl_start = gst_gl_redaction_gl_start;
  GST_GL_BASE_FILTER_CLASS (klass)->gl_stop = gst_gl_redaction_gl_stop;

  GST_GL_FILTER_CLASS (klass)->set_caps = gst_gl_redaction_set_caps;
  GST_GL_FILTER_CLASS (klass)->filter_texture = gst_gl_redaction_filter_texture;

  GST_BASE_TRANSFORM_CLASS (klass)->before_transform =
      GST_DEBUG_FUNCPTR (gst_gl_redaction_before_transform);

  g_object_class_install_property (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "location",
          "Location of image file to redaction", NULL, (GParamFlags)(GST_PARAM_CONTROLLABLE
          | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE
          | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_OFFSET_X,
      g_param_spec_int ("offset-x", "X Offset",
          "For positive value, horizontal offset of redaction image in pixels from"
          " left of video image. For negative value, horizontal offset of redaction"
          " image in pixels from right of video image", G_MININT, G_MAXINT, 0,
          (GParamFlags)(GST_PARAM_CONTROLLABLE | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE
          | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_OFFSET_Y,
      g_param_spec_int ("offset-y", "Y Offset",
          "For positive value, vertical offset of redaction image in pixels from"
          " top of video image. For negative value, vertical offset of redaction"
          " image in pixels from bottom of video image", G_MININT, G_MAXINT, 0,
          (GParamFlags)(GST_PARAM_CONTROLLABLE | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE
          | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_RELATIVE_X,
      g_param_spec_double ("relative-x", "Relative X Offset",
          "Horizontal offset of redaction image in fractions of video image "
          "width, from top-left corner of video image", 0.0, 1.0, 0.0,
          (GParamFlags)(GST_PARAM_CONTROLLABLE | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE
          | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_RELATIVE_Y,
      g_param_spec_double ("relative-y", "Relative Y Offset",
          "Vertical offset of redaction image in fractions of video image "
          "height, from top-left corner of video image", 0.0, 1.0, 0.0,
          (GParamFlags)(GST_PARAM_CONTROLLABLE | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE
          | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_REDACTION_WIDTH,
      g_param_spec_int ("overlay-width", "Redaction Width",
          "Width of redaction image in pixels (0 = same as redaction image)", 0,
          G_MAXINT, 0,
          (GParamFlags)(GST_PARAM_CONTROLLABLE | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE
          | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_REDACTION_HEIGHT,
      g_param_spec_int ("overlay-height", "Redaction Height",
          "Height of redaction image in pixels (0 = same as redaction image)", 0,
          G_MAXINT, 0,
          (GParamFlags)(GST_PARAM_CONTROLLABLE | GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE
          | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_ALPHA,
      g_param_spec_double ("alpha", "Alpha", "Global alpha of redaction image",
          0.0, 1.0, 1.0, (GParamFlags)(GST_PARAM_CONTROLLABLE | GST_PARAM_MUTABLE_PLAYING
          | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  gst_element_class_set_static_metadata (element_class,
      "Gstreamer OpenGL Redaction", "Filter/Effect/Video",
      "Redaction GL video texture with a JPEG/PNG image",
      "Filippo Argiolas <filippo.argiolas@gmail.com>, "
      "Matthew Waters <matthew@centricular.com>");

  GST_GL_BASE_FILTER_CLASS (klass)->supported_gl_api = (GstGLAPI)
      (GST_GL_API_OPENGL | GST_GL_API_GLES2 | GST_GL_API_OPENGL3);
}

static void
gst_gl_redaction_init (GstGLRedaction * redaction)
{
  redaction->offset_x = 0;
  redaction->offset_y = 0;

  redaction->relative_x = 0.0;
  redaction->relative_y = 0.0;

  redaction->overlay_width = 0;
  redaction->overlay_height = 0;

  redaction->alpha = 1.0;
}

static void
gst_gl_redaction_finalize (GObject * object)
{
  GstGLRedaction *redaction = GST_GL_REDACTION (object);

  g_free (redaction->location);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_gl_redaction_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGLRedaction *redaction = GST_GL_REDACTION (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_free (redaction->location);
      redaction->location_has_changed = TRUE;
      redaction->location = g_value_dup_string (value);
      break;
    case PROP_OFFSET_X:
      redaction->offset_x = g_value_get_int (value);
      redaction->geometry_change = TRUE;
      break;
    case PROP_OFFSET_Y:
      redaction->offset_y = g_value_get_int (value);
      redaction->geometry_change = TRUE;
      break;
    case PROP_RELATIVE_X:
      redaction->relative_x = g_value_get_double (value);
      redaction->geometry_change = TRUE;
      break;
    case PROP_RELATIVE_Y:
      redaction->relative_y = g_value_get_double (value);
      redaction->geometry_change = TRUE;
      break;
    case PROP_REDACTION_WIDTH:
      redaction->overlay_width = g_value_get_int (value);
      redaction->geometry_change = TRUE;
      break;
    case PROP_REDACTION_HEIGHT:
      redaction->overlay_height = g_value_get_int (value);
      redaction->geometry_change = TRUE;
      break;
    case PROP_ALPHA:
      redaction->alpha = g_value_get_double (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_redaction_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGLRedaction *redaction = GST_GL_REDACTION (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_value_set_string (value, redaction->location);
      break;
    case PROP_OFFSET_X:
      g_value_set_int (value, redaction->offset_x);
      break;
    case PROP_OFFSET_Y:
      g_value_set_int (value, redaction->offset_y);
      break;
    case PROP_RELATIVE_X:
      g_value_set_double (value, redaction->relative_x);
      break;
    case PROP_RELATIVE_Y:
      g_value_set_double (value, redaction->relative_y);
      break;
    case PROP_REDACTION_WIDTH:
      g_value_set_int (value, redaction->overlay_width);
      break;
    case PROP_REDACTION_HEIGHT:
      g_value_set_int (value, redaction->overlay_height);
      break;
    case PROP_ALPHA:
      g_value_set_double (value, redaction->alpha);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_gl_redaction_set_caps (GstGLFilter * filter, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstGLRedaction *redaction = GST_GL_REDACTION (filter);
  GstStructure *s = gst_caps_get_structure (incaps, 0);
  gint width = 0;
  gint height = 0;

  gst_structure_get_int (s, "width", &width);
  gst_structure_get_int (s, "height", &height);

  redaction->window_width = width;
  redaction->window_height = height;

  return TRUE;
}

static void
_unbind_buffer (GstGLRedaction * redaction)
{
  GstGLFilter *filter = GST_GL_FILTER (redaction);
  const GstGLFuncs *gl = GST_GL_BASE_FILTER (redaction)->context->gl_vtable;

  gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
  gl->BindBuffer (GL_ARRAY_BUFFER, 0);

  gl->DisableVertexAttribArray (filter->draw_attr_position_loc);
  gl->DisableVertexAttribArray (filter->draw_attr_texture_loc);
}

static void
_bind_buffer (GstGLRedaction * redaction, GLuint vbo)
{
  GstGLFilter *filter = GST_GL_FILTER (redaction);
  const GstGLFuncs *gl = GST_GL_BASE_FILTER (redaction)->context->gl_vtable;

  gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, redaction->vbo_indices);
  gl->BindBuffer (GL_ARRAY_BUFFER, vbo);

  gl->EnableVertexAttribArray (filter->draw_attr_position_loc);
  gl->EnableVertexAttribArray (filter->draw_attr_texture_loc);

  gl->VertexAttribPointer (filter->draw_attr_position_loc, 3, GL_FLOAT,
      GL_FALSE, 5 * sizeof (GLfloat), (void *) 0);
  gl->VertexAttribPointer (filter->draw_attr_texture_loc, 2, GL_FLOAT,
      GL_FALSE, 5 * sizeof (GLfloat), (void *) (3 * sizeof (GLfloat)));
}

// /* *INDENT-OFF* */
// float v_vertices[] = {
// /*|      Vertex     | TexCoord  |*/
//   -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
//    1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
//    1.0f,  1.0f, 0.0f, 1.0f, 1.0f,
//   -1.0f,  1.0f, 0.0f, 0.0f, 1.0f,
// };

static const GLushort indices[] = { 0, 1, 2, 0, 2, 3, };
/* *INDENT-ON* */

static gboolean
gst_gl_redaction_callback (GstGLFilter * filter, GstGLMemory * in_tex,
    gpointer stuff)
{

  /* *INDENT-OFF* */
  float vertices[] = {
    -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
    1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
    1.0f,  1.0f, 0.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0,  1.0f,
  };
  /* *INDENT-ON* */

  GstGLRedaction *redaction = GST_GL_REDACTION (filter);
  GstMapInfo map_info;
  guint image_tex;
  gboolean memory_mapped = FALSE;
  const GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;
  gboolean ret = FALSE;

#if GST_GL_HAVE_OPENGL
  if (gst_gl_context_get_gl_api (GST_GL_BASE_FILTER (filter)->context) &
      GST_GL_API_OPENGL) {

    gl->MatrixMode (GL_PROJECTION);
    gl->LoadIdentity ();
  }
#endif

  gl->ActiveTexture (GL_TEXTURE0);
  gl->BindTexture (GL_TEXTURE_2D, gst_gl_memory_get_texture_id (in_tex));

  gst_gl_shader_use (redaction->shader);

  gst_gl_shader_set_uniform_1f (redaction->shader, "alpha", 1.0f);
  gst_gl_shader_set_uniform_1i (redaction->shader, "texture", 0);

  filter->draw_attr_position_loc =
      gst_gl_shader_get_attribute_location (redaction->shader, "a_position");
  filter->draw_attr_texture_loc =
      gst_gl_shader_get_attribute_location (redaction->shader, "a_texcoord");

  gst_gl_filter_draw_fullscreen_quad (filter);

  if (!redaction->image_memory)
    goto out;

  if (!gst_memory_map ((GstMemory *) redaction->image_memory, &map_info,
          (GstMapFlags)(GST_MAP_READ | GST_MAP_GL)) || map_info.data == NULL)
    goto out;

  memory_mapped = TRUE;
  image_tex = *(guint *) map_info.data;

  if (!redaction->redaction_vbo) {
    if (gl->GenVertexArrays) {
      gl->GenVertexArrays (1, &redaction->redaction_vao);
      gl->BindVertexArray (redaction->redaction_vao);
    }

    gl->GenBuffers (1, &redaction->vbo_indices);
    gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, redaction->vbo_indices);
    gl->BufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof (indices), indices,
        GL_STATIC_DRAW);

    gl->GenBuffers (1, &redaction->redaction_vbo);
    gl->BindBuffer (GL_ARRAY_BUFFER, redaction->redaction_vbo);
    gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, redaction->vbo_indices);
    redaction->geometry_change = TRUE;
  }

  if (gl->GenVertexArrays) {
    gl->BindVertexArray (redaction->redaction_vao);
  }

  _bind_buffer (redaction, redaction->redaction_vbo);

  gint render_width, render_height;
  gfloat x, y, image_width, image_height;

  gl->BindTexture (GL_TEXTURE_2D, image_tex);
  gst_gl_shader_set_uniform_1f (redaction->shader, "alpha", redaction->alpha);

  gl->Enable (GL_BLEND);
  if (gl->BlendFuncSeparate)
    gl->BlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
        GL_ONE_MINUS_SRC_ALPHA);
  else
    gl->BlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl->BlendEquation (GL_FUNC_ADD);

  // updates to redactions could be delivered from a different CV based element in the pipeline and a different framerate
  if (redaction->frame_count++ % _GstGLRedaction::REDACTION_ROTATE_THRESHOLD == 0) 
  {
    std::memcpy(redaction->prev_redactions, redaction->redactions, sizeof(redaction->redactions));
    for (int i = 0; i < _GstGLRedaction::NUM_REDACTIONS; ++i) 
    {
      int x = -500 + rand()%(500 + redaction->window_width);
      int y = -500 + rand()%(500 + redaction->window_height);
      redaction->redactions[i] = {x, y, x - 50 + rand()%200,  y - 50 + rand()%200};
    }
  }

  for (int i = 0; i < _GstGLRedaction::NUM_REDACTIONS; ++i) 
  {
    float interp = (redaction->frame_count % _GstGLRedaction::REDACTION_ROTATE_THRESHOLD) / (float)_GstGLRedaction::REDACTION_ROTATE_THRESHOLD;
    x = redaction->prev_redactions[i].left + (redaction->redactions[i].left - redaction->prev_redactions[i].left)*interp;
    y = redaction->prev_redactions[i].top + (redaction->redactions[i].top - redaction->prev_redactions[i].top)*interp;

    render_width = redaction->prev_redactions[i].right + (redaction->redactions[i].right - redaction->prev_redactions[i].right)*interp - x;
    render_height = redaction->prev_redactions[i].bottom + (redaction->redactions[i].bottom - redaction->prev_redactions[i].bottom)*interp - y;

    RedactionBox to_check = {(gint)x,(gint)y, (gint)(x + render_width), (gint)(y + render_height)};
    to_check = redaction->clamp_to_window(to_check);
    if (!to_check.is_valid())
      continue;

    /* scale from [0, 1] -> [-1, 1] */
    x = (x / (gfloat) redaction->window_width) * 2.0f - 1.0;
    y = (y / (gfloat) redaction->window_height) * 2.0f - 1.0;
    /* scale from [0, 1] -> [0, 2] */
    image_width =
        ((gfloat) render_width / (gfloat) redaction->window_width) * 2.0f;
    image_height =
        ((gfloat) render_height / (gfloat) redaction->window_height) * 2.0f;

    vertices[0] = vertices[15] = x;
    vertices[5] = vertices[10] = x + image_width;
    vertices[1] = vertices[6] = y;
    vertices[11] = vertices[16] = y + image_height;

    gl->BufferData (GL_ARRAY_BUFFER, 4 * 5 * sizeof (GLfloat), vertices,
        GL_STATIC_DRAW);

    gl->DrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
  }

  gl->Disable (GL_BLEND);
  ret = TRUE;

out:
  if (gl->GenVertexArrays)
    gl->BindVertexArray (0);
  else
    _unbind_buffer (redaction);

  gst_gl_context_clear_shader (GST_GL_BASE_FILTER (filter)->context);

  if (memory_mapped)
    gst_memory_unmap ((GstMemory *) redaction->image_memory, &map_info);

  redaction->geometry_change = FALSE;

  return ret;
}

static gboolean
load_file (GstGLRedaction * redaction)
{
  FILE *fp;
  guint8 buff[16];
  gsize n_read;
  GstCaps *caps;
  GstStructure *structure;
  gboolean success = FALSE;

  if (redaction->location == NULL)
    return TRUE;

  if ((fp = fopen (redaction->location, "rb")) == NULL) {
    GST_ELEMENT_ERROR (redaction, RESOURCE, NOT_FOUND, ("Can't open file"),
        ("File: %s", redaction->location));
    return FALSE;
  }

  n_read = fread (buff, 1, sizeof (buff), fp);
  if (n_read != sizeof (buff)) {
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE, ("Can't read file header"),
        ("File: %s", redaction->location));
    goto out;
  }

  caps = gst_type_find_helper_for_data (GST_OBJECT (redaction), buff,
      sizeof (buff), NULL);

  if (caps == NULL) {
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE, ("Can't find file type"),
        ("File: %s", redaction->location));
    goto out;
  }

  fseek (fp, 0, SEEK_SET);

  structure = gst_caps_get_structure (caps, 0);
  if (gst_structure_has_name (structure, "image/jpeg")) {
    success = gst_gl_redaction_load_jpeg (redaction, fp);
  } else if (gst_structure_has_name (structure, "image/png")) {
    success = gst_gl_redaction_load_png (redaction, fp);
  } else {
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE, ("Image type not supported"),
        ("File: %s", redaction->location));
  }

out:
  fclose (fp);
  gst_caps_replace (&caps, NULL);

  return success;
}

static gboolean
gst_gl_redaction_filter_texture (GstGLFilter * filter, GstGLMemory * in_tex,
    GstGLMemory * out_tex)
{
  GstGLRedaction *redaction = GST_GL_REDACTION (filter);

  if (redaction->location_has_changed) {
    if (redaction->image_memory) {
      gst_memory_unref ((GstMemory *) redaction->image_memory);
      redaction->image_memory = NULL;
    }

    if (!load_file (redaction))
      return FALSE;

    redaction->location_has_changed = FALSE;
  }

  gst_gl_filter_render_to_target (filter, in_tex, out_tex,
      gst_gl_redaction_callback, redaction);

  return TRUE;
}

static void
gst_gl_redaction_before_transform (GstBaseTransform * trans, GstBuffer * outbuf)
{
  GstClockTime stream_time;

  stream_time = gst_segment_to_stream_time (&trans->segment, GST_FORMAT_TIME,
      GST_BUFFER_TIMESTAMP (outbuf));

  if (GST_CLOCK_TIME_IS_VALID (stream_time))
    gst_object_sync_values (GST_OBJECT (trans), stream_time);
}

static void
user_warning_fn (png_structp png_ptr, png_const_charp warning_msg)
{
  g_warning ("%s\n", warning_msg);
}

static gboolean
gst_gl_redaction_load_jpeg (GstGLRedaction * redaction, FILE * fp)
{
  GstGLBaseMemoryAllocator *mem_allocator;
  GstGLVideoAllocationParams *params;
  GstVideoInfo v_info;
  GstVideoAlignment v_align;
  GstMapInfo map_info;
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  JSAMPROW j;
  int i;

  jpeg_create_decompress (&cinfo);
  cinfo.err = jpeg_std_error (&jerr);
  jpeg_stdio_src (&cinfo, fp);
  jpeg_read_header (&cinfo, TRUE);
  jpeg_start_decompress (&cinfo);
  redaction->image_width = cinfo.image_width;
  redaction->image_height = cinfo.image_height;

  if (cinfo.num_components == 1)
    gst_video_info_set_format (&v_info, GST_VIDEO_FORMAT_Y444,
        redaction->image_width, redaction->image_height);
  else
    gst_video_info_set_format (&v_info, GST_VIDEO_FORMAT_RGB,
        redaction->image_width, redaction->image_height);

  gst_video_alignment_reset (&v_align);
  v_align.stride_align[0] = 32 - 1;
  gst_video_info_align (&v_info, &v_align);

  mem_allocator =
      GST_GL_BASE_MEMORY_ALLOCATOR (gst_gl_memory_allocator_get_default
      (GST_GL_BASE_FILTER (redaction)->context));
  params =
      gst_gl_video_allocation_params_new (GST_GL_BASE_FILTER (redaction)->context,
      NULL, &v_info, 0, &v_align, GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA);
  redaction->image_memory = (GstGLMemory *)
      gst_gl_base_memory_alloc (mem_allocator,
      (GstGLAllocationParams *) params);
  gst_gl_allocation_params_free ((GstGLAllocationParams *) params);
  gst_object_unref (mem_allocator);

  if (!gst_memory_map ((GstMemory *) redaction->image_memory, &map_info,
          GST_MAP_WRITE)) {
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE, ("failed to map memory"),
        ("File: %s", redaction->location));
    return FALSE;
  }

  for (i = 0; i < redaction->image_height; ++i) {
    j = map_info.data + v_info.stride[0] * i;
    jpeg_read_scanlines (&cinfo, &j, 1);
  }
  jpeg_finish_decompress (&cinfo);
  jpeg_destroy_decompress (&cinfo);
  gst_memory_unmap ((GstMemory *) redaction->image_memory, &map_info);

  return TRUE;
}

static gboolean
gst_gl_redaction_load_png (GstGLRedaction * redaction, FILE * fp)
{
  GstGLBaseMemoryAllocator *mem_allocator;
  GstGLVideoAllocationParams *params;
  GstVideoInfo v_info;
  GstMapInfo map_info;

  png_structp png_ptr;
  png_infop info_ptr;
  png_uint_32 width = 0;
  png_uint_32 height = 0;
  gint bit_depth = 0;
  gint color_type = 0;
  gint interlace_type = 0;
  guint y = 0;
  guchar **rows = NULL;
  gint filler;
  png_byte magic[8];
  gint n_read;

  if (!GST_GL_BASE_FILTER (redaction)->context)
    return FALSE;

  /* Read magic number */
  n_read = fread (magic, 1, sizeof (magic), fp);
  if (n_read != sizeof (magic)) {
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE,
        ("can't read PNG magic number"), ("File: %s", redaction->location));
    return FALSE;
  }

  /* Check for valid magic number */
  if (png_sig_cmp (magic, 0, sizeof (magic))) {
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE,
        ("not a valid PNG image"), ("File: %s", redaction->location));
    return FALSE;
  }

  png_ptr = png_create_read_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

  if (png_ptr == NULL) {
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE,
        ("failed to initialize the png_struct"), ("File: %s",
            redaction->location));
    return FALSE;
  }

  png_set_error_fn (png_ptr, NULL, NULL, user_warning_fn);

  info_ptr = png_create_info_struct (png_ptr);
  if (info_ptr == NULL) {
    png_destroy_read_struct (&png_ptr, png_infopp_NULL, png_infopp_NULL);
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE,
        ("failed to initialize the memory for image information"),
        ("File: %s", redaction->location));
    return FALSE;
  }

  png_init_io (png_ptr, fp);

  png_set_sig_bytes (png_ptr, sizeof (magic));

  png_read_info (png_ptr, info_ptr);

  png_get_IHDR (png_ptr, info_ptr, &width, &height, &bit_depth, &color_type,
      &interlace_type, int_p_NULL, int_p_NULL);

  if (color_type == PNG_COLOR_TYPE_RGB) {
    filler = 0xff;
    png_set_filler (png_ptr, filler, PNG_FILLER_AFTER);
    color_type = PNG_COLOR_TYPE_RGB_ALPHA;
  }

  if (color_type != PNG_COLOR_TYPE_RGB_ALPHA) {
    png_destroy_read_struct (&png_ptr, png_infopp_NULL, png_infopp_NULL);
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE,
        ("color type is not rgb"), ("File: %s", redaction->location));
    return FALSE;
  }

  redaction->image_width = width;
  redaction->image_height = height;

  gst_video_info_set_format (&v_info, GST_VIDEO_FORMAT_RGBA, width, height);
  mem_allocator =
      GST_GL_BASE_MEMORY_ALLOCATOR (gst_gl_memory_allocator_get_default
      (GST_GL_BASE_FILTER (redaction)->context));
  params =
      gst_gl_video_allocation_params_new (GST_GL_BASE_FILTER (redaction)->context,
      NULL, &v_info, 0, NULL, GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA);
  redaction->image_memory = (GstGLMemory *)
      gst_gl_base_memory_alloc (mem_allocator,
      (GstGLAllocationParams *) params);
  gst_gl_allocation_params_free ((GstGLAllocationParams *) params);
  gst_object_unref (mem_allocator);

  if (!gst_memory_map ((GstMemory *) redaction->image_memory, &map_info,
          GST_MAP_WRITE)) {
    png_destroy_read_struct (&png_ptr, &info_ptr, png_infopp_NULL);
    GST_ELEMENT_ERROR (redaction, STREAM, DECODE,
        ("failed to map memory"), ("File: %s", redaction->location));
    return FALSE;
  }
  rows = (guchar **) malloc (sizeof (guchar *) * height);

  for (y = 0; y < height; ++y)
    rows[y] = (guchar *) (map_info.data + y * width * 4);

  png_read_image (png_ptr, rows);

  free (rows);
  gst_memory_unmap ((GstMemory *) redaction->image_memory, &map_info);

  png_read_end (png_ptr, info_ptr);
  png_destroy_read_struct (&png_ptr, &info_ptr, png_infopp_NULL);

  return TRUE;
}

RedactionBox _GstGLRedaction::clamp_to_window(RedactionBox redaction) const 
{
  RedactionBox clamped_redaction = {0,0, window_width, window_height};
  clamped_redaction = clamped_redaction.intersection(redaction);
  return clamped_redaction;
}
