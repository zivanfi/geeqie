/*
 * Copyright (C) 2006 John Ellis
 * Copyright (C) 2008 - 2016 The Geeqie Team
 *
 * Author: John Ellis
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "pixbuf-renderer.h"
#include "renderer-tiles.h"

#include "intl.h"
#include "layout.h"

#include <gtk/gtk.h>


/* comment this out if not using this from within Geeqie
 * defining GQ_BUILD does these things:
 *   - Sets the shift-click scroller pixbuf to a nice icon instead of a black box
 */
#define GQ_BUILD 1

#ifdef GQ_BUILD
#include "main.h"
#include "pixbuf_util.h"
#include "exif.h"
#else
typedef enum {
	EXIF_ORIENTATION_UNKNOWN	= 0,
	EXIF_ORIENTATION_TOP_LEFT	= 1,
	EXIF_ORIENTATION_TOP_RIGHT	= 2,
	EXIF_ORIENTATION_BOTTOM_RIGHT	= 3,
	EXIF_ORIENTATION_BOTTOM_LEFT	= 4,
	EXIF_ORIENTATION_LEFT_TOP	= 5,
	EXIF_ORIENTATION_RIGHT_TOP	= 6,
	EXIF_ORIENTATION_RIGHT_BOTTOM	= 7,
	EXIF_ORIENTATION_LEFT_BOTTOM	= 8
} ExifOrientationType;
#endif


/* size to use when breaking up image pane for rendering */
#define PR_TILE_SIZE 128

typedef struct _ImageTile ImageTile;
typedef struct _QueueData QueueData;

struct _ImageTile
{
	cairo_surface_t *surface;	/* off screen buffer */
	GdkPixbuf *pixbuf;	/* pixbuf area for zooming */
	gint x;			/* x offset into image */
	gint y;			/* y offset into image */
	gint w;			/* width that is visible (may be less if at edge of image) */
	gint h;			/* height '' */

	gboolean blank;

/* render_todo: (explanation)
	NONE	do nothing
	AREA	render area of tile, usually only used when loading an image
		note: will jump to an ALL if render_done is not ALL.
	ALL	render entire tile, if never done before w/ ALL, for expose events *only*
*/

	ImageRenderType render_todo;	/* what to do (see above) */
	ImageRenderType render_done;	/* highest that has been done before on tile */

	QueueData *qd;
	QueueData *qd2;

	guint size;		/* est. memory used by pixmap and pixbuf */
};

struct _QueueData
{
	ImageTile *it;
	gint x;
	gint y;
	gint w;
	gint h;
	gboolean new_data;
};

typedef struct _OverlayData OverlayData;
struct _OverlayData
{
	gint id;

	GdkPixbuf *pixbuf;
	GdkWindow *window;

	gint x;
	gint y;

	OverlayRendererFlags flags;
};

typedef struct _RendererTiles RendererTiles;

struct _RendererTiles
{
	RendererFuncs f;
	PixbufRenderer *pr;

	gint tile_cache_max;		/* max mb to use for offscreen buffer */

	gint tile_width;
	gint tile_height;
	gint tile_cols;		/* count of tile columns */
	GList *tiles;		/* list of buffer tiles */
	gint tile_cache_size;	/* allocated size of pixmaps/pixbufs */
	GList *draw_queue;	/* list of areas to redraw */
	GList *draw_queue_2pass;/* list when 2 pass is enabled */

	GList *overlay_list;
	cairo_surface_t *overlay_buffer;

	guint draw_idle_id; /* event source id */

	GdkPixbuf *spare_tile;

	gint stereo_mode;
	gint stereo_off_x;
	gint stereo_off_y;

	gint x_scroll;  /* allow local adjustment and mirroring */
	gint y_scroll;

	gint hidpi_scale;
};



static void rt_border_draw(RendererTiles *rt, gint x, gint y, gint w, gint h);
static void rt_overlay_draw(RendererTiles *rt, gint x, gint y, gint w, gint h, ImageTile *it);


static void rt_tile_free_all(RendererTiles *rt);
static void rt_tile_invalidate_region(RendererTiles *rt, gint x, gint y, gint w, gint h);
static gboolean rt_tile_is_visible(RendererTiles *rt, ImageTile *it);
static void rt_queue_clear(RendererTiles *rt);
static void rt_queue_merge(QueueData *parent, QueueData *qd);
static void rt_queue(RendererTiles *rt, gint x, gint y, gint w, gint h,
		     gint clamp, ImageRenderType render, gboolean new_data, gboolean only_existing);

static void rt_hierarchy_changed_cb(GtkWidget *widget, GtkWidget *previous_toplevel, gpointer data);
static gint rt_queue_draw_idle_cb(gpointer data);

#define GET_RIGHT_PIXBUF_OFFSET(rt) \
        (( (rt->stereo_mode & PR_STEREO_RIGHT) && !(rt->stereo_mode & PR_STEREO_SWAP)) || \
         (!(rt->stereo_mode & PR_STEREO_RIGHT) &&  (rt->stereo_mode & PR_STEREO_SWAP)) ?  \
          rt->pr->stereo_pixbuf_offset_right : rt->pr->stereo_pixbuf_offset_left )

#define GET_LEFT_PIXBUF_OFFSET(rt) \
        ((!(rt->stereo_mode & PR_STEREO_RIGHT) && !(rt->stereo_mode & PR_STEREO_SWAP)) || \
         ( (rt->stereo_mode & PR_STEREO_RIGHT) &&  (rt->stereo_mode & PR_STEREO_SWAP)) ?  \
          rt->pr->stereo_pixbuf_offset_right : rt->pr->stereo_pixbuf_offset_left )


static void rt_sync_scroll(RendererTiles *rt)
{
	PixbufRenderer *pr = rt->pr;

	rt->x_scroll = (rt->stereo_mode & PR_STEREO_MIRROR) ?
	               pr->width - pr->vis_width - pr->x_scroll
	               : pr->x_scroll;

	rt->y_scroll = (rt->stereo_mode & PR_STEREO_FLIP) ?
	               pr->height - pr->vis_height - pr->y_scroll
	               : pr->y_scroll;
}

/*
 *-------------------------------------------------------------------
 * borders
 *-------------------------------------------------------------------
 */

static void rt_border_draw(RendererTiles *rt, gint x, gint y, gint w, gint h)
{
	PixbufRenderer *pr = rt->pr;
	GtkWidget *box;
	GdkWindow *window;
	gint rx, ry, rw, rh;
	cairo_t *cr;

	box = GTK_WIDGET(pr);
	window = gtk_widget_get_window(box);

	if (!window) return;

	cr = gdk_cairo_create(window);


	if (!pr->pixbuf && !pr->source_tiles_enabled)
		{
		if (pr_clip_region(x, y, w, h,
				   0, 0,
				   pr->viewport_width, pr->viewport_height,
				   &rx, &ry, &rw, &rh))
			{
			cairo_set_source_rgb(cr, (double)pr->color.red/65535, (double)pr->color.green/65535, (double)pr->color.blue/65535);
			cairo_rectangle(cr, rx + rt->stereo_off_x, ry + rt->stereo_off_y, rw, rh);
			cairo_fill(cr);
			rt_overlay_draw(rt, rx, ry, rw, rh, NULL);
			}
		cairo_destroy(cr);
		return;
		}

	if (pr->vis_width < pr->viewport_width)
		{
		if (pr->x_offset > 0 &&
		    pr_clip_region(x, y, w, h,
				   0, 0,
				   pr->x_offset, pr->viewport_height,
				   &rx, &ry, &rw, &rh))
			{
			cairo_set_source_rgb(cr, (double)pr->color.red/65535, (double)pr->color.green/65535, (double)pr->color.blue/65535);
			cairo_rectangle(cr, rx + rt->stereo_off_x, ry + rt->stereo_off_y, rw, rh);
			cairo_fill(cr);
			rt_overlay_draw(rt, rx, ry, rw, rh, NULL);
			}
		if (pr->viewport_width - pr->vis_width - pr->x_offset > 0 &&
		    pr_clip_region(x, y, w, h,
				   pr->x_offset + pr->vis_width, 0,
				   pr->viewport_width - pr->vis_width - pr->x_offset, pr->viewport_height,
				   &rx, &ry, &rw, &rh))
			{
			cairo_set_source_rgb(cr, (double)pr->color.red/65535, (double)pr->color.green/65535, (double)pr->color.blue/65535);
			cairo_rectangle(cr, rx + rt->stereo_off_x, ry + rt->stereo_off_y, rw, rh);
			cairo_fill(cr);
			rt_overlay_draw(rt, rx, ry, rw, rh, NULL);
			}
		}
	if (pr->vis_height < pr->viewport_height)
		{
		if (pr->y_offset > 0 &&
		    pr_clip_region(x, y, w, h,
				   pr->x_offset, 0,
				   pr->vis_width, pr->y_offset,
				   &rx, &ry, &rw, &rh))
			{
			cairo_set_source_rgb(cr, (double)pr->color.red/65535, (double)pr->color.green/65535, (double)pr->color.blue/65535);
			cairo_rectangle(cr, rx + rt->stereo_off_x, ry + rt->stereo_off_y, rw, rh);
			cairo_fill(cr);
			rt_overlay_draw(rt, rx, ry, rw, rh, NULL);
			}
		if (pr->viewport_height - pr->vis_height - pr->y_offset > 0 &&
		    pr_clip_region(x, y, w, h,
				   pr->x_offset, pr->y_offset + pr->vis_height,
				   pr->vis_width, pr->viewport_height - pr->vis_height - pr->y_offset,
				   &rx, &ry, &rw, &rh))
			{
			cairo_set_source_rgb(cr, (double)pr->color.red/65535, (double)pr->color.green/65535, (double)pr->color.blue/65535);
			cairo_rectangle(cr, rx + rt->stereo_off_x, ry + rt->stereo_off_y, rw, rh);
			cairo_fill(cr);
			rt_overlay_draw(rt, rx, ry, rw, rh, NULL);
			}
		}
	cairo_destroy(cr);
}

