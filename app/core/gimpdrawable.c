/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * This program is free software: you can redistribute it and/or modify
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <cairo.h>
#include <gegl.h>
#include <gegl-plugin.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "libgimpbase/gimpbase.h"
#include "libgimpcolor/gimpcolor.h"

#include "core-types.h"

#include "gegl/gimp-babl.h"
#include "gegl/gimp-gegl-nodes.h"
#include "gegl/gimp-gegl-utils.h"

#include "gimp-utils.h"
#include "gimpchannel.h"
#include "gimpcontext.h"
#include "gimpdrawable-combine.h"
#include "gimpdrawable-operation.h"
#include "gimpdrawable-preview.h"
#include "gimpdrawable-private.h"
#include "gimpdrawable-shadow.h"
#include "gimpdrawable-transform.h"
#include "gimpimage.h"
#include "gimpimage-colormap.h"
#include "gimpimage-undo-push.h"
#include "gimplayer.h"
#include "gimpmarshal.h"
#include "gimppattern.h"
#include "gimppickable.h"
#include "gimppreviewcache.h"
#include "gimpprogress.h"

#include "gimp-log.h"

#include "gimp-intl.h"


enum
{
  UPDATE,
  ALPHA_CHANGED,
  LAST_SIGNAL
};


/*  local function prototypes  */

static void  gimp_drawable_pickable_iface_init (GimpPickableInterface *iface);

static void       gimp_drawable_dispose            (GObject           *object);
static void       gimp_drawable_finalize           (GObject           *object);

static gint64     gimp_drawable_get_memsize        (GimpObject        *object,
                                                    gint64            *gui_size);

static gboolean   gimp_drawable_get_size           (GimpViewable      *viewable,
                                                    gint              *width,
                                                    gint              *height);
static void       gimp_drawable_invalidate_preview (GimpViewable      *viewable);

static void       gimp_drawable_removed            (GimpItem          *item);
static void       gimp_drawable_visibility_changed (GimpItem          *item);
static GimpItem * gimp_drawable_duplicate          (GimpItem          *item,
                                                    GType              new_type);
static void       gimp_drawable_scale              (GimpItem          *item,
                                                    gint               new_width,
                                                    gint               new_height,
                                                    gint               new_offset_x,
                                                    gint               new_offset_y,
                                                    GimpInterpolationType interp_type,
                                                    GimpProgress      *progress);
static void       gimp_drawable_resize             (GimpItem          *item,
                                                    GimpContext       *context,
                                                    gint               new_width,
                                                    gint               new_height,
                                                    gint               offset_x,
                                                    gint               offset_y);
static void       gimp_drawable_flip               (GimpItem          *item,
                                                    GimpContext       *context,
                                                    GimpOrientationType  flip_type,
                                                    gdouble            axis,
                                                    gboolean           clip_result);
static void       gimp_drawable_rotate             (GimpItem          *item,
                                                    GimpContext       *context,
                                                    GimpRotationType   rotate_type,
                                                    gdouble            center_x,
                                                    gdouble            center_y,
                                                    gboolean           clip_result);
static void       gimp_drawable_transform          (GimpItem          *item,
                                                    GimpContext       *context,
                                                    const GimpMatrix3 *matrix,
                                                    GimpTransformDirection direction,
                                                    GimpInterpolationType interpolation_type,
                                                    gint               recursion_level,
                                                    GimpTransformResize clip_result,
                                                    GimpProgress      *progress);

static gboolean   gimp_drawable_get_pixel_at       (GimpPickable      *pickable,
                                                    gint               x,
                                                    gint               y,
                                                    const Babl        *format,
                                                    gpointer           pixel);
static void       gimp_drawable_real_update        (GimpDrawable      *drawable,
                                                    gint               x,
                                                    gint               y,
                                                    gint               width,
                                                    gint               height);

static gint64  gimp_drawable_real_estimate_memsize (const GimpDrawable *drawable,
                                                    gint               width,
                                                    gint               height);

static void       gimp_drawable_real_convert_type  (GimpDrawable      *drawable,
                                                    GimpImage         *dest_image,
                                                    GimpImageBaseType  new_base_type,
                                                    GimpPrecision      new_precision,
                                                    gboolean           push_undo);

static GeglBuffer * gimp_drawable_real_get_buffer  (GimpDrawable      *drawable);
static void       gimp_drawable_real_set_buffer    (GimpDrawable      *drawable,
                                                    gboolean           push_undo,
                                                    const gchar       *undo_desc,
                                                    GeglBuffer        *buffer,
                                                    gint               offset_x,
                                                    gint               offset_y);
static GeglNode * gimp_drawable_get_node           (GimpItem          *item);

static void       gimp_drawable_real_push_undo     (GimpDrawable      *drawable,
                                                    const gchar       *undo_desc,
                                                    GeglBuffer        *buffer,
                                                    gint               x,
                                                    gint               y,
                                                    gint               width,
                                                    gint               height);
static void       gimp_drawable_real_swap_pixels   (GimpDrawable      *drawable,
                                                    GeglBuffer        *buffer,
                                                    gint               x,
                                                    gint               y);

static void       gimp_drawable_sync_source_node   (GimpDrawable      *drawable,
                                                    gboolean           detach_fs);
static void       gimp_drawable_fs_notify          (GimpLayer         *fs,
                                                    const GParamSpec  *pspec,
                                                    GimpDrawable      *drawable);
static void       gimp_drawable_fs_update          (GimpLayer         *fs,
                                                    gint               x,
                                                    gint               y,
                                                    gint               width,
                                                    gint               height,
                                                    GimpDrawable      *drawable);


G_DEFINE_TYPE_WITH_CODE (GimpDrawable, gimp_drawable, GIMP_TYPE_ITEM,
                         G_IMPLEMENT_INTERFACE (GIMP_TYPE_PICKABLE,
                                                gimp_drawable_pickable_iface_init))

#define parent_class gimp_drawable_parent_class

static guint gimp_drawable_signals[LAST_SIGNAL] = { 0 };


