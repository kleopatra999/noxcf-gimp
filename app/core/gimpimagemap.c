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

/* This file contains the code necessary for generating on canvas
 * previews, either by connecting a function to process the pixels or
 * by connecting a specified GEGL operation to do the processing. It
 * keeps an undo buffer to allow direct modification of the pixel data
 * (so that it will show up in the projection) and it will restore the
 * source in case the mapping procedure was cancelled.
 *
 * To create a tool that uses this, see /tools/gimpimagemaptool.c for
 * the interface and /tools/gimpcolorbalancetool.c for an example of
 * using that interface.
 *
 * Note that when talking about on canvas preview, we are speaking
 * about non destructive image editing where the operation is previewd
 * before being applied.
 */

#include "config.h"

#include <glib-object.h>
#include <gegl.h>

#include "core-types.h"

#include "gegl/gimp-gegl-utils.h"

#include "gimpdrawable.h"
#include "gimpdrawable-shadow.h"
#include "gimpimage.h"
#include "gimpimagemap.h"
#include "gimpmarshal.h"
#include "gimppickable.h"
#include "gimpviewable.h"
#include "gimpchannel.h"


enum
{
  FLUSH,
  LAST_SIGNAL
};


struct _GimpImageMap
{
  GimpObject     parent_instance;

  GimpDrawable  *drawable;
  gchar         *undo_desc;

  GeglBuffer    *undo_buffer;
  gint           undo_offset_x;
  gint           undo_offset_y;

  GeglNode      *gegl;
  GeglNode      *input;
  GeglNode      *translate;
  GeglNode      *operation;
  GeglNode      *output;
  GeglProcessor *processor;

  guint          idle_id;

  GTimer        *timer;
  guint64        pixel_count;
};


static void   gimp_image_map_pickable_iface_init (GimpPickableInterface *iface);

static void            gimp_image_map_dispose         (GObject             *object);
static void            gimp_image_map_finalize        (GObject             *object);

static GimpImage     * gimp_image_map_get_image       (GimpPickable        *pickable);
static const Babl    * gimp_image_map_get_format      (GimpPickable        *pickable);
static const Babl    * gimp_image_map_get_format_with_alpha
                                                      (GimpPickable        *pickable);
static GeglBuffer    * gimp_image_map_get_buffer      (GimpPickable        *pickable);
static gboolean        gimp_image_map_get_pixel_at    (GimpPickable        *pickable,
                                                       gint                 x,
                                                       gint                 y,
                                                       const Babl          *format,
                                                       gpointer             pixel);

static void            gimp_image_map_update_undo_buffer
                                                      (GimpImageMap        *image_map,
                                                       const GeglRectangle *rect);
static gboolean        gimp_image_map_do              (GimpImageMap        *image_map);
static void            gimp_image_map_data_written    (GObject             *operation,
                                                       const GeglRectangle *extent,
                                                       GimpImageMap        *image_map);
static void            gimp_image_map_stop_idle       (GimpImageMap        *image_map);


G_DEFINE_TYPE_WITH_CODE (GimpImageMap, gimp_image_map, GIMP_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (GIMP_TYPE_PICKABLE,
                                                gimp_image_map_pickable_iface_init))

#define parent_class gimp_image_map_parent_class

static guint image_map_signals[LAST_SIGNAL] = { 0 };