static void rt_border_clear(RendererTiles *rt)
{
	PixbufRenderer *pr = rt->pr;
	rt_border_draw(rt, 0, 0, pr->viewport_width, pr->viewport_height);
}


/*
 *-------------------------------------------------------------------
 * display tiles
 *-------------------------------------------------------------------
 */

static ImageTile *rt_tile_new(gint x, gint y, gint width, gint height)
{
	ImageTile *it;

	it = g_new0(ImageTile, 1);

	it->x = x;
	it->y = y;
	it->w = width;
	it->h = height;

	it->render_done = TILE_RENDER_NONE;

	return it;
}

static void rt_tile_free(ImageTile *it)
{
	if (!it) return;

	if (it->pixbuf) g_object_unref(it->pixbuf);
	if (it->surface) cairo_surface_destroy(it->surface);

	g_free(it);
}

static void rt_tile_free_all(RendererTiles *rt)
{
	GList *work;

	work = rt->tiles;
	while (work)
		{
		ImageTile *it;

		it = work->data;
		work = work->next;

		rt_tile_free(it);
		}

	g_list_free(rt->tiles);
	rt->tiles = NULL;
	rt->tile_cache_size = 0;
}

static ImageTile *rt_tile_add(RendererTiles *rt, gint x, gint y)
{
	PixbufRenderer *pr = rt->pr;
	ImageTile *it;

	it = rt_tile_new(x, y, rt->tile_width, rt->tile_height);

	if (it->x + it->w > pr->width) it->w = pr->width - it->x;
	if (it->y + it->h > pr->height) it->h = pr->height - it->y;

	rt->tiles = g_list_prepend(rt->tiles, it);
	rt->tile_cache_size += it->size;

	return it;
}

static void rt_tile_remove(RendererTiles *rt, ImageTile *it)
{
	if (it->qd)
		{
		QueueData *qd = it->qd;

		it->qd = NULL;
		rt->draw_queue = g_list_remove(rt->draw_queue, qd);
		g_free(qd);
		}

	if (it->qd2)
		{
		QueueData *qd = it->qd2;

		it->qd2 = NULL;
		rt->draw_queue_2pass = g_list_remove(rt->draw_queue_2pass, qd);
		g_free(qd);
		}

	rt->tiles = g_list_remove(rt->tiles, it);
	rt->tile_cache_size -= it->size;

	rt_tile_free(it);
}

static void rt_tile_free_space(RendererTiles *rt, guint space, ImageTile *it)
{
	PixbufRenderer *pr = rt->pr;
	GList *work;
	guint tile_max;

	work = g_list_last(rt->tiles);

	if (pr->source_tiles_enabled && pr->scale < 1.0)
		{
		gint tiles;

		tiles = (pr->vis_width / rt->tile_width + 1) * (pr->vis_height / rt->tile_height + 1);
		tile_max = MAX(tiles * rt->tile_width * rt->tile_height * 3,
			       (gint)((gdouble)rt->tile_cache_max * 1048576.0 * pr->scale));
		}
	else
		{
		tile_max = rt->tile_cache_max * 1048576;
		}

	while (work && rt->tile_cache_size + space > tile_max)
		{
		ImageTile *needle;

		needle = work->data;
		work = work->prev;
		if (needle != it &&
		    ((!needle->qd && !needle->qd2) || !rt_tile_is_visible(rt, needle))) rt_tile_remove(rt, needle);
		}
}

static void rt_tile_invalidate_all(RendererTiles *rt)
{
	PixbufRenderer *pr = rt->pr;
	GList *work;

	work = rt->tiles;
	while (work)
		{
		ImageTile *it;

		it = work->data;
		work = work->next;

		it->render_done = TILE_RENDER_NONE;
		it->render_todo = TILE_RENDER_ALL;
		it->blank = FALSE;

		it->w = MIN(rt->tile_width, pr->width - it->x);
		it->h = MIN(rt->tile_height, pr->height - it->y);
		}
}

static void rt_tile_invalidate_region(RendererTiles *rt, gint x, gint y, gint w, gint h)
{
	gint x1, x2;
	gint y1, y2;
	GList *work;

	x1 = ROUND_DOWN(x, rt->tile_width);
	x2 = ROUND_UP(x + w, rt->tile_width);

	y1 = ROUND_DOWN(y, rt->tile_height);
	y2 = ROUND_UP(y + h, rt->tile_height);

	work = rt->tiles;
	while (work)
		{
		ImageTile *it;

		it = work->data;
		work = work->next;

		if (it->x < x2 && it->x + it->w > x1 &&
		    it->y < y2 && it->y + it->h > y1)
			{
			it->render_done = TILE_RENDER_NONE;
			it->render_todo = TILE_RENDER_ALL;
			}
		}
}

static ImageTile *rt_tile_get(RendererTiles *rt, gint x, gint y, gboolean only_existing)
{
	GList *work;

	work = rt->tiles;
	while (work)
		{
		ImageTile *it;

		it = work->data;
		if (it->x == x && it->y == y)
			{
			rt->tiles = g_list_delete_link(rt->tiles, work);
			rt->tiles = g_list_prepend(rt->tiles, it);
			return it;
			}

		work = work->next;
		}

	if (only_existing) return NULL;

	return rt_tile_add(rt, x, y);
}

static gint pixmap_calc_size(cairo_surface_t *surface)
{
//	gint w, h, d;

//	d = gdk_drawable_get_depth(pixmap);
//	gdk_drawable_get_size(pixmap, &w, &h);
	return PR_TILE_SIZE * PR_TILE_SIZE * 4 / 8;
}

static void rt_tile_prepare(RendererTiles *rt, ImageTile *it)
{
	PixbufRenderer *pr = rt->pr;
	if (!it->surface)
		{
		cairo_surface_t *surface;
		guint size;

		surface = gdk_window_create_similar_surface(gtk_widget_get_window((GtkWidget *)pr),
		                                            CAIRO_CONTENT_COLOR,
		                                            rt->tile_width, rt->tile_height);

		size = pixmap_calc_size(surface);
		rt_tile_free_space(rt, size, it);

		it->surface = surface;
		it->size += size;
		rt->tile_cache_size += size;
		}

	if (!it->pixbuf)
		{
		GdkPixbuf *pixbuf;
		guint size;
		pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, rt->tile_width, rt->tile_height);

		size = gdk_pixbuf_get_rowstride(pixbuf) * rt->tile_height;
		rt_tile_free_space(rt, size, it);

		it->pixbuf = pixbuf;
		it->size += size;
		rt->tile_cache_size += size;
		}
}

/*
 *-------------------------------------------------------------------
 * overlays
 *-------------------------------------------------------------------
 */

static void rt_overlay_get_position(RendererTiles *rt, OverlayData *od,
				    gint *x, gint *y, gint *w, gint *h)
{
	PixbufRenderer *pr = rt->pr;
	gint px, py, pw, ph;

	pw = gdk_pixbuf_get_width(od->pixbuf);
	ph = gdk_pixbuf_get_height(od->pixbuf);
	px = od->x;
	py = od->y;

	if (od->flags & OVL_RELATIVE)
		{
		if (px < 0) px = pr->viewport_width - pw + px;
		if (py < 0) py = pr->viewport_height - ph + py;
		}

	if (x) *x = px;
	if (y) *y = py;
	if (w) *w = pw;
	if (h) *h = ph;
}

