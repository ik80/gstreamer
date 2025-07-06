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

#ifndef _GST_GL_REDACTION_H_
#define _GST_GL_REDACTION_H_

#include <gst/gl/gstglfilter.h>
#include <gst/gl/gstglfuncs.h>

#include <memory>
#include <mutex>
#include <algorithm>
#include <vector>
#include <thread>

G_BEGIN_DECLS

#define GST_TYPE_GL_REDACTION (gst_gl_redaction_get_type())
#define GST_GL_REDACTION(obj)                                                  \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_GL_REDACTION, GstGLRedaction))
#define GST_IS_GL_REDACTION(obj)                                               \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_GL_REDACTION))
#define GST_GL_REDACTION_CLASS(klass)                                          \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_GL_REDACTION, GstGLRedactionClass))
#define GST_IS_GL_REDACTION_CLASS(klass)                                       \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_GL_REDACTION))
#define GST_GL_REDACTION_GET_CLASS(obj)                                        \
  (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_GL_REDACTION, GstGLRedactionClass))

typedef struct _GstGLRedaction GstGLRedaction;
typedef struct _GstGLRedactionClass GstGLRedactionClass;

struct Rectangle {
  int left;
  int top;
  int right;
  int bottom;

  // Check if this rectangle intersects with another
  bool intersects(const Rectangle &other) const {
    return left < other.right && right > other.left && top < other.bottom &&
           bottom > other.top;
  }

  // Calculate intersection with another rectangle
  // Returns empty rectangle if no intersection
  Rectangle intersection(const Rectangle &other) const {
    Rectangle result;
    result.left = std::max(left, other.left);
    result.top = std::max(top, other.top);
    result.right = std::min(right, other.right);
    result.bottom = std::min(bottom, other.bottom);

    // Handle no intersection case
    if (result.left >= result.right || result.top >= result.bottom) {
      result = {0, 0, 0, 0}; // Empty rectangle
    }

    return result;
  }

  // Check if rectangle is valid
  bool is_valid() const { return !(left >= right || top >= bottom); }

  bool operator==(const Rectangle &other) const {
    return left == other.left && top == other.top && right == other.right &&
           bottom == other.bottom;
  }
};

using RedactionBox = Rectangle;

// Helper function to check if rectangle is empty

struct _GstGLRedaction {
public:
  GstGLFilter filter;

  /* properties */
  gchar *location;
  gint offset_x;
  gint offset_y;

  gdouble relative_x;
  gdouble relative_y;

  gint overlay_width;
  gint overlay_height;

  gdouble alpha;

  GstGLShader *shader;
  GstGLMemory *image_memory;

  gboolean location_has_changed;
  gint window_width, window_height;
  gint image_width, image_height;

  gboolean geometry_change;

  GLuint vao;
  GLuint redaction_vao;
  GLuint vbo;
  GLuint redaction_vbo;
  GLuint vbo_indices;
  GLuint attr_position;
  GLuint attr_texture;
  RedactionBox clamp_to_window(RedactionBox redaction) const;

  static const size_t NUM_REDACTIONS = 1000;
  RedactionBox redactions[NUM_REDACTIONS];
  RedactionBox prev_redactions[NUM_REDACTIONS];
  static const size_t REDACTION_ROTATE_THRESHOLD = 300;
  int frame_count = 0;
};

struct _GstGLRedactionClass {
  GstGLFilterClass filter_class;
};

GType gst_gl_redaction_get_type(void);

G_END_DECLS

#endif /* _GST_GL_REDACTION_H_ */