static void
gimp_image_map_class_init (GimpImageMapClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  image_map_signals[FLUSH] =
    g_signal_new ("flush",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  G_STRUCT_OFFSET (GimpImageMapClass, flush),
                  NULL, NULL,
                  gimp_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  object_class->dispose  = gimp_image_map_dispose;
  object_class->finalize = gimp_image_map_finalize;
}

static void
gimp_image_map_pickable_iface_init (GimpPickableInterface *iface)
{
  iface->get_image             = gimp_image_map_get_image;
  iface->get_format            = gimp_image_map_get_format;
  iface->get_format_with_alpha = gimp_image_map_get_format_with_alpha;
  iface->get_buffer            = gimp_image_map_get_buffer;
  iface->get_pixel_at          = gimp_image_map_get_pixel_at;
}

static void
gimp_image_map_init (GimpImageMap *image_map)
{
  image_map->drawable      = NULL;
  image_map->undo_desc     = NULL;
  image_map->undo_buffer   = NULL;
  image_map->undo_offset_x = 0;
  image_map->undo_offset_y = 0;
  image_map->idle_id       = 0;

#ifdef GIMP_UNSTABLE
  image_map->timer         = g_timer_new ();
#else
  image_map->timer         = NULL;
#endif

  image_map->pixel_count   = 0;

  if (image_map->timer)
    g_timer_stop (image_map->timer);
}

static void
gimp_image_map_dispose (GObject *object)
{
  GimpImageMap *image_map = GIMP_IMAGE_MAP (object);

  if (image_map->drawable)
    gimp_viewable_preview_thaw (GIMP_VIEWABLE (image_map->drawable));

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gimp_image_map_finalize (GObject *object)
{
  GimpImageMap *image_map = GIMP_IMAGE_MAP (object);

  if (image_map->undo_desc)
    {
      g_free (image_map->undo_desc);
      image_map->undo_desc = NULL;
    }

  if (image_map->undo_buffer)
    {
      g_object_unref (image_map->undo_buffer);
      image_map->undo_buffer = NULL;
    }

  gimp_image_map_stop_idle (image_map);

  if (image_map->gegl)
    {
      g_object_unref (image_map->gegl);
      image_map->gegl      = NULL;
      image_map->input     = NULL;
      image_map->translate = NULL;
      image_map->output    = NULL;
    }

  if (image_map->operation)
    {
      g_object_unref (image_map->operation);
      image_map->operation = NULL;
    }

  if (image_map->drawable)
    {
      gimp_drawable_free_shadow_buffer (image_map->drawable);

      g_object_unref (image_map->drawable);
      image_map->drawable = NULL;
    }

  if (image_map->timer)
    {
      g_timer_destroy (image_map->timer);
      image_map->timer = NULL;
    }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GimpImage *
gimp_image_map_get_image (GimpPickable *pickable)
{
  GimpImageMap *image_map = GIMP_IMAGE_MAP (pickable);

  return gimp_pickable_get_image (GIMP_PICKABLE (image_map->drawable));
}

static const Babl *
gimp_image_map_get_format (GimpPickable *pickable)
{
  GimpImageMap *image_map = GIMP_IMAGE_MAP (pickable);

  return gimp_pickable_get_format (GIMP_PICKABLE (image_map->drawable));
}

static const Babl *
gimp_image_map_get_format_with_alpha (GimpPickable *pickable)
{
  GimpImageMap *image_map = GIMP_IMAGE_MAP (pickable);

  return gimp_pickable_get_format_with_alpha (GIMP_PICKABLE (image_map->drawable));
}

static GeglBuffer *
gimp_image_map_get_buffer (GimpPickable *pickable)
{
  GimpImageMap *image_map = GIMP_IMAGE_MAP (pickable);

  if (image_map->undo_buffer)
    return image_map->undo_buffer;

  return gimp_pickable_get_buffer (GIMP_PICKABLE (image_map->drawable));
}

static gboolean
gimp_image_map_get_pixel_at (GimpPickable *pickable,
                             gint          x,
                             gint          y,
                             const Babl   *format,
                             gpointer      pixel)
{
  GimpImageMap *image_map = GIMP_IMAGE_MAP (pickable);
  GimpItem     *item      = GIMP_ITEM (image_map->drawable);

  if (x >= 0 && x < gimp_item_get_width  (item) &&
      y >= 0 && y < gimp_item_get_height (item))
    {
      /* Check if done damage to original image */
      if (image_map->undo_buffer)
        {
          gint offset_x = image_map->undo_offset_x;
          gint offset_y = image_map->undo_offset_y;
          gint width    = gegl_buffer_get_width (image_map->undo_buffer);
          gint height   = gegl_buffer_get_height (image_map->undo_buffer);

          if (x >= offset_x && x < offset_x + width &&
              y >= offset_y && y < offset_y + height)
            {
              gegl_buffer_sample (image_map->undo_buffer,
                                  x - offset_x, y - offset_y,
                                  NULL, pixel, format,
                                  GEGL_SAMPLER_NEAREST,
                                  GEGL_ABYSS_NONE);

              return TRUE;
            }
        }

      return gimp_pickable_get_pixel_at (GIMP_PICKABLE (image_map->drawable),
                                         x, y, format, pixel);
    }

  return FALSE;
}

GimpImageMap *
gimp_image_map_new (GimpDrawable *drawable,
                    const gchar  *undo_desc,
                    GeglNode     *operation)
{
  GimpImageMap *image_map;

  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)), NULL);
  g_return_val_if_fail (GEGL_IS_NODE (operation), NULL);

  image_map = g_object_new (GIMP_TYPE_IMAGE_MAP, NULL);

  image_map->drawable  = g_object_ref (drawable);
  image_map->undo_desc = g_strdup (undo_desc);

  if (operation)
    image_map->operation = g_object_ref (operation);

  gimp_viewable_preview_freeze (GIMP_VIEWABLE (drawable));

  return image_map;
}