static void rt_overlay_init_window(RendererTiles *rt, OverlayData *od)
{
	PixbufRenderer *pr = rt->pr;
	gint px, py, pw, ph;
	GdkWindowAttr attributes;
	gint attributes_mask;

	rt_overlay_get_position(rt, od, &px, &py, &pw, &ph);

	attributes.window_type = GDK_WINDOW_CHILD;
	attributes.wclass = GDK_INPUT_OUTPUT;
	attributes.width = pw;
	attributes.height = ph;
	attributes.event_mask = GDK_EXPOSURE_MASK;
	attributes_mask = 0;

	od->window = gdk_window_new(gtk_widget_get_window(GTK_WIDGET(pr)), &attributes, attributes_mask);
	gdk_window_set_user_data(od->window, pr);
	gdk_window_move(od->window, px + rt->stereo_off_x, py + rt->stereo_off_y);
	gdk_window_show(od->window);
}

static void rt_overlay_draw(RendererTiles *rt, gint x, gint y, gint w, gint h,
			    ImageTile *it)
{
	PixbufRenderer *pr = rt->pr;
	GList *work;

	work = rt->overlay_list;
	while (work)
		{
		OverlayData *od;
		gint px, py, pw, ph;
		gint rx, ry, rw, rh;

		od = work->data;
		work = work->next;

		if (!od->window) rt_overlay_init_window(rt, od);

		rt_overlay_get_position(rt, od, &px, &py, &pw, &ph);
		if (pr_clip_region(x, y, w, h, px, py, pw, ph, &rx, &ry, &rw, &rh))
			{
			if (!rt->overlay_buffer)
				{
				rt->overlay_buffer = gdk_window_create_similar_surface(gtk_widget_get_window((GtkWidget *)pr),
		                                            CAIRO_CONTENT_COLOR,
		                                            rt->tile_width, rt->tile_height);
				}

			if (it)
				{
				cairo_t *cr;

				cr = cairo_create(rt->overlay_buffer);
				cairo_set_source_surface(cr, it->surface, (pr->x_offset + (it->x - rt->x_scroll)) - rx, (pr->y_offset + (it->y - rt->y_scroll)) - ry);
				cairo_rectangle(cr, 0, 0, rw, rh);
				cairo_fill_preserve(cr);

				cairo_surface_t *surface;
				surface = gdk_cairo_surface_create_from_pixbuf (od->pixbuf, rt->hidpi_scale, NULL);
				cairo_set_source_surface (cr, surface, px - rx, py - ry);

				cairo_fill (cr);
				cairo_destroy (cr);
				cairo_surface_destroy (surface);

				cr = gdk_cairo_create(od->window);
				cairo_set_source_surface(cr, rt->overlay_buffer, rx - px, ry - py);
				cairo_rectangle (cr, rx - px, ry - py, rw, rh);
				cairo_fill (cr);
				cairo_destroy (cr);
				}
			else
				{
				/* no ImageTile means region may be larger than our scratch buffer */
				gint sx, sy;

				for (sx = rx; sx < rx + rw; sx += rt->tile_width)
				    for (sy = ry; sy < ry + rh; sy += rt->tile_height)
					{
					gint sw, sh;
					cairo_t *cr;

					sw = MIN(rx + rw - sx, rt->tile_width);
					sh = MIN(ry + rh - sy, rt->tile_height);

					cr = cairo_create(rt->overlay_buffer);
					cairo_set_source_rgb(cr, 0, 0, 0);
					cairo_rectangle(cr, 0, 0, sw, sh);
					cairo_fill_preserve(cr);

					cairo_surface_t *surface;
					surface = gdk_cairo_surface_create_from_pixbuf (od->pixbuf, rt->hidpi_scale, NULL);
					cairo_set_source_surface (cr, surface, px - sx, py - sy);

					cairo_fill (cr);
					cairo_destroy (cr);
					cairo_surface_destroy (surface);

					cr = gdk_cairo_create(od->window);
					cairo_set_source_surface(cr, rt->overlay_buffer, sx - px, sy - py);
					cairo_rectangle (cr, sx - px, sy - py, sw, sh);
					cairo_fill(cr);
					cairo_destroy(cr);
					}
				}
			}
		}
}

static void rt_overlay_queue_draw(RendererTiles *rt, OverlayData *od, gint x1, gint y1, gint x2, gint y2)
{
	PixbufRenderer *pr = rt->pr;
	gint x, y, w, h;

	rt_overlay_get_position(rt, od, &x, &y, &w, &h);

	/* add borders */
	x -= x1;
	y -= y1;
	w += x1 + x2;
	h += y1 + y2;

	rt_queue(rt, rt->x_scroll - pr->x_offset + x,
		 rt->y_scroll - pr->y_offset + y,
		 w, h,
		 FALSE, TILE_RENDER_ALL, FALSE, FALSE);

	rt_border_draw(rt, x, y, w, h);
}

static void rt_overlay_queue_all(RendererTiles *rt, gint x1, gint y1, gint x2, gint y2)
{
	GList *work;

	work = rt->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;

		rt_overlay_queue_draw(rt, od, x1, y1, x2, y2);
		}
}

static void rt_overlay_update_sizes(RendererTiles *rt)
{
	GList *work;

	work = rt->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;

		if (!od->window) rt_overlay_init_window(rt, od);

		if (od->flags & OVL_RELATIVE)
			{
			gint x, y, w, h;

			rt_overlay_get_position(rt, od, &x, &y, &w, &h);
			gdk_window_move_resize(od->window, x + rt->stereo_off_x, y + rt->stereo_off_y, w, h);
			}
		}
}

static OverlayData *rt_overlay_find(RendererTiles *rt, gint id)
{
	GList *work;

	work = rt->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;

		if (od->id == id) return od;
		}

	return NULL;
}


gint renderer_tiles_overlay_add(void *renderer, GdkPixbuf *pixbuf, gint x, gint y,
				 OverlayRendererFlags flags)
{
	RendererTiles *rt = (RendererTiles *) renderer;
	PixbufRenderer *pr = rt->pr;
	OverlayData *od;
	gint id;

	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), -1);
	g_return_val_if_fail(pixbuf != NULL, -1);

	id = 1;
	while (rt_overlay_find(rt, id)) id++;

	od = g_new0(OverlayData, 1);
	od->id = id;
	od->pixbuf = pixbuf;
	g_object_ref(G_OBJECT(od->pixbuf));
	od->x = x;
	od->y = y;
	od->flags = flags;

	rt_overlay_init_window(rt, od);

	rt->overlay_list = g_list_append(rt->overlay_list, od);

	rt_overlay_queue_draw(rt, od, 0, 0, 0, 0);

	return od->id;
}

static void rt_overlay_free(RendererTiles *rt, OverlayData *od)
{
	rt->overlay_list = g_list_remove(rt->overlay_list, od);

	if (od->pixbuf) g_object_unref(G_OBJECT(od->pixbuf));
	if (od->window) gdk_window_destroy(od->window);
	g_free(od);

	if (!rt->overlay_list && rt->overlay_buffer)
		{
		cairo_surface_destroy(rt->overlay_buffer);
		rt->overlay_buffer = NULL;
		}
}

static void rt_overlay_list_clear(RendererTiles *rt)
{
	while (rt->overlay_list)
		{
		OverlayData *od;

		od = rt->overlay_list->data;
		rt_overlay_free(rt, od);
		}
}

static void rt_overlay_list_reset_window(RendererTiles *rt)
{
	GList *work;

	if (rt->overlay_buffer) cairo_surface_destroy(rt->overlay_buffer);
	rt->overlay_buffer = NULL;

	work = rt->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;
		if (od->window) gdk_window_destroy(od->window);
		od->window = NULL;
		}
}

void renderer_tiles_overlay_set(void *renderer, gint id, GdkPixbuf *pixbuf, gint x, gint y)
{
	RendererTiles *rt = (RendererTiles *) renderer;
	PixbufRenderer *pr = rt->pr;
	OverlayData *od;

	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	od = rt_overlay_find(rt, id);
	if (!od) return;

	if (pixbuf)
		{
		gint px, py, pw, ph;

		g_object_ref(G_OBJECT(pixbuf));
		g_object_unref(G_OBJECT(od->pixbuf));
		od->pixbuf = pixbuf;

		od->x = x;
		od->y = y;

		if (!od->window) rt_overlay_init_window(rt, od);

		rt_overlay_queue_draw(rt, od, 0, 0, 0, 0);
		rt_overlay_get_position(rt, od, &px, &py, &pw, &ph);
		gdk_window_move_resize(od->window, px + rt->stereo_off_x, py + rt->stereo_off_y, pw, ph);
		}
	else
		{
		rt_overlay_queue_draw(rt, od, 0, 0, 0, 0);
		rt_overlay_free(rt, od);
		}
}