static void
gimp_drawable_class_init (GimpDrawableClass *klass)
{
  GObjectClass      *object_class      = G_OBJECT_CLASS (klass);
  GimpObjectClass   *gimp_object_class = GIMP_OBJECT_CLASS (klass);
  GimpViewableClass *viewable_class    = GIMP_VIEWABLE_CLASS (klass);
  GimpItemClass     *item_class        = GIMP_ITEM_CLASS (klass);

  gimp_drawable_signals[UPDATE] =
    g_signal_new ("update",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  G_STRUCT_OFFSET (GimpDrawableClass, update),
                  NULL, NULL,
                  gimp_marshal_VOID__INT_INT_INT_INT,
                  G_TYPE_NONE, 4,
                  G_TYPE_INT,
                  G_TYPE_INT,
                  G_TYPE_INT,
                  G_TYPE_INT);

  gimp_drawable_signals[ALPHA_CHANGED] =
    g_signal_new ("alpha-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  G_STRUCT_OFFSET (GimpDrawableClass, alpha_changed),
                  NULL, NULL,
                  gimp_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  object_class->dispose              = gimp_drawable_dispose;
  object_class->finalize             = gimp_drawable_finalize;

  gimp_object_class->get_memsize     = gimp_drawable_get_memsize;

  viewable_class->get_size           = gimp_drawable_get_size;
  viewable_class->invalidate_preview = gimp_drawable_invalidate_preview;
  viewable_class->get_preview        = gimp_drawable_get_preview;

  item_class->removed                = gimp_drawable_removed;
  item_class->visibility_changed     = gimp_drawable_visibility_changed;
  item_class->duplicate              = gimp_drawable_duplicate;
  item_class->scale                  = gimp_drawable_scale;
  item_class->resize                 = gimp_drawable_resize;
  item_class->flip                   = gimp_drawable_flip;
  item_class->rotate                 = gimp_drawable_rotate;
  item_class->transform              = gimp_drawable_transform;
  item_class->get_node               = gimp_drawable_get_node;

  klass->update                      = gimp_drawable_real_update;
  klass->alpha_changed               = NULL;
  klass->estimate_memsize            = gimp_drawable_real_estimate_memsize;
  klass->invalidate_boundary         = NULL;
  klass->get_active_components       = NULL;
  klass->get_active_mask             = NULL;
  klass->convert_type                = gimp_drawable_real_convert_type;
  klass->apply_buffer                = gimp_drawable_real_apply_buffer;
  klass->replace_buffer              = gimp_drawable_real_replace_buffer;
  klass->get_buffer                  = gimp_drawable_real_get_buffer;
  klass->set_buffer                  = gimp_drawable_real_set_buffer;
  klass->push_undo                   = gimp_drawable_real_push_undo;
  klass->swap_pixels                 = gimp_drawable_real_swap_pixels;

  g_type_class_add_private (klass, sizeof (GimpDrawablePrivate));
}

static void
gimp_drawable_init (GimpDrawable *drawable)
{
  drawable->private = G_TYPE_INSTANCE_GET_PRIVATE (drawable,
                                                   GIMP_TYPE_DRAWABLE,
                                                   GimpDrawablePrivate);
}

/* sorry for the evil casts */

static void
gimp_drawable_pickable_iface_init (GimpPickableInterface *iface)
{
  iface->get_image             = (GimpImage     * (*) (GimpPickable *pickable)) gimp_item_get_image;
  iface->get_format            = (const Babl    * (*) (GimpPickable *pickable)) gimp_drawable_get_format;
  iface->get_format_with_alpha = (const Babl    * (*) (GimpPickable *pickable)) gimp_drawable_get_format_with_alpha;
  iface->get_buffer            = (GeglBuffer    * (*) (GimpPickable *pickable)) gimp_drawable_get_buffer;
  iface->get_pixel_at          = gimp_drawable_get_pixel_at;
}

static void
gimp_drawable_dispose (GObject *object)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (object);

  if (gimp_drawable_get_floating_sel (drawable))
    gimp_drawable_detach_floating_sel (drawable);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gimp_drawable_finalize (GObject *object)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (object);

  if (drawable->private->buffer)
    {
      g_object_unref (drawable->private->buffer);
      drawable->private->buffer = NULL;
    }

  gimp_drawable_free_shadow_buffer (drawable);

  if (drawable->private->source_node)
    {
      g_object_unref (drawable->private->source_node);
      drawable->private->source_node = NULL;
    }

  if (drawable->private->preview_cache)
    gimp_preview_cache_invalidate (&drawable->private->preview_cache);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gint64
gimp_drawable_get_memsize (GimpObject *object,
                           gint64     *gui_size)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (object);
  gint64        memsize  = 0;

  memsize += gimp_gegl_buffer_get_memsize (gimp_drawable_get_buffer (drawable));
  memsize += gimp_gegl_buffer_get_memsize (drawable->private->shadow);

  *gui_size += gimp_preview_cache_get_memsize (drawable->private->preview_cache);

  return memsize + GIMP_OBJECT_CLASS (parent_class)->get_memsize (object,
                                                                  gui_size);
}

static gboolean
gimp_drawable_get_size (GimpViewable *viewable,
                        gint         *width,
                        gint         *height)
{
  GimpItem *item = GIMP_ITEM (viewable);

  *width  = gimp_item_get_width  (item);
  *height = gimp_item_get_height (item);

  return TRUE;
}

static void
gimp_drawable_invalidate_preview (GimpViewable *viewable)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (viewable);

  GIMP_VIEWABLE_CLASS (parent_class)->invalidate_preview (viewable);

  drawable->private->preview_valid = FALSE;

  if (drawable->private->preview_cache)
    gimp_preview_cache_invalidate (&drawable->private->preview_cache);
}

static void
gimp_drawable_removed (GimpItem *item)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (item);

  gimp_drawable_free_shadow_buffer (drawable);

  if (GIMP_ITEM_CLASS (parent_class)->removed)
    GIMP_ITEM_CLASS (parent_class)->removed (item);
}

static void
gimp_drawable_visibility_changed (GimpItem *item)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (item);
  GeglNode     *node;

  /*  don't use gimp_item_get_node() because that would create
   *  the node.
   */
  node = gimp_item_peek_node (item);

  if (node)
    {
      GeglNode *input  = gegl_node_get_input_proxy  (node, "input");
      GeglNode *output = gegl_node_get_output_proxy (node, "output");

      if (gimp_item_get_visible (item))
        {
          gegl_node_connect_to (input,                        "output",
                                drawable->private->mode_node, "input");
          gegl_node_connect_to (drawable->private->mode_node, "output",
                                output,                       "input");
        }
      else
        {
          gegl_node_disconnect (drawable->private->mode_node, "input");

          gegl_node_connect_to (input,  "output",
                                output, "input");
        }

      /* FIXME: chain up again when above floating sel special case is gone */
      return;
    }

  GIMP_ITEM_CLASS (parent_class)->visibility_changed (item);
}