void
gimp_image_map_apply (GimpImageMap        *image_map,
                      const GeglRectangle *visible)
{
  GeglBuffer    *input_buffer;
  GeglBuffer    *output_buffer;
  GeglRectangle  rect;

  g_return_if_fail (GIMP_IS_IMAGE_MAP (image_map));

  /*  If we're still working, remove the timer  */
  gimp_image_map_stop_idle (image_map);

  /*  Make sure the drawable is still valid  */
  if (! gimp_item_is_attached (GIMP_ITEM (image_map->drawable)))
    return;

  /*  The application should occur only within selection bounds  */
  if (! gimp_item_mask_intersect (GIMP_ITEM (image_map->drawable),
                                  &rect.x, &rect.y,
                                  &rect.width, &rect.height))
    return;

  /*  If undo buffer don't exist, or change size, (re)allocate  */
  gimp_image_map_update_undo_buffer (image_map, &rect);

  input_buffer  = image_map->undo_buffer;
  output_buffer = gimp_drawable_get_shadow_buffer (image_map->drawable);

  if (! image_map->gegl)
    {
      image_map->gegl = gegl_node_new ();

      g_object_set (image_map->gegl,
                    "dont-cache", TRUE,
                    NULL);

      image_map->input =
        gegl_node_new_child (image_map->gegl,
                             "operation", "gegl:buffer-source",
                             NULL);

      image_map->translate =
        gegl_node_new_child (image_map->gegl,
                             "operation", "gegl:translate",
                             NULL);

      gegl_node_add_child (image_map->gegl, image_map->operation);

      image_map->output =
        gegl_node_new_child (image_map->gegl,
                             "operation", "gegl:write-buffer",
                             NULL);

      g_signal_connect (image_map->output, "computed",
                        G_CALLBACK (gimp_image_map_data_written),
                        image_map);

      if (gegl_node_has_pad (image_map->operation, "input") &&
          gegl_node_has_pad (image_map->operation, "output"))
        {
          /*  if there are input and output pads we probably have a
           *  filter OP, connect it on both ends.
           */
          gegl_node_link_many (image_map->input,
                               image_map->translate,
                               image_map->operation,
                               image_map->output,
                               NULL);
        }
      else if (gegl_node_has_pad (image_map->operation, "output"))
        {
          /*  if there is only an output pad we probably have a
           *  source OP, blend its result on top of the original
           *  pixels.
           */
          GeglNode *over = gegl_node_new_child (image_map->gegl,
                                                "operation", "gegl:over",
                                                NULL);

          gegl_node_link_many (image_map->input,
                               image_map->translate,
                               over,
                               image_map->output,
                               NULL);

          gegl_node_connect_to (image_map->operation, "output",
                                over, "aux");
        }
      else
        {
          /* otherwise we just construct a silly nop pipleline
           */
          gegl_node_link_many (image_map->input,
                               image_map->translate,
                               image_map->output,
                               NULL);
        }
    }

  gegl_node_set (image_map->input,
                 "buffer", input_buffer,
                 NULL);

  gegl_node_set (image_map->translate,
                 "x", (gdouble) rect.x,
                 "y", (gdouble) rect.y,
                 NULL);

  gegl_node_set (image_map->output,
                 "buffer", output_buffer,
                 NULL);

  image_map->processor = gegl_node_new_processor (image_map->output, &rect);

  if (image_map->timer)
    {
      image_map->pixel_count = 0;
      g_timer_start (image_map->timer);
      g_timer_stop (image_map->timer);
    }

  /*  Start the intermittant work procedure  */
  image_map->idle_id = g_idle_add ((GSourceFunc) gimp_image_map_do, image_map);
}