gboolean renderer_tiles_overlay_get(void *renderer, gint id, GdkPixbuf **pixbuf, gint *x, gint *y)
{
	RendererTiles *rt = (RendererTiles *) renderer;
	PixbufRenderer *pr = rt->pr;
	OverlayData *od;

	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);

	od = rt_overlay_find(rt, id);
	if (!od) return FALSE;

	if (pixbuf) *pixbuf = od->pixbuf;
	if (x) *x = od->x;
	if (y) *y = od->y;

	return TRUE;
}

static void rt_hierarchy_changed_cb(GtkWidget *widget, GtkWidget *previous_toplevel, gpointer data)
{
	RendererTiles *rt = data;
	rt_overlay_list_reset_window(rt);
}

/*
 *-------------------------------------------------------------------
 * drawing
 *-------------------------------------------------------------------
 */

static GdkPixbuf *rt_get_spare_tile(RendererTiles *rt)
{
	if (!rt->spare_tile) rt->spare_tile = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, rt->tile_width, rt->tile_height);
	return rt->spare_tile;
}

#define COLOR_BYTES 3	/* rgb */

static void rt_tile_rotate_90_clockwise(RendererTiles *rt, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *ip, *spi, *dpi;
	gint i, j;
	gint tw = rt->tile_width;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);
	spi = s_pix + (x * COLOR_BYTES);

	dest = rt_get_spare_tile(rt);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi = d_pix + (tw - 1) * COLOR_BYTES;

	for (i = y; i < y + h; i++)
		{
		sp = spi + (i * srs);
		ip = dpi - (i * COLOR_BYTES);
		for (j = x; j < x + w; j++)
			{
			dp = ip + (j * drs);
			memcpy(dp, sp, COLOR_BYTES);
			sp += COLOR_BYTES;
			}
		}

	rt->spare_tile = src;
	*tile = dest;
}

static void rt_tile_rotate_90_counter_clockwise(RendererTiles *rt, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *ip, *spi, *dpi;
	gint i, j;
	gint th = rt->tile_height;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);
	spi = s_pix + (x * COLOR_BYTES);

	dest = rt_get_spare_tile(rt);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi = d_pix + (th - 1) * drs;

	for (i = y; i < y + h; i++)
		{
		sp = spi + (i * srs);
		ip = dpi + (i * COLOR_BYTES);
		for (j = x; j < x + w; j++)
			{
			dp = ip - (j * drs);
			memcpy(dp, sp, COLOR_BYTES);
			sp += COLOR_BYTES;
			}
		}

	rt->spare_tile = src;
	*tile = dest;
}

static void rt_tile_mirror_only(RendererTiles *rt, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *spi, *dpi;
	gint i, j;

	gint tw = rt->tile_width;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);
	spi = s_pix + (x * COLOR_BYTES);

	dest = rt_get_spare_tile(rt);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi =  d_pix + (tw - x - 1) * COLOR_BYTES;

	for (i = y; i < y + h; i++)
		{
		sp = spi + (i * srs);
		dp = dpi + (i * drs);
		for (j = 0; j < w; j++)
			{
			memcpy(dp, sp, COLOR_BYTES);
			sp += COLOR_BYTES;
			dp -= COLOR_BYTES;
			}
		}

	rt->spare_tile = src;
	*tile = dest;
}

static void rt_tile_mirror_and_flip(RendererTiles *rt, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *dpi;
	gint i, j;
	gint tw = rt->tile_width;
	gint th = rt->tile_height;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);

	dest = rt_get_spare_tile(rt);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi = d_pix + (th - 1) * drs + (tw - 1) * COLOR_BYTES;

	for (i = y; i < y + h; i++)
		{
		sp = s_pix + (i * srs) + (x * COLOR_BYTES);
		dp = dpi - (i * drs) - (x * COLOR_BYTES);
		for (j = 0; j < w; j++)
			{
			memcpy(dp, sp, COLOR_BYTES);
			sp += COLOR_BYTES;
			dp -= COLOR_BYTES;
			}
		}

	rt->spare_tile = src;
	*tile = dest;
}

static void rt_tile_flip_only(RendererTiles *rt, GdkPixbuf **tile, gint x, gint y, gint w, gint h)
{
	GdkPixbuf *src = *tile;
	GdkPixbuf *dest;
	gint srs, drs;
	guchar *s_pix, *d_pix;
	guchar *sp, *dp;
	guchar *spi, *dpi;
	gint i;
	gint th = rt->tile_height;

	srs = gdk_pixbuf_get_rowstride(src);
	s_pix = gdk_pixbuf_get_pixels(src);
	spi = s_pix + (x * COLOR_BYTES);

	dest = rt_get_spare_tile(rt);
	drs = gdk_pixbuf_get_rowstride(dest);
	d_pix = gdk_pixbuf_get_pixels(dest);
	dpi = d_pix + (th - 1) * drs + (x * COLOR_BYTES);

	for (i = y; i < y + h; i++)
		{
		sp = spi + (i * srs);
		dp = dpi - (i * drs);
		memcpy(dp, sp, w * COLOR_BYTES);
		}

	rt->spare_tile = src;
	*tile = dest;
}

static void rt_tile_apply_orientation(RendererTiles *rt, gint orientation, GdkPixbuf **pixbuf, gint x, gint y, gint w, gint h)
{
	switch (orientation)
		{
		case EXIF_ORIENTATION_TOP_LEFT:
			/* normal -- nothing to do */
			break;
		case EXIF_ORIENTATION_TOP_RIGHT:
			/* mirrored */
			{
				rt_tile_mirror_only(rt, pixbuf, x, y, w, h);
			}
			break;
		case EXIF_ORIENTATION_BOTTOM_RIGHT:
			/* upside down */
			{
				rt_tile_mirror_and_flip(rt, pixbuf, x, y, w, h);
			}
			break;
		case EXIF_ORIENTATION_BOTTOM_LEFT:
			/* flipped */
			{
				rt_tile_flip_only(rt, pixbuf, x, y, w, h);
			}
			break;
		case EXIF_ORIENTATION_LEFT_TOP:
			{
				rt_tile_flip_only(rt, pixbuf, x, y, w, h);
				rt_tile_rotate_90_clockwise(rt, pixbuf, x, rt->tile_height - y - h, w, h);
			}
			break;
		case EXIF_ORIENTATION_RIGHT_TOP:
			/* rotated -90 (270) */
			{
				rt_tile_rotate_90_clockwise(rt, pixbuf, x, y, w, h);
			}
			break;
		case EXIF_ORIENTATION_RIGHT_BOTTOM:
			{
				rt_tile_flip_only(rt, pixbuf, x, y, w, h);
				rt_tile_rotate_90_counter_clockwise(rt, pixbuf, x, rt->tile_height - y - h, w, h);
			}
			break;
		case EXIF_ORIENTATION_LEFT_BOTTOM:
			/* rotated 90 */
			{
				rt_tile_rotate_90_counter_clockwise(rt, pixbuf, x, y, w, h);
			}
			break;
		default:
			/* The other values are out of range */
			break;
		}
}