static GimpItem *
gimp_drawable_duplicate (GimpItem *item,
                         GType     new_type)
{
  GimpItem *new_item;

  g_return_val_if_fail (g_type_is_a (new_type, GIMP_TYPE_DRAWABLE), NULL);

  new_item = GIMP_ITEM_CLASS (parent_class)->duplicate (item, new_type);

  if (GIMP_IS_DRAWABLE (new_item))
    {
      GimpDrawable  *drawable     = GIMP_DRAWABLE (item);
      GimpDrawable  *new_drawable = GIMP_DRAWABLE (new_item);

      if (new_drawable->private->buffer)
        g_object_unref (new_drawable->private->buffer);

      new_drawable->private->buffer =
        gimp_gegl_buffer_dup (gimp_drawable_get_buffer (drawable));
    }

  return new_item;
}

static void
gimp_drawable_scale (GimpItem              *item,
                     gint                   new_width,
                     gint                   new_height,
                     gint                   new_offset_x,
                     gint                   new_offset_y,
                     GimpInterpolationType  interpolation_type,
                     GimpProgress          *progress)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (item);
  GeglBuffer   *new_buffer;
  GeglNode     *scale;

  new_buffer = gimp_gegl_buffer_new (GEGL_RECTANGLE (0, 0,
                                                     new_width, new_height),
                                     gimp_drawable_get_format (drawable));

  scale = g_object_new (GEGL_TYPE_NODE,
                        "operation", "gegl:scale",
                        NULL);

  gegl_node_set (scale,
                 "origin-x",   0.0,
                 "origin-y",   0.0,
                 "filter",     gimp_interpolation_to_gegl_filter (interpolation_type),
                 "hard-edges", TRUE,
                 "x",          ((gdouble) new_width /
                                gimp_item_get_width  (item)),
                 "y",          ((gdouble) new_height /
                                gimp_item_get_height (item)),
                 NULL);

  gimp_drawable_apply_operation_to_buffer (drawable, progress,
                                           C_("undo-type", "Scale"),
                                           scale, new_buffer);
  g_object_unref (scale);

  gimp_drawable_set_buffer_full (drawable, gimp_item_is_attached (item), NULL,
                                 new_buffer,
                                 new_offset_x, new_offset_y);
  g_object_unref (new_buffer);
}

static void
gimp_drawable_resize (GimpItem    *item,
                      GimpContext *context,
                      gint         new_width,
                      gint         new_height,
                      gint         offset_x,
                      gint         offset_y)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (item);
  GeglBuffer   *new_buffer;
  gint          new_offset_x;
  gint          new_offset_y;
  gint          copy_x, copy_y;
  gint          copy_width, copy_height;

  /*  if the size doesn't change, this is a nop  */
  if (new_width  == gimp_item_get_width  (item) &&
      new_height == gimp_item_get_height (item) &&
      offset_x   == 0                       &&
      offset_y   == 0)
    return;

  new_offset_x = gimp_item_get_offset_x (item) - offset_x;
  new_offset_y = gimp_item_get_offset_y (item) - offset_y;

  gimp_rectangle_intersect (gimp_item_get_offset_x (item),
                            gimp_item_get_offset_y (item),
                            gimp_item_get_width (item),
                            gimp_item_get_height (item),
                            new_offset_x,
                            new_offset_y,
                            new_width,
                            new_height,
                            &copy_x,
                            &copy_y,
                            &copy_width,
                            &copy_height);

  new_buffer = gimp_gegl_buffer_new (GEGL_RECTANGLE (0, 0,
                                                     new_width, new_height),
                                     gimp_drawable_get_format (drawable));

  if (copy_width  != new_width ||
      copy_height != new_height)
    {
      /*  Clear the new tiles if needed  */

      GimpRGB    bg;
      GeglColor *col;

      if (! gimp_drawable_has_alpha (drawable) && ! GIMP_IS_CHANNEL (drawable))
        gimp_context_get_background (context, &bg);
      else
        gimp_rgba_set (&bg, 0.0, 0.0, 0.0, 0.0);

      col = gimp_gegl_color_new (&bg);

      gegl_buffer_set_color (new_buffer, NULL, col);
      g_object_unref (col);
    }

  if (copy_width && copy_height)
    {
      /*  Copy the pixels in the intersection  */
      gegl_buffer_copy (gimp_drawable_get_buffer (drawable),
                        GEGL_RECTANGLE (copy_x - gimp_item_get_offset_x (item),
                                        copy_y - gimp_item_get_offset_y (item),
                                        copy_width,
                                        copy_height),
                        new_buffer,
                        GEGL_RECTANGLE (copy_x - new_offset_x,
                                        copy_y - new_offset_y, 0, 0));
    }

  gimp_drawable_set_buffer_full (drawable, gimp_item_is_attached (item), NULL,
                                 new_buffer,
                                 new_offset_x, new_offset_y);
  g_object_unref (new_buffer);
}

static void
gimp_drawable_flip (GimpItem            *item,
                    GimpContext         *context,
                    GimpOrientationType  flip_type,
                    gdouble              axis,
                    gboolean             clip_result)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (item);
  GeglBuffer   *buffer;
  gint          off_x, off_y;
  gint          new_off_x, new_off_y;

  gimp_item_get_offset (item, &off_x, &off_y);

  buffer = gimp_drawable_transform_buffer_flip (drawable, context,
                                                gimp_drawable_get_buffer (drawable),
                                                off_x, off_y,
                                                flip_type, axis,
                                                clip_result,
                                                &new_off_x, &new_off_y);

  if (buffer)
    {
      gimp_drawable_transform_paste (drawable, buffer,
                                     new_off_x, new_off_y, FALSE);
      g_object_unref (buffer);
    }
}

