/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995-1997 Spencer Kimball and Peter Mattis
 *
 * gimpprojectable.h
 * Copyright (C) 2008  Michael Natterer <mitch@gimp.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef __GIMP_PROJECTABLE_H__
#define __GIMP_PROJECTABLE_H__


#define GIMP_TYPE_PROJECTABLE               (gimp_projectable_interface_get_type ())
#define GIMP_IS_PROJECTABLE(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GIMP_TYPE_PROJECTABLE))
#define GIMP_PROJECTABLE(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj), GIMP_TYPE_PROJECTABLE, GimpProjectable))
#define GIMP_PROJECTABLE_GET_INTERFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE ((obj), GIMP_TYPE_PROJECTABLE, GimpProjectableInterface))


typedef struct _GimpProjectableInterface GimpProjectableInterface;

struct _GimpProjectableInterface
{
  GTypeInterface base_iface;

  /*  signals  */
  void           (* update)             (GimpProjectable *projectable,
                                         gint             x,
                                         gint             y,
                                         gint             width,
                                         gint             height);
  void           (* flush)              (GimpProjectable *projectable,
                                         gboolean         invalidate_preview);
  void           (* structure_changed)  (GimpProjectable *projectable);

  /*  virtual functions  */
  GimpImage    * (* get_image)          (GimpProjectable *projectable);
  void           (* get_size)           (GimpProjectable *projectable,
                                         gint            *width,
                                         gint            *height);
  GeglNode     * (* get_graph)          (GimpProjectable *projectable);
  void           (* invalidate_preview) (GimpProjectable *projectable);

  /*  legacy API virtual functions  */
  GList        * (* get_layers)         (GimpProjectable *projectable);
  GList        * (* get_channels)       (GimpProjectable *projectable);
  gboolean     * (* get_components)     (GimpProjectable *projectable);
  const guchar * (* get_colormap)       (GimpProjectable *projectable);
};


GType          gimp_projectable_interface_get_type (void) G_GNUC_CONST;

void           gimp_projectable_update             (GimpProjectable *projectable,
                                                    gint             x,
                                                    gint             y,
                                                    gint             width,
                                                    gint             height);
void           gimp_projectable_flush              (GimpProjectable *projectable,
                                                    gboolean         preview_invalidated);
void           gimp_projectable_structure_changed  (GimpProjectable *projectable);

GimpImage    * gimp_projectable_get_image          (GimpProjectable *projectable);
void           gimp_projectable_get_size           (GimpProjectable *projectable,
                                                    gint            *width,
                                                    gint            *height);
GeglNode     * gimp_projectable_get_graph          (GimpProjectable *projectable);
void           gimp_projectable_invalidate_preview (GimpProjectable *projectable);

/*  legacy API  */
GList        * gimp_projectable_get_layers         (GimpProjectable *projectable);
GList        * gimp_projectable_get_channels       (GimpProjectable *projectable);
gboolean     * gimp_projectable_get_components     (GimpProjectable *projectable);
const guchar * gimp_projectable_get_colormap       (GimpProjectable *projectable);


#endif  /* __GIMP_PROJECTABLE_H__ */