static gboolean rt_source_tile_render(RendererTiles *rt, ImageTile *it,
				      gint x, gint y, gint w, gint h,
				      gboolean new_data, gboolean fast)
{
	PixbufRenderer *pr = rt->pr;
	GList *list;
	GList *work;
	gboolean draw = FALSE;

	if (pr->zoom == 1.0 || pr->scale == 1.0)
		{
		list = pr_source_tile_compute_region(pr, it->x + x, it->y + y, w, h, TRUE);
		work = list;
		while (work)
			{
			SourceTile *st;
			gint rx, ry, rw, rh;

			st = work->data;
			work = work->next;

			if (pr_clip_region(st->x, st->y, pr->source_tile_width, pr->source_tile_height,
					   it->x + x, it->y + y, w, h,
					   &rx, &ry, &rw, &rh))
				{
				cairo_t *cr;
				cr = cairo_create(it->surface);
				cairo_rectangle (cr, rx - it->x, ry - it->y, rw, rh);

				if (st->blank)
					{
					cairo_set_source_rgb(cr, 0, 0, 0);
					cairo_fill (cr);
					cairo_destroy (cr);
					}
				else /* (pr->zoom == 1.0 || pr->scale == 1.0) */
					{
					cairo_surface_t *surface;
					surface = gdk_cairo_surface_create_from_pixbuf (st->pixbuf, 2, NULL);
					cairo_set_source_surface (cr, surface, -it->x + st->x, -it->y + st->y);

					cairo_fill (cr);
					cairo_destroy (cr);
					cairo_surface_destroy (surface);
					}
				}
			}
		}
	else
		{
		gdouble scale_x, scale_y;
		gint sx, sy, sw, sh;

		if (pr->image_width == 0 || pr->image_height == 0) return FALSE;
		scale_x = (gdouble)pr->width / pr->image_width;
		scale_y = (gdouble)pr->height / pr->image_height;

		sx = (gdouble)(it->x + x) / scale_x;
		sy = (gdouble)(it->y + y) / scale_y;
		sw = (gdouble)w / scale_x;
		sh = (gdouble)h / scale_y;

		if (pr->width < PR_MIN_SCALE_SIZE || pr->height < PR_MIN_SCALE_SIZE) fast = TRUE;

#if 0
		/* draws red over draw region, to check for leaks (regions not filled) */
		pixbuf_set_rect_fill(it->pixbuf, x, y, w, h, 255, 0, 0, 255);
#endif

		list = pr_source_tile_compute_region(pr, sx, sy, sw, sh, TRUE);
		work = list;
		while (work)
			{
			SourceTile *st;
			gint rx, ry, rw, rh;
			gint stx, sty, stw, sth;

			st = work->data;
			work = work->next;

			stx = floor((gdouble)st->x * scale_x);
			sty = floor((gdouble)st->y * scale_y);
			stw = ceil((gdouble)(st->x + pr->source_tile_width) * scale_x) - stx;
			sth = ceil((gdouble)(st->y + pr->source_tile_height) * scale_y) - sty;

			if (pr_clip_region(stx, sty, stw, sth,
					   it->x + x, it->y + y, w, h,
					   &rx, &ry, &rw, &rh))
				{

				if (st->blank)
					{
					cairo_t *cr;
					cr = cairo_create(it->surface);
					cairo_rectangle (cr, rx - st->x, ry - st->y, rw, rh);
					cairo_set_source_rgb(cr, 0, 0, 0);
					cairo_fill (cr);
					cairo_destroy (cr);
					}
				else
					{
					gdouble offset_x;
					gdouble offset_y;

					/* may need to use unfloored stx,sty values here */
					offset_x = (gdouble)(stx - it->x);
					offset_y = (gdouble)(sty - it->y);

					gdk_pixbuf_scale(st->pixbuf, it->pixbuf, rx - it->x, ry - it->y, rw, rh,
						 (gdouble) 0.0 + offset_x,
						 (gdouble) 0.0 + offset_y,
						 scale_x, scale_y,
						 (fast) ? GDK_INTERP_NEAREST : pr->zoom_quality);
					draw = TRUE;
					}
				}
			}
		}

	g_list_free(list);

	return draw;
}

static void rt_tile_get_region(gboolean has_alpha,
                               const GdkPixbuf *src, GdkPixbuf *dest,
                               int pb_x, int pb_y, int pb_w, int pb_h,
                               double offset_x, double offset_y, double scale_x, double scale_y,
                               GdkInterpType interp_type,
                               int check_x, int check_y)
{
	if (!has_alpha)
		{
		if (scale_x == 1.0 && scale_y == 1.0)
			{
			gdk_pixbuf_copy_area(src,
					     -offset_x + pb_x, -offset_y + pb_y,
					     pb_w, pb_h,
					     dest,
					     pb_x, pb_y);
			}
		else
			{
			gdk_pixbuf_scale(src, dest,
					 pb_x, pb_y, pb_w, pb_h,
					 offset_x,
					 offset_y,
					 scale_x, scale_y,
					 interp_type);
			}
		}
	else
		{
		gdk_pixbuf_composite_color(src, dest,
					 pb_x, pb_y, pb_w, pb_h,
					 offset_x,
					 offset_y,
					 scale_x, scale_y,
					 (scale_x == 1.0 && scale_y == 1.0) ? GDK_INTERP_NEAREST : interp_type,
					 255, check_x, check_y,
					 PR_ALPHA_CHECK_SIZE,
					 ((options->image.alpha_color_1.red << 8 & 0x00FF0000) +
					 (options->image.alpha_color_1.green & 0x00FF00) +
					 (options->image.alpha_color_1.blue >> 8 & 0x00FF)),
					 ((options->image.alpha_color_2.red << 8 & 0x00FF0000) +
					 (options->image.alpha_color_2.green & 0x00FF00) +
					 (options->image.alpha_color_2.blue >> 8 & 0x00FF)));
		}
}


static gint rt_get_orientation(RendererTiles *rt)
{
	PixbufRenderer *pr = rt->pr;

	gint orientation = pr->orientation;
	static const gint mirror[]       = {1,   2, 1, 4, 3, 6, 5, 8, 7};
	static const gint flip[]         = {1,   4, 3, 2, 1, 8, 7, 6, 5};

	if (rt->stereo_mode & PR_STEREO_MIRROR) orientation = mirror[orientation];
	if (rt->stereo_mode & PR_STEREO_FLIP) orientation = flip[orientation];
        return orientation;
}


static void rt_tile_render(RendererTiles *rt, ImageTile *it,
			   gint x, gint y, gint w, gint h,
			   gboolean new_data, gboolean fast)
{
	PixbufRenderer *pr = rt->pr;
	gboolean has_alpha;
	gboolean draw = FALSE;
	gint orientation = rt_get_orientation(rt);

	if (it->render_todo == TILE_RENDER_NONE && it->surface && !new_data) return;

	if (it->render_done != TILE_RENDER_ALL)
		{
		x = 0;
		y = 0;
		w = it->w;
		h = it->h;
		if (!fast) it->render_done = TILE_RENDER_ALL;
		}
	else if (it->render_todo != TILE_RENDER_AREA)
		{
		if (!fast) it->render_todo = TILE_RENDER_NONE;
		return;
		}

	if (!fast) it->render_todo = TILE_RENDER_NONE;

	if (new_data) it->blank = FALSE;

	rt_tile_prepare(rt, it);
	has_alpha = (pr->pixbuf && gdk_pixbuf_get_has_alpha(pr->pixbuf));

	/* FIXME checker colors for alpha should be configurable,
	 * also should be drawn for blank = TRUE
	 */

	if (it->blank)
		{
		/* no data, do fast rect fill */
		cairo_t *cr;
		cr = cairo_create(it->surface);
		cairo_rectangle (cr, 0, 0, it->w, it->h);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_fill (cr);
		cairo_destroy (cr);
		}
	else if (pr->source_tiles_enabled)
		{
		draw = rt_source_tile_render(rt, it, x, y, w, h, new_data, fast);
		}
	else
		{
		gdouble scale_x, scale_y;
		gdouble src_x, src_y;
		gint pb_x, pb_y;
		gint pb_w, pb_h;

		if (pr->image_width == 0 || pr->image_height == 0) return;

		scale_x = (gdouble)pr->width / pr->image_width;
		scale_y = (gdouble)pr->height / pr->image_height;

		pr_tile_coords_map_orientation(orientation, it->x, it->y,
					    pr->width, pr->height,
					    rt->tile_width, rt->tile_height,
					    &src_x, &src_y);
		pr_tile_region_map_orientation(orientation, x, y,
					    rt->tile_width, rt->tile_height,
					    w, h,
					    &pb_x, &pb_y,
					    &pb_w, &pb_h);

		switch (orientation)
			{
			gdouble tmp;
			case EXIF_ORIENTATION_LEFT_TOP:
			case EXIF_ORIENTATION_RIGHT_TOP:
			case EXIF_ORIENTATION_RIGHT_BOTTOM:
			case EXIF_ORIENTATION_LEFT_BOTTOM:
				tmp = scale_x;
				scale_x = scale_y;
				scale_y = tmp;
				break;
			default:
				/* nothing to do */
				break;
			}

		/* HACK: The pixbuf scalers get kinda buggy(crash) with extremely
		 * small sizes for anything but GDK_INTERP_NEAREST
		 */
		if (pr->width < PR_MIN_SCALE_SIZE || pr->height < PR_MIN_SCALE_SIZE) fast = TRUE;

		rt_tile_get_region(has_alpha,
				   pr->pixbuf, it->pixbuf, pb_x, pb_y, pb_w, pb_h,
				   (gdouble) 0.0 - src_x - GET_RIGHT_PIXBUF_OFFSET(rt) * scale_x,
				   (gdouble) 0.0 - src_y,
				   scale_x, scale_y,
				   (fast) ? GDK_INTERP_NEAREST : pr->zoom_quality,
				   it->x + pb_x, it->y + pb_y);
		if (rt->stereo_mode & PR_STEREO_ANAGLYPH &&
		    (pr->stereo_pixbuf_offset_right > 0 || pr->stereo_pixbuf_offset_left > 0))
			{
			GdkPixbuf *right_pb = rt_get_spare_tile(rt);
			rt_tile_get_region(has_alpha,
					   pr->pixbuf, right_pb, pb_x, pb_y, pb_w, pb_h,
					   (gdouble) 0.0 - src_x - GET_LEFT_PIXBUF_OFFSET(rt) * scale_x,
					   (gdouble) 0.0 - src_y,
					   scale_x, scale_y,
					   (fast) ? GDK_INTERP_NEAREST : pr->zoom_quality,
					   it->x + pb_x, it->y + pb_y);
			pr_create_anaglyph(rt->stereo_mode, it->pixbuf, right_pb, pb_x, pb_y, pb_w, pb_h);
			/* do not care about freeing spare_tile, it will be reused */
			}
		rt_tile_apply_orientation(rt, orientation, &it->pixbuf, pb_x, pb_y, pb_w, pb_h);
		draw = TRUE;
		}

	if (draw && it->pixbuf && !it->blank)
		{
		cairo_t *cr;

		if (pr->func_post_process && !(pr->post_process_slow && fast))
			pr->func_post_process(pr, &it->pixbuf, x, y, w, h, pr->post_process_user_data);

		cr = cairo_create(it->surface);
		cairo_rectangle (cr, x, y, w, h);
		cairo_surface_t *surface;
		surface = gdk_cairo_surface_create_from_pixbuf (it->pixbuf, rt->hidpi_scale, NULL);
		cairo_set_source_surface (cr, surface, 0, 0);
		cairo_fill (cr);
		cairo_destroy (cr);
		cairo_surface_destroy (surface);
		}
}