static void
gimp_drawable_rotate (GimpItem         *item,
                      GimpContext      *context,
                      GimpRotationType  rotate_type,
                      gdouble           center_x,
                      gdouble           center_y,
                      gboolean          clip_result)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (item);
  GeglBuffer   *buffer;
  gint          off_x, off_y;
  gint          new_off_x, new_off_y;

  gimp_item_get_offset (item, &off_x, &off_y);

  buffer = gimp_drawable_transform_buffer_rotate (drawable, context,
                                                  gimp_drawable_get_buffer (drawable),
                                                  off_x, off_y,
                                                  rotate_type, center_x, center_y,
                                                  clip_result,
                                                  &new_off_x, &new_off_y);

  if (buffer)
    {
      gimp_drawable_transform_paste (drawable, buffer,
                                     new_off_x, new_off_y, FALSE);
      g_object_unref (buffer);
    }
}

static void
gimp_drawable_transform (GimpItem               *item,
                         GimpContext            *context,
                         const GimpMatrix3      *matrix,
                         GimpTransformDirection  direction,
                         GimpInterpolationType   interpolation_type,
                         gint                    recursion_level,
                         GimpTransformResize     clip_result,
                         GimpProgress           *progress)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (item);
  GeglBuffer   *buffer;
  gint          off_x, off_y;
  gint          new_off_x, new_off_y;

  gimp_item_get_offset (item, &off_x, &off_y);

  buffer = gimp_drawable_transform_buffer_affine (drawable, context,
                                                  gimp_drawable_get_buffer (drawable),
                                                  off_x, off_y,
                                                  matrix, direction,
                                                  interpolation_type,
                                                  recursion_level,
                                                  clip_result,
                                                  &new_off_x, &new_off_y,
                                                  progress);

  if (buffer)
    {
      gimp_drawable_transform_paste (drawable, buffer,
                                     new_off_x, new_off_y, FALSE);
      g_object_unref (buffer);
    }
}

static gboolean
gimp_drawable_get_pixel_at (GimpPickable *pickable,
                            gint          x,
                            gint          y,
                            const Babl   *format,
                            gpointer      pixel)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (pickable);

  /* do not make this a g_return_if_fail() */
  if (x < 0 || x >= gimp_item_get_width  (GIMP_ITEM (drawable)) ||
      y < 0 || y >= gimp_item_get_height (GIMP_ITEM (drawable)))
    return FALSE;

  gegl_buffer_sample (gimp_drawable_get_buffer (drawable),
                      x, y, NULL, pixel, format,
                      GEGL_SAMPLER_NEAREST, GEGL_ABYSS_NONE);

  return TRUE;
}

static void
gimp_drawable_real_update (GimpDrawable *drawable,
                           gint          x,
                           gint          y,
                           gint          width,
                           gint          height)
{
  if (drawable->private->buffer_source_node)
    {
      GObject *operation = NULL;

      g_object_get (drawable->private->buffer_source_node,
                    "gegl-operation", &operation,
                    NULL);

      if (operation)
        {
          gegl_operation_invalidate (GEGL_OPERATION (operation),
                                     GEGL_RECTANGLE (x,y,width,height), FALSE);
          g_object_unref (operation);
        }
    }

  gimp_viewable_invalidate_preview (GIMP_VIEWABLE (drawable));
}

static gint64
gimp_drawable_real_estimate_memsize (const GimpDrawable *drawable,
                                     gint                width,
                                     gint                height)
{
  const Babl *format = gimp_drawable_get_format (drawable);

  return (gint64) babl_format_get_bytes_per_pixel (format) * width * height;
}

static void
gimp_drawable_real_convert_type (GimpDrawable      *drawable,
                                 GimpImage         *dest_image,
                                 GimpImageBaseType  new_base_type,
                                 GimpPrecision      new_precision,
                                 gboolean           push_undo)
{
  GeglBuffer *dest_buffer;
  const Babl *format;

  format = gimp_image_get_format (dest_image,
                                  new_base_type,
                                  new_precision,
                                  gimp_drawable_has_alpha (drawable));

  dest_buffer =
    gimp_gegl_buffer_new (GEGL_RECTANGLE (0, 0,
                                          gimp_item_get_width  (GIMP_ITEM (drawable)),
                                          gimp_item_get_height (GIMP_ITEM (drawable))),
                          format);

  gegl_buffer_copy (gimp_drawable_get_buffer (drawable), NULL,
                    dest_buffer, NULL);

  gimp_drawable_set_buffer (drawable, push_undo, NULL, dest_buffer);
  g_object_unref (dest_buffer);
}

static GeglBuffer *
gimp_drawable_real_get_buffer (GimpDrawable *drawable)
{
  gegl_buffer_flush (drawable->private->buffer);
  gimp_gegl_buffer_refetch_tiles (drawable->private->buffer);

  return drawable->private->buffer;
}

static void
gimp_drawable_real_set_buffer (GimpDrawable *drawable,
                               gboolean      push_undo,
                               const gchar  *undo_desc,
                               GeglBuffer   *buffer,
                               gint          offset_x,
                               gint          offset_y)
{
  GimpItem *item = GIMP_ITEM (drawable);
  gboolean  old_has_alpha;

  old_has_alpha = gimp_drawable_has_alpha (drawable);

  gimp_drawable_invalidate_boundary (drawable);

  if (push_undo)
    gimp_image_undo_push_drawable_mod (gimp_item_get_image (item), undo_desc,
                                       drawable, FALSE);

  /*  ref new before unrefing old, they might be the same  */
  g_object_ref (buffer);

  if (drawable->private->buffer)
    g_object_unref (drawable->private->buffer);

  drawable->private->buffer = buffer;

  gimp_item_set_offset (item, offset_x, offset_y);
  gimp_item_set_size (item,
                      gegl_buffer_get_width  (buffer),
                      gegl_buffer_get_height (buffer));

  if (old_has_alpha != gimp_drawable_has_alpha (drawable))
    gimp_drawable_alpha_changed (drawable);

  if (drawable->private->buffer_source_node)
    gegl_node_set (drawable->private->buffer_source_node,
                   "buffer", gimp_drawable_get_buffer (drawable),
                   NULL);
}