void
gimp_image_map_commit (GimpImageMap *image_map)
{
  g_return_if_fail (GIMP_IS_IMAGE_MAP (image_map));

  if (image_map->idle_id)
    {
      g_source_remove (image_map->idle_id);
      image_map->idle_id = 0;

      /*  Finish the changes  */
      while (gimp_image_map_do (image_map));
    }

  /*  Make sure the drawable is still valid  */
  if (! gimp_item_is_attached (GIMP_ITEM (image_map->drawable)))
    return;

  /*  Register an undo step  */
  if (image_map->undo_buffer)
    {
      gint x      = image_map->undo_offset_x;
      gint y      = image_map->undo_offset_y;
      gint width  = gegl_buffer_get_width  (image_map->undo_buffer);
      gint height = gegl_buffer_get_height (image_map->undo_buffer);

      gimp_drawable_push_undo (image_map->drawable,
                               image_map->undo_desc,
                               image_map->undo_buffer,
                               x, y, width, height);

      g_object_unref (image_map->undo_buffer);
      image_map->undo_buffer = NULL;
    }
}

void
gimp_image_map_clear (GimpImageMap *image_map)
{
  g_return_if_fail (GIMP_IS_IMAGE_MAP (image_map));

  gimp_image_map_stop_idle (image_map);

  /*  Make sure the drawable is still valid  */
  if (! gimp_item_is_attached (GIMP_ITEM (image_map->drawable)))
    return;

  /*  restore the original image  */
  if (image_map->undo_buffer)
    {
      if (gegl_buffer_get_format (image_map->undo_buffer) !=
          gimp_drawable_get_format (image_map->drawable))
        {
          g_message ("image depth change, unable to restore original image");
        }
      else
        {
          gint x      = image_map->undo_offset_x;
          gint y      = image_map->undo_offset_y;
          gint width  = gegl_buffer_get_width  (image_map->undo_buffer);
          gint height = gegl_buffer_get_height (image_map->undo_buffer);

          gegl_buffer_copy (image_map->undo_buffer,
                            GEGL_RECTANGLE (0, 0, width, height),
                            gimp_drawable_get_buffer (image_map->drawable),
                            GEGL_RECTANGLE (x, y, width, height));

          gimp_drawable_update (image_map->drawable, x, y, width, height);
        }

      g_object_unref (image_map->undo_buffer);
      image_map->undo_buffer = NULL;
    }
}

void
gimp_image_map_abort (GimpImageMap *image_map)
{
  g_return_if_fail (GIMP_IS_IMAGE_MAP (image_map));

  gimp_image_map_stop_idle (image_map);

  if (! gimp_item_is_attached (GIMP_ITEM (image_map->drawable)))
    return;

  gimp_image_map_clear (image_map);
}


/*  private functions  */