static void rt_tile_expose(RendererTiles *rt, ImageTile *it,
			   gint x, gint y, gint w, gint h,
			   gboolean new_data, gboolean fast)
{
	PixbufRenderer *pr = rt->pr;
	GtkWidget *box;
	GdkWindow *window;
	cairo_t *cr;

	/* clamp to visible */
	if (it->x + x < rt->x_scroll)
		{
		w -= rt->x_scroll - it->x - x;
		x = rt->x_scroll - it->x;
		}
	if (it->x + x + w > rt->x_scroll + pr->vis_width)
		{
		w = rt->x_scroll + pr->vis_width - it->x - x;
		}
	if (w < 1) return;
	if (it->y + y < rt->y_scroll)
		{
		h -= rt->y_scroll - it->y - y;
		y = rt->y_scroll - it->y;
		}
	if (it->y + y + h > rt->y_scroll + pr->vis_height)
		{
		h = rt->y_scroll + pr->vis_height - it->y - y;
		}
	if (h < 1) return;

	rt_tile_render(rt, it, x, y, w, h, new_data, fast);

	box = GTK_WIDGET(pr);
	window = gtk_widget_get_window(box);

	cr = gdk_cairo_create(window);
	cairo_set_source_surface(cr, it->surface, pr->x_offset + (it->x - rt->x_scroll) + rt->stereo_off_x, pr->y_offset + (it->y - rt->y_scroll) + rt->stereo_off_y);
	cairo_rectangle (cr, pr->x_offset + (it->x - rt->x_scroll) + x + rt->stereo_off_x, pr->y_offset + (it->y - rt->y_scroll) + y + rt->stereo_off_y, w, h);
	cairo_fill (cr);
	cairo_destroy (cr);

	if (rt->overlay_list)
		{
		rt_overlay_draw(rt, pr->x_offset + (it->x - rt->x_scroll) + x,
				pr->y_offset + (it->y - rt->y_scroll) + y,
				w, h,
				it);
		}
}


static gboolean rt_tile_is_visible(RendererTiles *rt, ImageTile *it)
{
	PixbufRenderer *pr = rt->pr;
	return (it->x + it->w >= rt->x_scroll && it->x < rt->x_scroll + pr->vis_width &&
		it->y + it->h >= rt->y_scroll && it->y < rt->y_scroll + pr->vis_height);
}

/*
 *-------------------------------------------------------------------
 * draw queue
 *-------------------------------------------------------------------
 */

static gint rt_get_queued_area(GList *work)
{
	gint area = 0;

	while (work)
		{
		QueueData *qd = work->data;
		area += qd->w * qd->h;
		work = work->next;
		}
	return area;
}


static gboolean rt_queue_schedule_next_draw(RendererTiles *rt, gboolean force_set)
{
	PixbufRenderer *pr = rt->pr;
	gfloat percent;
	gint visible_area = pr->vis_width * pr->vis_height;

	if (!pr->loading)
		{
		/* 2pass prio */
		DEBUG_2("redraw priority: 2pass");
		rt->draw_idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, rt_queue_draw_idle_cb, rt, NULL);
		return FALSE;
		}

	if (visible_area == 0)
		{
		/* not known yet */
		percent = 100.0;
		}
	else
		{
		percent = 100.0 * rt_get_queued_area(rt->draw_queue) / visible_area;
		}

	if (percent > 10.0)
		{
		/* we have enough data for starting intensive redrawing */
		DEBUG_2("redraw priority: high %.2f %%", percent);
		rt->draw_idle_id = g_idle_add_full(GDK_PRIORITY_REDRAW, rt_queue_draw_idle_cb, rt, NULL);
		return FALSE;
		}

	if (percent < 1.0 || force_set)
		{
		/* queue is (almost) empty, wait  50 ms*/
		DEBUG_2("redraw priority: wait %.2f %%", percent);
		rt->draw_idle_id = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, 50, rt_queue_draw_idle_cb, rt, NULL);
		return FALSE;
		}

	/* keep the same priority as before */
	DEBUG_2("redraw priority: no change %.2f %%", percent);
	return TRUE;
}


static gboolean rt_queue_draw_idle_cb(gpointer data)
{
	RendererTiles *rt = data;
	PixbufRenderer *pr = rt->pr;
	QueueData *qd;
	gboolean fast;


	if ((!pr->pixbuf && !pr->source_tiles_enabled) ||
	    (!rt->draw_queue && !rt->draw_queue_2pass) ||
	    !rt->draw_idle_id)
		{
		pr_render_complete_signal(pr);

		rt->draw_idle_id = 0;
		return FALSE;
		}

	if (rt->draw_queue)
		{
		qd = rt->draw_queue->data;
		fast = (pr->zoom_2pass && ((pr->zoom_quality != GDK_INTERP_NEAREST && pr->scale != 1.0) || pr->post_process_slow));
		}
	else
		{
		if (pr->loading)
			{
			/* still loading, wait till done (also drops the higher priority) */

			return rt_queue_schedule_next_draw(rt, FALSE);
			}

		qd = rt->draw_queue_2pass->data;
		fast = FALSE;
		}

	if (gtk_widget_get_realized(GTK_WIDGET(pr)))
		{
		if (rt_tile_is_visible(rt, qd->it))
			{
			rt_tile_expose(rt, qd->it, qd->x, qd->y, qd->w, qd->h, qd->new_data, fast);
			}
		else if (qd->new_data)
			{
			/* if new pixel data, and we already have a pixmap, update the tile */
			qd->it->blank = FALSE;
			if (qd->it->surface && qd->it->render_done == TILE_RENDER_ALL)
				{
				rt_tile_render(rt, qd->it, qd->x, qd->y, qd->w, qd->h, qd->new_data, fast);
				}
			}
		}

	if (rt->draw_queue)
		{
		qd->it->qd = NULL;
		rt->draw_queue = g_list_remove(rt->draw_queue, qd);
		if (fast)
			{
			if (qd->it->qd2)
				{
				rt_queue_merge(qd->it->qd2, qd);
				g_free(qd);
				}
			else
				{
				qd->it->qd2 = qd;
				rt->draw_queue_2pass = g_list_append(rt->draw_queue_2pass, qd);
				}
			}
		else
			{
			g_free(qd);
			}
		}
	else
		{
		qd->it->qd2 = NULL;
		rt->draw_queue_2pass = g_list_remove(rt->draw_queue_2pass, qd);
		g_free(qd);
		}

	if (!rt->draw_queue && !rt->draw_queue_2pass)
		{
		pr_render_complete_signal(pr);

		rt->draw_idle_id = 0;
		return FALSE;
		}

		return rt_queue_schedule_next_draw(rt, FALSE);
}