static GeglNode *
gimp_drawable_get_node (GimpItem *item)
{
  GimpDrawable *drawable = GIMP_DRAWABLE (item);
  GeglNode     *node;
  GeglNode     *input;
  GeglNode     *output;

  node = GIMP_ITEM_CLASS (parent_class)->get_node (item);

  g_warn_if_fail (drawable->private->mode_node == NULL);

  drawable->private->mode_node =
    gegl_node_new_child (node,
                         "operation", "gimp:normal-mode",
                         NULL);

  input  = gegl_node_get_input_proxy  (node, "input");
  output = gegl_node_get_output_proxy (node, "output");

  if (gimp_item_get_visible (GIMP_ITEM (drawable)))
    {
      gegl_node_connect_to (input,                        "output",
                            drawable->private->mode_node, "input");
      gegl_node_connect_to (drawable->private->mode_node, "output",
                            output,                       "input");
    }
  else
    {
      gegl_node_connect_to (input,  "output",
                            output, "input");
    }

  return node;
}

static void
gimp_drawable_real_push_undo (GimpDrawable *drawable,
                              const gchar  *undo_desc,
                              GeglBuffer   *buffer,
                              gint          x,
                              gint          y,
                              gint          width,
                              gint          height)
{
  if (! buffer)
    {
      buffer = gegl_buffer_new (GEGL_RECTANGLE (0, 0, width, height),
                                gimp_drawable_get_format (drawable));

      gegl_buffer_copy (gimp_drawable_get_buffer (drawable),
                        GEGL_RECTANGLE (x, y, width, height),
                        buffer,
                        GEGL_RECTANGLE (0, 0, 0, 0));
    }
  else
    {
      g_object_ref (buffer);
    }

  gimp_image_undo_push_drawable (gimp_item_get_image (GIMP_ITEM (drawable)),
                                 undo_desc, drawable,
                                 buffer, x, y);

  g_object_unref (buffer);
}

static void
gimp_drawable_real_swap_pixels (GimpDrawable *drawable,
                                GeglBuffer   *buffer,
                                gint          x,
                                gint          y)
{
  GeglBuffer *tmp;
  gint        width  = gegl_buffer_get_width (buffer);
  gint        height = gegl_buffer_get_height (buffer);

  tmp = gegl_buffer_dup (buffer);

  gegl_buffer_copy (gimp_drawable_get_buffer (drawable),
                    GEGL_RECTANGLE (x, y, width, height),
                    buffer,
                    GEGL_RECTANGLE (0, 0, 0, 0));
  gegl_buffer_copy (tmp,
                    GEGL_RECTANGLE (0, 0, width, height),
                    gimp_drawable_get_buffer (drawable),
                    GEGL_RECTANGLE (x, y, 0, 0));

  g_object_unref (tmp);

  gimp_drawable_update (drawable, x, y, width, height);
}

static void
gimp_drawable_sync_source_node (GimpDrawable *drawable,
                                gboolean      detach_fs)
{
  GimpLayer *fs = gimp_drawable_get_floating_sel (drawable);
  GeglNode  *output;

  if (! drawable->private->source_node)
    return;

  output = gegl_node_get_output_proxy (drawable->private->source_node, "output");

  if (fs && ! detach_fs)
    {
      gint off_x, off_y;
      gint fs_off_x, fs_off_y;

      if (! drawable->private->fs_crop_node)
        {
          GeglNode *fs_source;

          fs_source = gimp_drawable_get_source_node (GIMP_DRAWABLE (fs));

          /* rip the fs' source node out of its graph */
          if (fs->layer_offset_node)
            {
              gegl_node_disconnect (fs->layer_offset_node, "input");
              gegl_node_remove_child (gimp_item_get_node (GIMP_ITEM (fs)),
                                      fs_source);
            }

          gegl_node_add_child (drawable->private->source_node, fs_source);

          drawable->private->fs_crop_node =
            gegl_node_new_child (drawable->private->source_node,
                                 "operation", "gegl:crop",
                                 NULL);

          gegl_node_connect_to (fs_source,                       "output",
                                drawable->private->fs_crop_node, "input");

          drawable->private->fs_offset_node =
            gegl_node_new_child (drawable->private->source_node,
                                 "operation", "gegl:translate",
                                 NULL);

          gegl_node_connect_to (drawable->private->fs_crop_node,   "output",
                                drawable->private->fs_offset_node, "input");

          drawable->private->fs_mode_node =
            gegl_node_new_child (drawable->private->source_node,
                                 "operation", "gimp:normal-mode",
                                 NULL);

          gegl_node_connect_to (drawable->private->buffer_source_node, "output",
                                drawable->private->fs_mode_node,       "input");
          gegl_node_connect_to (drawable->private->fs_offset_node,     "output",
                                drawable->private->fs_mode_node,       "aux");

          gegl_node_connect_to (drawable->private->fs_mode_node, "output",
                                output,                          "input");

          g_signal_connect (fs, "notify",
                            G_CALLBACK (gimp_drawable_fs_notify),
                            drawable);
        }

      gimp_item_get_offset (GIMP_ITEM (drawable), &off_x, &off_y);
      gimp_item_get_offset (GIMP_ITEM (fs), &fs_off_x, &fs_off_y);

      gegl_node_set (drawable->private->fs_crop_node,
                     "x",      (gdouble) (off_x - fs_off_x),
                     "y",      (gdouble) (off_y - fs_off_y),
                     "width",  (gdouble) gimp_item_get_width  (GIMP_ITEM (drawable)),
                     "height", (gdouble) gimp_item_get_height (GIMP_ITEM (drawable)),
                     NULL);

      gegl_node_set (drawable->private->fs_offset_node,
                     "x", (gdouble) (fs_off_x - off_x),
                     "y", (gdouble) (fs_off_y - off_y),
                     NULL);

      gimp_gegl_mode_node_set (drawable->private->fs_mode_node,
                               gimp_layer_get_mode (fs),
                               gimp_layer_get_opacity (fs),
                               FALSE);
    }
  else
    {
      if (drawable->private->fs_crop_node)
        {
          GeglNode *fs_source;

          gegl_node_disconnect (drawable->private->fs_crop_node, "input");
          gegl_node_disconnect (drawable->private->fs_offset_node, "input");
          gegl_node_disconnect (drawable->private->fs_mode_node, "input");
          gegl_node_disconnect (drawable->private->fs_mode_node, "aux");

          fs_source = gimp_drawable_get_source_node (GIMP_DRAWABLE (fs));
          gegl_node_remove_child (drawable->private->source_node,
                                  fs_source);

          /* plug the fs' source node back into its graph */
          if (fs->layer_offset_node)
            {
              gegl_node_add_child (gimp_item_get_node (GIMP_ITEM (fs)),
                                   fs_source);
              gegl_node_connect_to (fs_source,             "output",
                                    fs->layer_offset_node, "input");
            }

          gegl_node_remove_child (drawable->private->source_node,
                                  drawable->private->fs_crop_node);
          drawable->private->fs_crop_node = NULL;

          gegl_node_remove_child (drawable->private->source_node,
                                  drawable->private->fs_offset_node);
          drawable->private->fs_offset_node = NULL;

          gegl_node_remove_child (drawable->private->source_node,
                                  drawable->private->fs_mode_node);
          drawable->private->fs_mode_node = NULL;

          g_signal_handlers_disconnect_by_func (fs,
                                                gimp_drawable_fs_notify,
                                                drawable);
        }

      gegl_node_connect_to (drawable->private->buffer_source_node, "output",
                            output,                                "input");
    }
}