static void
gimp_image_map_update_undo_buffer (GimpImageMap        *image_map,
                                   const GeglRectangle *rect)
{
  gint undo_offset_x;
  gint undo_offset_y;
  gint undo_width;
  gint undo_height;

  if (image_map->undo_buffer)
    {
      undo_offset_x = image_map->undo_offset_x;
      undo_offset_y = image_map->undo_offset_y;
      undo_width    = gegl_buffer_get_width  (image_map->undo_buffer);
      undo_height   = gegl_buffer_get_height (image_map->undo_buffer);
    }
  else
    {
      undo_offset_x = 0;
      undo_offset_y = 0;
      undo_width    = 0;
      undo_height   = 0;
    }

  if (! image_map->undo_buffer     ||
      undo_offset_x != rect->x     ||
      undo_offset_y != rect->y     ||
      undo_width    != rect->width ||
      undo_height   != rect->height)
    {
      /* If either the extents changed or the buffer don't exist,
       * allocate new
       */
      if (! image_map->undo_buffer   ||
          undo_width  != rect->width ||
          undo_height != rect->height)
        {
          if (image_map->undo_buffer)
            g_object_unref (image_map->undo_buffer);

          image_map->undo_buffer =
            gegl_buffer_new (GEGL_RECTANGLE (0, 0,
                                             rect->width, rect->height),
                             gimp_drawable_get_format (image_map->drawable));
        }

      /*  Copy from the image to the new tiles  */
      gegl_buffer_copy (gimp_drawable_get_buffer (image_map->drawable),
                        rect,
                        image_map->undo_buffer,
                        GEGL_RECTANGLE (0, 0, 0, 0));

      /*  Set the offsets  */
      image_map->undo_offset_x = rect->x;
      image_map->undo_offset_y = rect->y;
    }
}

static gboolean
gimp_image_map_do (GimpImageMap *image_map)
{
  gboolean pending;

  if (! gimp_item_is_attached (GIMP_ITEM (image_map->drawable)))
    {
      image_map->idle_id = 0;

      if (image_map->processor)
        {
          g_object_unref (image_map->processor);
          image_map->processor = NULL;
        }

      return FALSE;
    }

  if (image_map->timer)
    g_timer_continue (image_map->timer);

  pending = gegl_processor_work (image_map->processor, NULL);

  if (image_map->timer)
    g_timer_stop (image_map->timer);

  if (! pending)
    {
      if (image_map->timer)
        g_printerr ("%s: %g MPixels/sec\n",
                    image_map->undo_desc,
                    (gdouble) image_map->pixel_count /
                    (1000000.0 *
                     g_timer_elapsed (image_map->timer, NULL)));

      g_object_unref (image_map->processor);
      image_map->processor = NULL;

      image_map->idle_id = 0;

      g_signal_emit (image_map, image_map_signals[FLUSH], 0);

      return FALSE;
    }

  g_signal_emit (image_map, image_map_signals[FLUSH], 0);

  return TRUE;
}

static void
gimp_image_map_data_written (GObject             *operation,
                             const GeglRectangle *extent,
                             GimpImageMap        *image_map)
{
  GimpImage *image = gimp_item_get_image (GIMP_ITEM (image_map->drawable));

  if (! gimp_channel_is_empty (gimp_image_get_mask (image)))
    {
      /* Reset to initial drawable conditions. */

      gegl_buffer_copy (image_map->undo_buffer,
                        GEGL_RECTANGLE (extent->x - image_map->undo_offset_x,
                                        extent->y - image_map->undo_offset_y,
                                        extent->width, extent->height),
                        gimp_drawable_get_buffer (image_map->drawable),
                        GEGL_RECTANGLE (extent->x, extent->y, 0, 0));
    }

  /* Apply the result of the gegl graph. */
  gimp_drawable_apply_buffer (image_map->drawable,
                              gimp_drawable_get_shadow_buffer (image_map->drawable),
                              GEGL_RECTANGLE (extent->x, extent->y,
                                              extent->width, extent->height),
                              FALSE, NULL,
                              GIMP_OPACITY_OPAQUE, GIMP_REPLACE_MODE,
                              NULL, extent->x, extent->y);

  gimp_drawable_update (image_map->drawable,
                        extent->x, extent->y,
                        extent->width, extent->height);

  if (image_map->timer)
    image_map->pixel_count += extent->width * extent->height;
}

static void
gimp_image_map_stop_idle (GimpImageMap *image_map)
{
  if (image_map->idle_id)
    {
      g_source_remove (image_map->idle_id);
      image_map->idle_id = 0;

      if (image_map->processor)
        {
          g_object_unref (image_map->processor);
          image_map->processor = NULL;
        }
    }
}