static void rt_queue_list_free(GList *list)
{
	GList *work;

	work = list;
	while (work)
		{
		QueueData *qd;

		qd = work->data;
		work = work->next;

		qd->it->qd = NULL;
		qd->it->qd2 = NULL;
		g_free(qd);
		}

	g_list_free(list);
}

static void rt_queue_clear(RendererTiles *rt)
{
	rt_queue_list_free(rt->draw_queue);
	rt->draw_queue = NULL;

	rt_queue_list_free(rt->draw_queue_2pass);
	rt->draw_queue_2pass = NULL;

	if (rt->draw_idle_id)
		{
		g_source_remove(rt->draw_idle_id);
		rt->draw_idle_id = 0;
		}
	rt_sync_scroll(rt);
}

static void rt_queue_merge(QueueData *parent, QueueData *qd)
{
	if (parent->x + parent->w < qd->x + qd->w)
		{
		parent->w += (qd->x + qd->w) - (parent->x + parent->w);
		}
	if (parent->x > qd->x)
		{
		parent->w += parent->x - qd->x;
		parent->x = qd->x;
		}

	if (parent->y + parent->h < qd->y + qd->h)
		{
		parent->h += (qd->y + qd->h) - (parent->y + parent->h);
		}
	if (parent->y > qd->y)
		{
		parent->h += parent->y - qd->y;
		parent->y = qd->y;
		}

	parent->new_data |= qd->new_data;
}

static gboolean rt_clamp_to_visible(RendererTiles *rt, gint *x, gint *y, gint *w, gint *h)
{
	PixbufRenderer *pr = rt->pr;
	gint nx, ny;
	gint nw, nh;
	gint vx, vy;
	gint vw, vh;

	vw = pr->vis_width;
	vh = pr->vis_height;

	vx = rt->x_scroll;
	vy = rt->y_scroll;

	if (*x + *w < vx || *x > vx + vw || *y + *h < vy || *y > vy + vh) return FALSE;

	/* now clamp it */
	nx = CLAMP(*x, vx, vx + vw);
	nw = CLAMP(*w - (nx - *x), 1, vw);

	ny = CLAMP(*y, vy, vy + vh);
	nh = CLAMP(*h - (ny - *y), 1, vh);

	*x = nx;
	*y = ny;
	*w = nw;
	*h = nh;

	return TRUE;
}

static gboolean rt_queue_to_tiles(RendererTiles *rt, gint x, gint y, gint w, gint h,
				  gboolean clamp, ImageRenderType render,
			      	  gboolean new_data, gboolean only_existing)
{
	PixbufRenderer *pr = rt->pr;
	gint i, j;
	gint x1, x2;
	gint y1, y2;

	if (clamp && !rt_clamp_to_visible(rt, &x, &y, &w, &h)) return FALSE;

	x1 = ROUND_DOWN(x, rt->tile_width);
	x2 = ROUND_UP(x + w, rt->tile_width);

	y1 = ROUND_DOWN(y, rt->tile_height);
	y2 = ROUND_UP(y + h, rt->tile_height);

	for (j = y1; j <= y2; j += rt->tile_height)
		{
		for (i = x1; i <= x2; i += rt->tile_width)
			{
			ImageTile *it;

			it = rt_tile_get(rt, i, j,
					 (only_existing &&
					  (i + rt->tile_width < rt->x_scroll ||
					   i > rt->x_scroll + pr->vis_width ||
					   j + rt->tile_height < rt->y_scroll ||
					   j > rt->y_scroll + pr->vis_height)));
			if (it)
				{
				QueueData *qd;

				if ((render == TILE_RENDER_ALL && it->render_done != TILE_RENDER_ALL) ||
				    (render == TILE_RENDER_AREA && it->render_todo != TILE_RENDER_ALL))
					{
					it->render_todo = render;
					}

				qd = g_new(QueueData, 1);
				qd->it = it;
				qd->new_data = new_data;

				if (i < x)
					{
					qd->x = x - i;
					}
				else
					{
					qd->x = 0;
					}
				qd->w = x + w - i - qd->x;
				if (qd->x + qd->w > rt->tile_width) qd->w = rt->tile_width - qd->x;

				if (j < y)
					{
					qd->y = y - j;
					}
				else
					{
					qd->y = 0;
					}
				qd->h = y + h - j - qd->y;
				if (qd->y + qd->h > rt->tile_height) qd->h = rt->tile_height - qd->y;

				if (qd->w < 1 || qd->h < 1)
					{
					g_free(qd);
					}
				else if (it->qd)
					{
					rt_queue_merge(it->qd, qd);
					g_free(qd);
					}
				else
					{
					it->qd = qd;
					rt->draw_queue = g_list_append(rt->draw_queue, qd);
					}
				}
			}
		}

	return TRUE;
}

static void rt_queue(RendererTiles *rt, gint x, gint y, gint w, gint h,
		     gboolean clamp, ImageRenderType render,
		     gboolean new_data, gboolean only_existing)
{
	PixbufRenderer *pr = rt->pr;
	gint nx, ny;

	rt_sync_scroll(rt);

	nx = CLAMP(x, 0, pr->width - 1);
	ny = CLAMP(y, 0, pr->height - 1);
	w -= (nx - x);
	h -= (ny - y);
	w = CLAMP(w, 0, pr->width - nx);
	h = CLAMP(h, 0, pr->height - ny);
	if (w < 1 || h < 1) return;

	if (rt_queue_to_tiles(rt, nx, ny, w, h, clamp, render, new_data, only_existing) &&
	    ((!rt->draw_queue && !rt->draw_queue_2pass) || !rt->draw_idle_id))
		{
		if (rt->draw_idle_id)
			{
			g_source_remove(rt->draw_idle_id);
			rt->draw_idle_id = 0;
			}
		rt_queue_schedule_next_draw(rt, TRUE);
		}
}

static void rt_scroll(void *renderer, gint x_off, gint y_off)
{
	RendererTiles *rt = (RendererTiles *) renderer;
	PixbufRenderer *pr = rt->pr;

	rt_sync_scroll(rt);
	if (rt->stereo_mode & PR_STEREO_MIRROR) x_off = -x_off;
	if (rt->stereo_mode & PR_STEREO_FLIP) y_off = -y_off;

	gint w = pr->vis_width - abs(x_off);
	gint h = pr->vis_height - abs(y_off);

	if (w < 1 || h < 1)
		{
		/* scrolled completely to new material */
		rt_queue(rt, 0, 0, pr->width, pr->height, TRUE, TILE_RENDER_ALL, FALSE, FALSE);
		return;
		}
	else
		{
		gint x1, y1;
		gint x2, y2;
		GtkWidget *box;
		GdkWindow *window;
		cairo_t *cr;
		cairo_surface_t *surface;

		if (x_off < 0)
			{
			x1 = abs(x_off);
			x2 = 0;
			}
		else
			{
			x1 = 0;
			x2 = abs(x_off);
			}

		if (y_off < 0)
			{
			y1 = abs(y_off);
			y2 = 0;
			}
		else
			{
			y1 = 0;
			y2 = abs(y_off);
			}

		box = GTK_WIDGET(pr);
		window = gtk_widget_get_window(box);

		cr = gdk_cairo_create(window);
		surface = cairo_get_target(cr);
		/* clipping restricts the intermediate surface's size, so it's a good idea
		 * to use it. */
		cairo_rectangle(cr, x1 + pr->x_offset + rt->stereo_off_x, y1 + pr->y_offset + rt->stereo_off_y, w, h);
		cairo_clip (cr);
		/* Now push a group to change the target */
		cairo_push_group (cr);
		cairo_set_source_surface(cr, surface, x1 - x2, y1 - y2);
		cairo_paint(cr);
		/* Now copy the intermediate target back */
		cairo_pop_group_to_source(cr);
		cairo_paint(cr);
		cairo_destroy(cr);

		rt_overlay_queue_all(rt, x2, y2, x1, y1);

		w = pr->vis_width - w;
		h = pr->vis_height - h;

		if (w > 0)
			{
			rt_queue(rt,
				    x_off > 0 ? rt->x_scroll + (pr->vis_width - w) : rt->x_scroll, rt->y_scroll,
				    w, pr->vis_height, TRUE, TILE_RENDER_ALL, FALSE, FALSE);
			}
		if (h > 0)
			{
			/* FIXME, to optimize this, remove overlap */
			rt_queue(rt,
				    rt->x_scroll, y_off > 0 ? rt->y_scroll + (pr->vis_height - h) : rt->y_scroll,
				    pr->vis_width, h, TRUE, TILE_RENDER_ALL, FALSE, FALSE);
			}
		}
}