static void
gimp_drawable_fs_notify (GimpLayer        *fs,
                         const GParamSpec *pspec,
                         GimpDrawable     *drawable)
{
  if (! strcmp (pspec->name, "offset-x") ||
      ! strcmp (pspec->name, "offset-y") ||
      ! strcmp (pspec->name, "visible")  ||
      ! strcmp (pspec->name, "mode")     ||
      ! strcmp (pspec->name, "opacity"))
    {
      gimp_drawable_sync_source_node (drawable, FALSE);
    }
}

static void
gimp_drawable_fs_update (GimpLayer    *fs,
                         gint          x,
                         gint          y,
                         gint          width,
                         gint          height,
                         GimpDrawable *drawable)
{
  gint fs_off_x, fs_off_y;
  gint off_x, off_y;
  gint dr_x, dr_y, dr_width, dr_height;

  gimp_item_get_offset (GIMP_ITEM (fs), &fs_off_x, &fs_off_y);
  gimp_item_get_offset (GIMP_ITEM (drawable), &off_x, &off_y);

  if (gimp_rectangle_intersect (x + fs_off_x,
                                y + fs_off_y,
                                width,
                                height,
                                off_x,
                                off_y,
                                gimp_item_get_width  (GIMP_ITEM (drawable)),
                                gimp_item_get_height (GIMP_ITEM (drawable)),
                                &dr_x,
                                &dr_y,
                                &dr_width,
                                &dr_height))
    {
      gimp_drawable_update (drawable,
                            dr_x - off_x, dr_y - off_y,
                            dr_width, dr_height);
    }
}


/*  public functions  */

GimpDrawable *
gimp_drawable_new (GType          type,
                   GimpImage     *image,
                   const gchar   *name,
                   gint           offset_x,
                   gint           offset_y,
                   gint           width,
                   gint           height,
                   const Babl    *format)
{
  GimpDrawable *drawable;

  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (g_type_is_a (type, GIMP_TYPE_DRAWABLE), NULL);
  g_return_val_if_fail (width > 0 && height > 0, NULL);
  g_return_val_if_fail (format != NULL, NULL);

  drawable = GIMP_DRAWABLE (gimp_item_new (type,
                                           image, name,
                                           offset_x, offset_y,
                                           width, height));

  drawable->private->buffer = gimp_gegl_buffer_new (GEGL_RECTANGLE (0, 0,
                                                                    width, height),
                                                    format);

  return drawable;
}

gint64
gimp_drawable_estimate_memsize (const GimpDrawable *drawable,
                                gint                width,
                                gint                height)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), 0);

  return GIMP_DRAWABLE_GET_CLASS (drawable)->estimate_memsize (drawable,
                                                               width, height);
}

void
gimp_drawable_update (GimpDrawable *drawable,
                      gint          x,
                      gint          y,
                      gint          width,
                      gint          height)
{
  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));

  if (drawable->private->buffer)
    {
      gegl_buffer_flush (drawable->private->buffer);
      gimp_gegl_buffer_refetch_tiles (drawable->private->buffer);
    }

  g_signal_emit (drawable, gimp_drawable_signals[UPDATE], 0,
                 x, y, width, height);
}

void
gimp_drawable_alpha_changed (GimpDrawable *drawable)
{
  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));

  g_signal_emit (drawable, gimp_drawable_signals[ALPHA_CHANGED], 0);
}

void
gimp_drawable_invalidate_boundary (GimpDrawable *drawable)
{
  GimpDrawableClass *drawable_class;

  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));

  drawable_class = GIMP_DRAWABLE_GET_CLASS (drawable);

  if (drawable_class->invalidate_boundary)
    drawable_class->invalidate_boundary (drawable);
}

void
gimp_drawable_get_active_components (const GimpDrawable *drawable,
                                     gboolean           *active)
{
  GimpDrawableClass *drawable_class;

  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (active != NULL);

  drawable_class = GIMP_DRAWABLE_GET_CLASS (drawable);

  if (drawable_class->get_active_components)
    drawable_class->get_active_components (drawable, active);
}

GimpComponentMask
gimp_drawable_get_active_mask (const GimpDrawable *drawable)
{
  GimpDrawableClass *drawable_class;

  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), 0);

  drawable_class = GIMP_DRAWABLE_GET_CLASS (drawable);

  if (drawable_class->get_active_mask)
    return drawable_class->get_active_mask (drawable);

  return 0;
}

void
gimp_drawable_convert_type (GimpDrawable      *drawable,
                            GimpImage         *dest_image,
                            GimpImageBaseType  new_base_type,
                            GimpPrecision      new_precision,
                            gboolean           push_undo)
{
  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (GIMP_IS_IMAGE (dest_image));
  g_return_if_fail (new_base_type != gimp_drawable_get_base_type (drawable) ||
                    new_precision != gimp_drawable_get_precision (drawable));

  if (! gimp_item_is_attached (GIMP_ITEM (drawable)))
    push_undo = FALSE;

  GIMP_DRAWABLE_GET_CLASS (drawable)->convert_type (drawable, dest_image,
                                                    new_base_type,
                                                    new_precision,
                                                    push_undo);
}

void
gimp_drawable_apply_buffer (GimpDrawable         *drawable,
                            GeglBuffer           *buffer,
                            const GeglRectangle  *buffer_region,
                            gboolean              push_undo,
                            const gchar          *undo_desc,
                            gdouble               opacity,
                            GimpLayerModeEffects  mode,
                            GeglBuffer           *base_buffer,
                            gint                  base_x,
                            gint                  base_y)
{
  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)));
  g_return_if_fail (GEGL_IS_BUFFER (buffer));
  g_return_if_fail (buffer_region != NULL);
  g_return_if_fail (base_buffer == NULL || GEGL_IS_BUFFER (base_buffer));

  GIMP_DRAWABLE_GET_CLASS (drawable)->apply_buffer (drawable, buffer,
                                                    buffer_region,
                                                    push_undo, undo_desc,
                                                    opacity, mode,
                                                    base_buffer,
                                                    base_x, base_y);
}

void
gimp_drawable_replace_buffer (GimpDrawable        *drawable,
                              GeglBuffer          *buffer,
                              const GeglRectangle *buffer_region,
                              gboolean             push_undo,
                              const gchar         *undo_desc,
                              gdouble              opacity,
                              GeglBuffer          *mask,
                              const GeglRectangle *mask_region,
                              gint                 x,
                              gint                 y)
{
  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)));
  g_return_if_fail (GEGL_IS_BUFFER (buffer));
  g_return_if_fail (GEGL_IS_BUFFER (mask));

  GIMP_DRAWABLE_GET_CLASS (drawable)->replace_buffer (drawable, buffer,
                                                      buffer_region,
                                                      push_undo, undo_desc,
                                                      opacity,
                                                      mask, mask_region,
                                                      x, y);
}

GeglBuffer *
gimp_drawable_get_buffer (GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);

  return GIMP_DRAWABLE_GET_CLASS (drawable)->get_buffer (drawable);
}

void
gimp_drawable_set_buffer (GimpDrawable *drawable,
                          gboolean      push_undo,
                          const gchar  *undo_desc,
                          GeglBuffer   *buffer)
{
  gint offset_x, offset_y;

  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (GEGL_IS_BUFFER (buffer));

  if (! gimp_item_is_attached (GIMP_ITEM (drawable)))
    push_undo = FALSE;

  gimp_item_get_offset (GIMP_ITEM (drawable), &offset_x, &offset_y);

  gimp_drawable_set_buffer_full (drawable, push_undo, undo_desc, buffer,
                                 offset_x, offset_y);
}

void
gimp_drawable_set_buffer_full (GimpDrawable *drawable,
                               gboolean      push_undo,
                               const gchar  *undo_desc,
                               GeglBuffer   *buffer,
                               gint          offset_x,
                               gint          offset_y)
{
  GimpItem *item;

  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (GEGL_IS_BUFFER (buffer));

  item = GIMP_ITEM (drawable);

  if (! gimp_item_is_attached (GIMP_ITEM (drawable)))
    push_undo = FALSE;

  if (gimp_item_get_width  (item)   != gegl_buffer_get_width (buffer)  ||
      gimp_item_get_height (item)   != gegl_buffer_get_height (buffer) ||
      gimp_item_get_offset_x (item) != offset_x                        ||
      gimp_item_get_offset_y (item) != offset_y)
    {
      gimp_drawable_update (drawable,
                            0, 0,
                            gimp_item_get_width  (item),
                            gimp_item_get_height (item));
    }

  g_object_freeze_notify (G_OBJECT (drawable));

  GIMP_DRAWABLE_GET_CLASS (drawable)->set_buffer (drawable,
                                                  push_undo, undo_desc,
                                                  buffer,
                                                  offset_x, offset_y);

  g_object_thaw_notify (G_OBJECT (drawable));

  gimp_drawable_update (drawable,
                        0, 0,
                        gimp_item_get_width  (item),
                        gimp_item_get_height (item));
}

GeglNode *
gimp_drawable_get_source_node (GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);

  if (drawable->private->source_node)
    return drawable->private->source_node;

  drawable->private->source_node = gegl_node_new ();

  drawable->private->buffer_source_node =
    gegl_node_new_child (drawable->private->source_node,
                         "operation", "gegl:buffer-source",
                         "buffer",    gimp_drawable_get_buffer (drawable),
                         NULL);

  gimp_drawable_sync_source_node (drawable, FALSE);

  return drawable->private->source_node;
}

GeglNode *
gimp_drawable_get_mode_node (GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);

  if (! drawable->private->mode_node)
    gimp_item_get_node (GIMP_ITEM (drawable));

  return drawable->private->mode_node;
}

void
gimp_drawable_swap_pixels (GimpDrawable *drawable,
                           GeglBuffer   *buffer,
                           gint          x,
                           gint          y)
{
  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (GEGL_IS_BUFFER (buffer));

  GIMP_DRAWABLE_GET_CLASS (drawable)->swap_pixels (drawable, buffer, x, y);
}

void
gimp_drawable_push_undo (GimpDrawable *drawable,
                         const gchar  *undo_desc,
                         GeglBuffer   *buffer,
                         gint          x,
                         gint          y,
                         gint          width,
                         gint          height)
{
  GimpItem *item;

  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (buffer == NULL || GEGL_IS_BUFFER (buffer));

  item = GIMP_ITEM (drawable);

  g_return_if_fail (gimp_item_is_attached (item));

  if (! buffer &&
      ! gimp_rectangle_intersect (x, y,
                                  width, height,
                                  0, 0,
                                  gimp_item_get_width (item),
                                  gimp_item_get_height (item),
                                  &x, &y, &width, &height))
    {
      g_warning ("%s: tried to push empty region", G_STRFUNC);
      return;
    }

  GIMP_DRAWABLE_GET_CLASS (drawable)->push_undo (drawable, undo_desc,
                                                 buffer,
                                                 x, y, width, height);
}

void
gimp_drawable_fill (GimpDrawable      *drawable,
                    const GimpRGB     *color,
                    const GimpPattern *pattern)
{
  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (color != NULL || pattern != NULL);
  g_return_if_fail (pattern == NULL || GIMP_IS_PATTERN (pattern));

  if (color)
    {
      GimpRGB    c = *color;
      GeglColor *col;

      if (! gimp_drawable_has_alpha (drawable))
        gimp_rgb_set_alpha (&c, 1.0);

      col = gimp_gegl_color_new (&c);
      gegl_buffer_set_color (gimp_drawable_get_buffer (drawable),
                             NULL, col);
      g_object_unref (col);
    }
  else
    {
      GeglBuffer *src_buffer = gimp_pattern_create_buffer (pattern);

      gegl_buffer_set_pattern (gimp_drawable_get_buffer (drawable),
                               NULL, src_buffer, 0, 0);
      g_object_unref (src_buffer);
    }

  gimp_drawable_update (drawable,
                        0, 0,
                        gimp_item_get_width  (GIMP_ITEM (drawable)),
                        gimp_item_get_height (GIMP_ITEM (drawable)));
}