static void renderer_area_changed(void *renderer, gint src_x, gint src_y, gint src_w, gint src_h)
{
	RendererTiles *rt = (RendererTiles *)renderer;
	PixbufRenderer *pr = rt->pr;
	gint x, y, width, height,  x1, y1, x2, y2;

	gint orientation = rt_get_orientation(rt);
	pr_coords_map_orientation_reverse(orientation,
				     src_x - GET_RIGHT_PIXBUF_OFFSET(rt), src_y,
				     pr->image_width, pr->image_height,
				     src_w, src_h,
				     &x, &y,
				     &width, &height);

	if (pr->scale != 1.0 && pr->zoom_quality != GDK_INTERP_NEAREST)
		{
		/* increase region when using a zoom quality that may access surrounding pixels */
		y -= 1;
		height += 2;
		}

	x1 = (gint)floor((gdouble)x * pr->scale);
	y1 = (gint)floor((gdouble)y * pr->scale * pr->aspect_ratio);
	x2 = (gint)ceil((gdouble)(x + width) * pr->scale);
	y2 = (gint)ceil((gdouble)(y + height) * pr->scale * pr->aspect_ratio);

	rt_queue(rt, x1, y1, x2 - x1, y2 - y1, FALSE, TILE_RENDER_AREA, TRUE, TRUE);
}

static void renderer_redraw(RendererTiles *rt, gint x, gint y, gint w, gint h,
                     gint clamp, ImageRenderType render, gboolean new_data, gboolean only_existing)
{
	PixbufRenderer *pr = rt->pr;

	x -= rt->stereo_off_x;
	y -= rt->stereo_off_y;

	rt_border_draw(rt, x, y, w, h);

	x = MAX(0, x - pr->x_offset + pr->x_scroll);
	y = MAX(0, y - pr->y_offset + pr->y_scroll);

	rt_queue(rt,
		 x, y,
		 MIN(w, pr->width - x),
		 MIN(h, pr->height - y),
		 clamp, render, new_data, only_existing);
}

static void renderer_update_pixbuf(void *renderer, gboolean lazy)
{
	rt_queue_clear((RendererTiles *)renderer);
}

static void renderer_update_zoom(void *renderer, gboolean lazy)
{
	RendererTiles *rt = (RendererTiles *)renderer;
	PixbufRenderer *pr = rt->pr;

	rt_tile_invalidate_all((RendererTiles *)renderer);
	if (!lazy)
		{
		renderer_redraw(renderer, 0, 0, pr->width, pr->height, TRUE, TILE_RENDER_ALL, TRUE, FALSE);
		}
	rt_border_clear(rt);
}

static void renderer_invalidate_region(void *renderer, gint x, gint y, gint w, gint h)
{
	rt_tile_invalidate_region((RendererTiles *)renderer, x, y, w, h);
}

static void renderer_update_viewport(void *renderer)
{
	RendererTiles *rt = (RendererTiles *)renderer;

	rt->stereo_off_x = 0;
	rt->stereo_off_y = 0;

	if (rt->stereo_mode & PR_STEREO_RIGHT)
		{
		if (rt->stereo_mode & PR_STEREO_HORIZ)
			{
			rt->stereo_off_x = rt->pr->viewport_width;
			}
		else if (rt->stereo_mode & PR_STEREO_VERT)
			{
			rt->stereo_off_y = rt->pr->viewport_height;
			}
		else if (rt->stereo_mode & PR_STEREO_FIXED)
			{
			rt->stereo_off_x = rt->pr->stereo_fixed_x_right;
			rt->stereo_off_y = rt->pr->stereo_fixed_y_right;
			}
		}
	else
		{
		if (rt->stereo_mode & PR_STEREO_FIXED)
			{
			rt->stereo_off_x = rt->pr->stereo_fixed_x_left;
			rt->stereo_off_y = rt->pr->stereo_fixed_y_left;
			}
		}
        DEBUG_1("update size: %p  %d %d   %d %d", rt, rt->stereo_off_x, rt->stereo_off_y, rt->pr->viewport_width, rt->pr->viewport_height);
	rt_sync_scroll(rt);
	rt_overlay_update_sizes(rt);
	rt_border_clear(rt);
}

static void renderer_stereo_set(void *renderer, gint stereo_mode)
{
	RendererTiles *rt = (RendererTiles *)renderer;

	rt->stereo_mode = stereo_mode;
}

static void renderer_free(void *renderer)
{
	RendererTiles *rt = (RendererTiles *)renderer;
	rt_queue_clear(rt);
	rt_tile_free_all(rt);
	if (rt->spare_tile) g_object_unref(rt->spare_tile);
	if (rt->overlay_buffer) g_object_unref(rt->overlay_buffer);
	rt_overlay_list_clear(rt);
	/* disconnect "hierarchy-changed" */
	g_signal_handlers_disconnect_matched(G_OBJECT(rt->pr), G_SIGNAL_MATCH_DATA,
                                                     0, 0, 0, NULL, rt);
        g_free(rt);
}

#if GTK_CHECK_VERSION(3,0,0)

static gboolean rt_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	RendererTiles *rt = (RendererTiles *)data;
	if (gtk_widget_is_drawable(widget))
		{
		if (gtk_widget_get_has_window(widget))
			{
			GdkRectangle area;
			if (gdk_cairo_get_clip_rectangle(cr, &area))
				{
				renderer_redraw(rt, area.x, area.y, area.width, area.height,
						FALSE, TILE_RENDER_ALL, FALSE, FALSE);
				}
			}
		}

	return FALSE;
}

#else
static gboolean rt_expose_cb(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
	RendererTiles *rt = (RendererTiles *)data;
	if (gtk_widget_is_drawable(widget))
		{
		if (gtk_widget_get_has_window(widget))
			{
			if (event->window != gtk_widget_get_window(widget))
				{
				GdkRectangle area;

				gdk_window_get_position(event->window, &area.x, &area.y);
				area.x += event->area.x;
				area.y += event->area.y;
				area.width = event->area.width;
				area.height = event->area.height;
				renderer_redraw(rt, area.x, area.y, area.width, area.height,
						FALSE, TILE_RENDER_ALL, FALSE, FALSE);

				}
			else
				{
				renderer_redraw(rt, event->area.x, event->area.y, event->area.width, event->area.height,
						FALSE, TILE_RENDER_ALL, FALSE, FALSE);
				}
			}
		}

	return FALSE;
}
#endif


RendererFuncs *renderer_tiles_new(PixbufRenderer *pr)
{
	RendererTiles *rt = g_new0(RendererTiles, 1);

	rt->pr = pr;

	rt->f.area_changed = renderer_area_changed;
	rt->f.update_pixbuf = renderer_update_pixbuf;
	rt->f.free = renderer_free;
	rt->f.update_zoom = renderer_update_zoom;
	rt->f.invalidate_region = renderer_invalidate_region;
	rt->f.scroll = rt_scroll;
	rt->f.update_viewport = renderer_update_viewport;


	rt->f.overlay_add = renderer_tiles_overlay_add;
	rt->f.overlay_set = renderer_tiles_overlay_set;
	rt->f.overlay_get = renderer_tiles_overlay_get;

	rt->f.stereo_set = renderer_stereo_set;

	rt->tile_width = PR_TILE_SIZE;
	rt->tile_height = PR_TILE_SIZE;

	rt->tiles = NULL;
	rt->tile_cache_size = 0;

	rt->tile_cache_max = PR_CACHE_SIZE_DEFAULT;

	rt->draw_idle_id = 0;

	rt->stereo_mode = 0;
	rt->stereo_off_x = 0;
	rt->stereo_off_y = 0;

	g_signal_connect(G_OBJECT(pr), "hierarchy-changed",
			 G_CALLBACK(rt_hierarchy_changed_cb), rt);

#if GTK_CHECK_VERSION(3,0,0)
	g_signal_connect(G_OBJECT(pr), "draw",
	                 G_CALLBACK(rt_draw_cb), rt);
#else
	g_signal_connect(G_OBJECT(pr), "expose_event",
	                 G_CALLBACK(rt_expose_cb), rt);
#endif

	rt->hidpi_scale = gtk_widget_get_scale_factor(GTK_WIDGET(pr));

	return (RendererFuncs *) rt;
}


/* vim: set shiftwidth=8 softtabstop=0 cindent cinoptions={1s: */