void
gimp_drawable_fill_by_type (GimpDrawable *drawable,
                            GimpContext  *context,
                            GimpFillType  fill_type)
{
  GimpRGB      color;
  GimpPattern *pattern = NULL;

  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));

  switch (fill_type)
    {
    case GIMP_FOREGROUND_FILL:
      gimp_context_get_foreground (context, &color);
      break;

    case GIMP_BACKGROUND_FILL:
      gimp_context_get_background (context, &color);
      break;

    case GIMP_WHITE_FILL:
      gimp_rgba_set (&color, 1.0, 1.0, 1.0, GIMP_OPACITY_OPAQUE);
      break;

    case GIMP_TRANSPARENT_FILL:
      gimp_rgba_set (&color, 0.0, 0.0, 0.0, GIMP_OPACITY_TRANSPARENT);
      break;

    case GIMP_PATTERN_FILL:
      pattern = gimp_context_get_pattern (context);
      break;

    case GIMP_NO_FILL:
      return;

    default:
      g_warning ("%s: unknown fill type %d", G_STRFUNC, fill_type);
      return;
    }

  gimp_drawable_fill (drawable, pattern ? NULL : &color, pattern);
}

const Babl *
gimp_drawable_get_format (const GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);

  return gegl_buffer_get_format (drawable->private->buffer);
}

const Babl *
gimp_drawable_get_format_with_alpha (const GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);

  return gimp_image_get_format (gimp_item_get_image (GIMP_ITEM (drawable)),
                                gimp_drawable_get_base_type (drawable),
                                gimp_drawable_get_precision (drawable),
                                TRUE);
}

const Babl *
gimp_drawable_get_format_without_alpha (const GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);

  return gimp_image_get_format (gimp_item_get_image (GIMP_ITEM (drawable)),
                                gimp_drawable_get_base_type (drawable),
                                gimp_drawable_get_precision (drawable),
                                FALSE);
}

gboolean
gimp_drawable_has_alpha (const GimpDrawable *drawable)
{
  const Babl *format;

  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), FALSE);

  format = gegl_buffer_get_format (drawable->private->buffer);

  return babl_format_has_alpha (format);
}

GimpImageBaseType
gimp_drawable_get_base_type (const GimpDrawable *drawable)
{
  const Babl *format;

  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), -1);

  format = gegl_buffer_get_format (drawable->private->buffer);

  return gimp_babl_format_get_base_type (format);
}

GimpPrecision
gimp_drawable_get_precision (const GimpDrawable *drawable)
{
  const Babl *format;

  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), -1);

  format = gegl_buffer_get_format (drawable->private->buffer);

  return gimp_babl_format_get_precision (format);
}

gboolean
gimp_drawable_is_rgb (const GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), FALSE);

  return (gimp_drawable_get_base_type (drawable) == GIMP_RGB);
}

gboolean
gimp_drawable_is_gray (const GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), FALSE);

  return (gimp_drawable_get_base_type (drawable) == GIMP_GRAY);
}

gboolean
gimp_drawable_is_indexed (const GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), FALSE);

  return (gimp_drawable_get_base_type (drawable) == GIMP_INDEXED);
}

const guchar *
gimp_drawable_get_colormap (const GimpDrawable *drawable)
{
  GimpImage *image;

  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);

  image = gimp_item_get_image (GIMP_ITEM (drawable));

  return image ? gimp_image_get_colormap (image) : NULL;
}

GimpLayer *
gimp_drawable_get_floating_sel (const GimpDrawable *drawable)
{
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);

  return drawable->private->floating_selection;
}

void
gimp_drawable_attach_floating_sel (GimpDrawable *drawable,
                                   GimpLayer    *floating_sel)
{
  GimpImage *image;

  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)));
  g_return_if_fail (gimp_drawable_get_floating_sel (drawable) == NULL);
  g_return_if_fail (GIMP_IS_LAYER (floating_sel));

  GIMP_LOG (FLOATING_SELECTION, "%s", G_STRFUNC);

  image = gimp_item_get_image (GIMP_ITEM (drawable));

  drawable->private->floating_selection = floating_sel;
  gimp_image_set_floating_selection (image, floating_sel);

  /*  clear the selection  */
  gimp_drawable_invalidate_boundary (GIMP_DRAWABLE (floating_sel));

  gimp_drawable_sync_source_node (drawable, FALSE);

  g_signal_connect (floating_sel, "update",
                    G_CALLBACK (gimp_drawable_fs_update),
                    drawable);

  gimp_drawable_fs_update (floating_sel,
                           0, 0,
                           gimp_item_get_width  (GIMP_ITEM (floating_sel)),
                           gimp_item_get_height (GIMP_ITEM (floating_sel)),
                           drawable);
}

void
gimp_drawable_detach_floating_sel (GimpDrawable *drawable)
{
  GimpImage *image;
  GimpLayer *floating_sel;

  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (gimp_drawable_get_floating_sel (drawable) != NULL);

  GIMP_LOG (FLOATING_SELECTION, "%s", G_STRFUNC);

  image        = gimp_item_get_image (GIMP_ITEM (drawable));
  floating_sel = drawable->private->floating_selection;

  gimp_drawable_sync_source_node (drawable, TRUE);

  g_signal_handlers_disconnect_by_func (floating_sel,
                                        gimp_drawable_fs_update,
                                        drawable);

  gimp_drawable_fs_update (floating_sel,
                           0, 0,
                           gimp_item_get_width  (GIMP_ITEM (floating_sel)),
                           gimp_item_get_height (GIMP_ITEM (floating_sel)),
                           drawable);

  /*  clear the selection  */
  gimp_drawable_invalidate_boundary (GIMP_DRAWABLE (floating_sel));

  gimp_image_set_floating_selection (image, NULL);
  drawable->private->floating_selection = NULL;
}
