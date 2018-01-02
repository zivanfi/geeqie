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

#include "main.h"
#include "image-overlay.h"

#include "collect.h"
#include "exif.h"
#include "filedata.h"
#include "histogram.h"
#include "image.h"
#include "img-view.h"
#include "layout.h"
#include "metadata.h"
#include "pixbuf-renderer.h"
#include "pixbuf_util.h"
#include "ui_fileops.h"
#include "image-load.h"
#include "glua.h"

/*
 *----------------------------------------------------------------------------
 * image overlay
 *----------------------------------------------------------------------------
 */


typedef struct _OverlayStateData OverlayStateData;
struct _OverlayStateData {
	ImageWindow *imd;
	ImageState changed_states;
	NotifyType notify;

	Histogram *histogram;

	OsdShowFlags show;

	gint ovl_info;

	gint x;
	gint y;

	gint icon_time[IMAGE_OSD_COUNT];
	gint icon_id[IMAGE_OSD_COUNT];

	guint idle_id; /* event source id */
	guint timer_id; /* event source id */
	gulong destroy_id;
};


typedef struct _OSDIcon OSDIcon;
struct _OSDIcon {
	gboolean reset;	/* reset on new image */
	gint x;		/* x, y offset */
	gint y;
	gchar *key;	/* inline pixbuf */
};

static OSDIcon osd_icons[] = {
	{  TRUE,   0,   0, NULL },			/* none */
	{  TRUE, -10, -10, NULL },			/* auto rotated */
	{  TRUE, -10, -10, NULL },			/* user rotated */
	{  TRUE, -40, -10, NULL },			/* color embedded */
	{  TRUE, -70, -10, NULL },			/* first image */
	{  TRUE, -70, -10, NULL },			/* last image */
	{ FALSE, -70, -10, NULL },			/* osd enabled */
	{ FALSE, 0, 0, NULL }
};

#define OSD_DATA "overlay-data"

#define IMAGE_OSD_DEFAULT_DURATION 30

#define HISTOGRAM_HEIGHT 50
#define HISTOGRAM_WIDTH  256

static void image_osd_timer_schedule(OverlayStateData *osd);

void set_image_overlay_template_string(gchar **template_string, const gchar *value)
{
	g_assert(template_string);

	g_free(*template_string);
	*template_string = g_strdup(value);
}


void set_default_image_overlay_template_string(gchar **template_string)
{
	set_image_overlay_template_string(template_string, DEFAULT_OVERLAY_INFO);
}

void set_image_overlay_font_string(gchar **font_string, const gchar *value)
{
	g_assert(font_string);

	g_free(*font_string);
	*font_string = g_strdup(value);
}

static OverlayStateData *image_get_osd_data(ImageWindow *imd)
{
	OverlayStateData *osd;

	if (!imd) return NULL;

	g_assert(imd->pr);

	osd = g_object_get_data(G_OBJECT(imd->pr), "IMAGE_OVERLAY_DATA");
	return osd;
}

static void image_set_osd_data(ImageWindow *imd, OverlayStateData *osd)
{
	g_assert(imd);
	g_assert(imd->pr);
	g_object_set_data(G_OBJECT(imd->pr), "IMAGE_OVERLAY_DATA", osd);
}

/*
 *----------------------------------------------------------------------------
 * image histogram
 *----------------------------------------------------------------------------
 */


void image_osd_histogram_toggle_channel(ImageWindow *imd)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	if (!osd || !osd->histogram) return;

	histogram_toggle_channel(osd->histogram);
	image_osd_update(imd);
}

void image_osd_histogram_toggle_mode(ImageWindow *imd)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	if (!osd || !osd->histogram) return;

	histogram_toggle_mode(osd->histogram);
	image_osd_update(imd);
}

void image_osd_histogram_set_channel(ImageWindow *imd, gint chan)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	if (!osd || !osd->histogram) return;

	histogram_set_channel(osd->histogram, chan);
	image_osd_update(imd);
}

void image_osd_histogram_set_mode(ImageWindow *imd, gint mode)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	if (!osd || !osd->histogram) return;

	histogram_set_mode(osd->histogram, mode);
	image_osd_update(imd);
}

gint image_osd_histogram_get_channel(ImageWindow *imd)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	if (!osd || !osd->histogram) return HCHAN_DEFAULT;

	return histogram_get_channel(osd->histogram);
}

gint image_osd_histogram_get_mode(ImageWindow *imd)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	if (!osd || !osd->histogram) return 0;

	return histogram_get_mode(osd->histogram);
}

void image_osd_toggle(ImageWindow *imd)
{
	OsdShowFlags show;

	if (!imd) return;

	show = image_osd_get(imd);
	if (show == OSD_SHOW_NOTHING)
		{
		image_osd_set(imd, OSD_SHOW_INFO | OSD_SHOW_STATUS);
		return;
		}
	else
		{
		if (show & OSD_SHOW_HISTOGRAM)
			{
			image_osd_set(imd, OSD_SHOW_NOTHING);
			}
		else
			{
			image_osd_set(imd, show | OSD_SHOW_HISTOGRAM);
			}
		}
}

static gchar *keywords_to_string(FileData *fd)
{
	GList *keywords;
	GString *kwstr = NULL;
	gchar *ret = NULL;

	g_assert(fd);

	keywords = metadata_read_list(fd, KEYWORD_KEY, METADATA_PLAIN);

	if (keywords)
		{
		GList *work = keywords;

		while (work)
			{
			gchar *kw = work->data;
			work = work->next;

			if (!kw) continue;
			if (!kwstr)
				kwstr = g_string_new("");
			else
				g_string_append(kwstr, ", ");

			g_string_append(kwstr, kw);
			}
		string_list_free(keywords);
		}

	if (kwstr)
		{
		ret = kwstr->str;
		g_string_free(kwstr, FALSE);
		}

	return ret;
}

static gchar *image_osd_mkinfo(const gchar *str, ImageWindow *imd, GHashTable *vars)
{
	gchar delim = '%', imp = '|', sep[] = ", ";
	gchar *start, *end;
	guint pos, prev;
	gboolean want_separator = FALSE;
	gchar *name, *data;
	GString *new;
	gchar *ret;

	if (!str || !*str) return g_strdup("");

	new = g_string_new(str);

	prev = -1;

	while (TRUE)
		{
		guint limit = 0;
		gchar *trunc = NULL;
		gchar *limpos = NULL;
		gchar *extra = NULL;
		gchar *extrapos = NULL;
		gchar *p;

		start = strchr(new->str + (prev + 1), delim);
		if (!start)
			break;
		end = strchr(start+1, delim);
		if (!end)
			break;

		/* Search for optionnal modifiers
		 * %name:99:extra% -> name = "name", limit=99, extra = "extra"
		 */
		for (p = start + 1; p < end; p++)
			{
			if (p[0] == ':')
				{
				if (g_ascii_isdigit(p[1]) && !limpos)
					{
					limpos = p + 1;
					if (!trunc) trunc = p;
					}
				else
					{
					extrapos = p + 1;
					if (!trunc) trunc = p;
					break;
					}
				}
			}

		if (limpos)
			limit = (guint) atoi(limpos);

		if (extrapos)
			extra = g_strndup(extrapos, end - extrapos);

		name = g_strndup(start+1, (trunc ? trunc : end)-start-1);
		pos = start - new->str;
		data = NULL;

		if (strcmp(name, "keywords") == 0)
			{
			data = keywords_to_string(imd->image_fd);
			}
		else if (strcmp(name, "comment") == 0)
			{
			data = metadata_read_string(imd->image_fd, COMMENT_KEY, METADATA_PLAIN);
			}
		else if (strcmp(name, "imagecomment") == 0)
			{
			data = exif_get_image_comment(imd->image_fd);
			}
#ifdef HAVE_LUA
		else if (strncmp(name, "lua/", 4) == 0)
			{
			gchar *tmp;
			tmp = strchr(name+4, '/');
			if (!tmp)
				break;
			*tmp = '\0';
			data = lua_callvalue(imd->image_fd, name+4, tmp+1);
			}
#endif
		else
			{
			data = g_strdup(g_hash_table_lookup(vars, name));
			if (!data)
				data = metadata_read_string(imd->image_fd, name, METADATA_FORMATTED);
			}

		if (data && *data && limit > 0 && strlen(data) > limit + 3)
			{
			gchar *new_data = g_strdup_printf("%-*.*s...", limit, limit, data);
			g_free(data);
			data = new_data;
			}

		if (data)
			{
			/* Since we use pango markup to display, we need to escape here */
			gchar *escaped = g_markup_escape_text(data, -1);
			g_free(data);
			data = escaped;
			}

		if (extra)
			{
			if (data && *data)
				{
				/* Display data between left and right parts of extra string
				 * the data is expressed by a '*' character. A '*' may be escaped
				 * by a \. You should escape all '*' characters, do not rely on the
				 * current implementation which only replaces the first unescaped '*'.
				 * If no "*" is present, the extra string is just appended to data string.
				 * Pango mark up is accepted in left and right parts.
				 * Any \n is replaced by a newline
				 * Examples:
				 * "<i>*</i>\n" -> data is displayed in italics ended with a newline
				 * "\n" 	-> ended with newline
				 * "ISO *"	-> prefix data with "ISO " (ie. "ISO 100")
				 * "\**\*"	-> prefix data with a star, and append a star (ie. "*100*")
				 * "\\*"	-> prefix data with an anti slash (ie "\100")
				 * "Collection <b>*</b>\n" -> display data in bold prefixed by "Collection " and a newline is appended
				 *
				 * FIXME: using background / foreground colors lead to weird results.
				 */
				gchar *new_data;
				gchar *left = NULL;
				gchar *right = extra;
				gchar *p;
				guint len = strlen(extra);

				/* Search for left and right parts and unescape characters */
				for (p = extra; *p; p++, len--)
					if (p[0] == '\\')
						{
						if (p[1] == 'n')
							{
							memmove(p+1, p+2, --len);
							p[0] = '\n';
							}
						else if (p[1] != '\0')
							memmove(p, p+1, len--); // includes \0
						}
					else if (p[0] == '*' && !left)
						{
						right = p + 1;
						left = extra;
						}

				if (left) right[-1] = '\0';

				new_data = g_strdup_printf("%s%s%s", left ? left : "", data, right);
				g_free(data);
				data = new_data;
				}
			g_free(extra);
			}

		g_string_erase(new, pos, end-start+1);
		if (data && *data)
			{
			if (want_separator)
				{
				/* insert separator */
				g_string_insert(new, pos, sep);
				pos += strlen(sep);
				want_separator = FALSE;
				}

			g_string_insert(new, pos, data);
			pos += strlen(data);
		}

		if (pos-prev >= 1 && new->str[pos] == imp)
			{
			/* pipe character is replaced by a separator, delete it
			 * and raise a flag if needed */
			g_string_erase(new, pos--, 1);
			want_separator |= (data && *data);
			}

		if (new->str[pos] == '\n') want_separator = FALSE;

		prev = pos - 1;

		g_free(name);
		g_free(data);
		}

	/* search and destroy empty lines */
	end = new->str;
	while ((start = strchr(end, '\n')))
		{
		end = start;
		while (*++(end) == '\n')
			;
		g_string_erase(new, start-new->str, end-start-1);
		}

	g_strchomp(new->str);

	ret = new->str;
	g_string_free(new, FALSE);

	return ret;
}

typedef enum {
	OSDT_NONE 	= 0,
	OSDT_FREE 	= 1 << 0,
	OSDT_NO_DUP 	= 1 << 1
} OsdTemplateFlags;

static void osd_template_insert(GHashTable *vars, gchar *keyword, gchar *value, OsdTemplateFlags flags)
{
	if (!value)
		{
		g_hash_table_insert(vars, keyword, g_strdup(""));
		return;
		}

	if (flags & OSDT_NO_DUP)
		{
		g_hash_table_insert(vars, keyword, value);
		return;
		}
	else
		{
		g_hash_table_insert(vars, keyword, g_strdup(value));
		}

	if (flags & OSDT_FREE) g_free((gpointer) value);
}

static GdkPixbuf *image_osd_info_render(OverlayStateData *osd)
{
	GdkPixbuf *pixbuf = NULL;
	gint width, height;
	PangoLayout *layout;
	const gchar *name;
	gchar *text;
	gboolean with_hist;
	const HistMap *histmap = NULL;
	ImageWindow *imd = osd->imd;
	FileData *fd = image_get_fd(imd);
	PangoFontDescription *font_desc;

	if (!fd) return NULL;

	name = image_get_name(imd);
	if (name)
		{
		gint n, t;
		CollectionData *cd;
		CollectInfo *info;
		GHashTable *vars;

		vars = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

		cd = image_get_collection(imd, &info);
		if (cd)
			{
			t = g_list_length(cd->list);
			n = g_list_index(cd->list, info) + 1;
			if (cd->name)
				{
				if (file_extension_match(cd->name, GQ_COLLECTION_EXT))
					osd_template_insert(vars, "collection", remove_extension_from_path(cd->name), OSDT_FREE);
				else
					osd_template_insert(vars, "collection", cd->name, OSDT_NONE);
				}
			else
				{
				osd_template_insert(vars, "collection", _("Untitled"), OSDT_NONE);
				}
			}
		else
			{
			LayoutWindow *lw = layout_find_by_image(imd);
			if (lw)
				{
				if (lw->slideshow)
					{
					n = g_list_length(lw->slideshow->list_done);
					t = n + g_list_length(lw->slideshow->list);
					if (n == 0) n = t;
					}
				else
					{
					t = layout_list_count(lw, NULL);
					n = layout_list_get_index(lw, image_get_fd(lw->image)) + 1;
					}
				}
			else if (view_window_find_image(imd, &n, &t))
				{
				n++;
				}
			else
				{
				t = 1;
				n = 1;
				}

			if (n < 1) n = 1;
			if (t < 1) t = 1;

			osd_template_insert(vars, "collection", NULL, OSDT_NONE);
			}

		osd_template_insert(vars, "number", g_strdup_printf("%d", n), OSDT_NO_DUP);
		osd_template_insert(vars, "total", g_strdup_printf("%d", t), OSDT_NO_DUP);
		osd_template_insert(vars, "name", (gchar *) name, OSDT_NONE);
		osd_template_insert(vars, "date", imd->image_fd ? ((gchar *) text_from_time(imd->image_fd->date)) : "", OSDT_NONE);
		osd_template_insert(vars, "size", imd->image_fd ? (text_from_size_abrev(imd->image_fd->size)) : g_strdup(""), OSDT_FREE);
		osd_template_insert(vars, "zoom", image_zoom_get_as_text(imd), OSDT_FREE);

		if (!imd->unknown)
			{
			gint w, h;
			GdkPixbuf *load_pixbuf = image_loader_get_pixbuf(imd->il);

			if (imd->delay_flip &&
			    imd->il && load_pixbuf &&
			    image_get_pixbuf(imd) != load_pixbuf)
				{
				w = gdk_pixbuf_get_width(load_pixbuf);
				h = gdk_pixbuf_get_height(load_pixbuf);
				}
			else
				{
				image_get_image_size(imd, &w, &h);
				}


			osd_template_insert(vars, "width", g_strdup_printf("%d", w), OSDT_NO_DUP);
	 		osd_template_insert(vars, "height", g_strdup_printf("%d", h), OSDT_NO_DUP);
	 		osd_template_insert(vars, "res", g_strdup_printf("%d × %d", w, h), OSDT_FREE);
	 		}
		else
			{
			osd_template_insert(vars, "width", NULL, OSDT_NONE);
	 		osd_template_insert(vars, "height", NULL, OSDT_NONE);
	 		osd_template_insert(vars, "res", NULL, OSDT_NONE);
			}

	 	text = image_osd_mkinfo(options->image_overlay.template_string, imd, vars);
		g_hash_table_destroy(vars);

	} else {
		/* When does this occur ?? */
		text = g_markup_escape_text(_("Untitled"), -1);
	}

	with_hist = ((osd->show & OSD_SHOW_HISTOGRAM) && osd->histogram);
	if (with_hist)
		{
		histmap = histmap_get(imd->image_fd);
		if (!histmap)
			{
			histmap_start_idle(imd->image_fd);
			with_hist = FALSE;
			}
		}


	{
		gint active_marks = 0;
		gint mark;
		gchar *text2;

		for (mark = 0; mark < FILEDATA_MARKS_SIZE; mark++)
			{
			active_marks += file_data_get_mark(fd, mark);
			}

		if (active_marks > 0)
			{
			GString *buf = g_string_sized_new(FILEDATA_MARKS_SIZE * 2);

			for (mark = 0; mark < FILEDATA_MARKS_SIZE; mark++)
				{
				g_string_append_printf(buf, file_data_get_mark(fd, mark) ? " <span background='#FF00FF'>%c</span>" : " %c", '1' + (mark < 9 ? mark : -1) );
				}

			if (*text)
				text2 = g_strdup_printf("%s\n%s", text, buf->str);
			else
				text2 = g_strdup(buf->str);
			g_string_free(buf, TRUE);
			g_free(text);
			text = text2;
			}
/*
		if (with_hist)
			{
			gchar *escaped_histogram_label = g_markup_escape_text(histogram_label(osd->histogram), -1);
			if (*text)
				text2 = g_strdup_printf("%s\n%s", text, escaped_histogram_label);
			else
				text2 = g_strdup(escaped_histogram_label);
			g_free(escaped_histogram_label);
			g_free(text);
			text = text2;
			}
*/
	}

	font_desc = pango_font_description_from_string(options->image_overlay.font);
	layout = gtk_widget_create_pango_layout(imd->pr, NULL);
	pango_layout_set_font_description(layout, font_desc);

	pango_layout_set_markup(layout, text, -1);
	g_free(text);

	pango_layout_get_pixel_size(layout, &width, &height);
	/* with empty text width is set to 0, but not height) */
	if (width == 0)
		height = 0;
	else if (height == 0)
		width = 0;
	if (width > 0) width += 10;
	if (height > 0) height += 10;

	if (with_hist)
		{
		if (width < HISTOGRAM_WIDTH + 10) width = HISTOGRAM_WIDTH + 10;
		height += HISTOGRAM_HEIGHT + 5;
		}

	if (width > 0 && height > 0)
		{
		pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
		pixbuf_set_rect_fill(pixbuf, 3, 3, width-6, height-6, options->image_overlay.background_red, options->image_overlay.background_green,
															options->image_overlay.background_blue, options->image_overlay.background_alpha);
		pixbuf_set_rect(pixbuf, 0, 0, width, height, 240, 240, 240, 80, 1, 1, 1, 1);
		pixbuf_set_rect(pixbuf, 1, 1, width-2, height-2, 240, 240, 240, 130, 1, 1, 1, 1);
		pixbuf_set_rect(pixbuf, 2, 2, width-4, height-4, 240, 240, 240, 180, 1, 1, 1, 1);
		pixbuf_pixel_set(pixbuf, 0, 0, 0, 0, 0, 0);
		pixbuf_pixel_set(pixbuf, width - 1, 0, 0, 0, 0, 0);
		pixbuf_pixel_set(pixbuf, 0, height - 1, 0, 0, 0, 0);
		pixbuf_pixel_set(pixbuf, width - 1, height - 1, 0, 0, 0, 0);

		if (with_hist)
			{
			gint x = 5;
			gint y = height - HISTOGRAM_HEIGHT - 5;
			gint w = width - 10;

			pixbuf_set_rect_fill(pixbuf, x, y, w, HISTOGRAM_HEIGHT, 220, 220, 220, 210);
			histogram_draw(osd->histogram, histmap, pixbuf, x, y, w, HISTOGRAM_HEIGHT);
			}
		pixbuf_draw_layout(pixbuf, layout, imd->pr, 5, 5, options->image_overlay.text_red, options->image_overlay.text_green,
															options->image_overlay.text_blue, options->image_overlay.text_alpha);
	}

	g_object_unref(G_OBJECT(layout));

	return pixbuf;
}

static GdkPixbuf *image_osd_icon_pixbuf(ImageOSDFlag flag)
{
	static GdkPixbuf **icons = NULL;
	GdkPixbuf *icon = NULL;

	if (!icons) icons = g_new0(GdkPixbuf *, IMAGE_OSD_COUNT);
	if (icons[flag]) return icons[flag];

	if (osd_icons[flag].key)
		{
		icon = pixbuf_inline(osd_icons[flag].key);
		}

	if (!icon)
		{
		icon = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 24, 24);
		pixbuf_set_rect_fill(icon, 1, 1, 22, 22, 255, 255, 255, 200);
		pixbuf_set_rect(icon, 0, 0, 24, 24, 0, 0, 0, 128, 1, 1, 1, 1);
		switch (flag)
			{
			case IMAGE_OSD_ROTATE_AUTO:
				pixbuf_set_rect(icon, 3, 8, 11, 12,
						0, 0, 0, 255,
						3, 0, 3, 0);
				pixbuf_draw_triangle(icon, 14, 3, 6, 12,
						     20, 9, 14, 15, 14, 3,
						     0, 0, 0, 255);
				break;
			case IMAGE_OSD_ROTATE_USER:
				break;
			case IMAGE_OSD_COLOR:
				pixbuf_set_rect_fill(icon, 3, 3, 18, 6, 200, 0, 0, 255);
				pixbuf_set_rect_fill(icon, 3, 9, 18, 6, 0, 200, 0, 255);
				pixbuf_set_rect_fill(icon, 3, 15, 18, 6, 0, 0, 200, 255);
				break;
			case IMAGE_OSD_FIRST:
				pixbuf_set_rect(icon, 3, 3, 18, 18, 0, 0, 0, 200, 3, 3, 3, 0);
				pixbuf_draw_triangle(icon, 6, 5, 12, 6,
						     12, 5, 18, 11, 6, 11,
						     0, 0, 0, 255);
				break;
			case IMAGE_OSD_LAST:
				pixbuf_set_rect(icon, 3, 3, 18, 18, 0, 0, 0, 200, 3, 3, 0, 3);
				pixbuf_draw_triangle(icon, 6, 12, 12, 6,
						     12, 18, 6, 12, 18, 12,
						     0, 0, 0, 255);
				break;
			case IMAGE_OSD_ICON:
				pixbuf_set_rect_fill(icon, 11, 3, 3, 12, 0, 0, 0, 255);
				pixbuf_set_rect_fill(icon, 11, 17, 3, 3, 0, 0, 0, 255);
				break;
			default:
				break;
			}
		}

	icons[flag] = icon;

	return icon;
}

static gint image_overlay_add(ImageWindow *imd, GdkPixbuf *pixbuf, gint x, gint y,
			      OverlayRendererFlags flags)
{
	return pixbuf_renderer_overlay_add((PixbufRenderer *)imd->pr, pixbuf, x, y, flags);
}

static void image_overlay_set(ImageWindow *imd, gint id, GdkPixbuf *pixbuf, gint x, gint y)
{
	pixbuf_renderer_overlay_set((PixbufRenderer *)imd->pr, id, pixbuf, x, y);
}

static void image_overlay_remove(ImageWindow *imd, gint id)
{
	pixbuf_renderer_overlay_remove((PixbufRenderer *)imd->pr, id);
}

static void image_osd_icon_show(OverlayStateData *osd, ImageOSDFlag flag)
{
	GdkPixbuf *pixbuf;

	if (osd->icon_id[flag]) return;

	pixbuf = image_osd_icon_pixbuf(flag);
	if (!pixbuf) return;

	osd->icon_id[flag] = image_overlay_add(osd->imd, pixbuf,
					       osd_icons[flag].x, osd_icons[flag].y,
					       OVL_RELATIVE);
}

static void image_osd_icon_hide(OverlayStateData *osd, ImageOSDFlag flag)
{
	if (osd->icon_id[flag])
		{
		image_overlay_remove(osd->imd, osd->icon_id[flag]);
		osd->icon_id[flag] = 0;
		}
}

static void image_osd_icons_reset_time(OverlayStateData *osd)
{
	gint i;

	for (i = 0; i < IMAGE_OSD_COUNT; i++)
		{
		if (osd_icons[i].reset)
			{
			osd->icon_time[i] = 0;
			}
		}
}

static void image_osd_icons_update(OverlayStateData *osd)
{
	gint i;

	for (i = 0; i < IMAGE_OSD_COUNT; i++)
		{
		if (osd->icon_time[i] > 0)
			{
			image_osd_icon_show(osd, i);
			}
		else
			{
			image_osd_icon_hide(osd, i);
			}
		}
}

static void image_osd_icons_hide(OverlayStateData *osd)
{
	gint i;

	for (i = 0; i < IMAGE_OSD_COUNT; i++)
		{
		image_osd_icon_hide(osd, i);
		}
}

static void image_osd_info_show(OverlayStateData *osd, GdkPixbuf *pixbuf)
{
	if (osd->ovl_info == 0)
		{
		osd->ovl_info = image_overlay_add(osd->imd, pixbuf, osd->x, osd->y, OVL_RELATIVE);
		}
	else
		{
		image_overlay_set(osd->imd, osd->ovl_info, pixbuf, osd->x, osd->y);
		}
}

static void image_osd_info_hide(OverlayStateData *osd)
{
	if (osd->ovl_info == 0) return;

	image_overlay_remove(osd->imd, osd->ovl_info);
	osd->ovl_info = 0;
}

static gboolean image_osd_update_cb(gpointer data)
{
	OverlayStateData *osd = data;

	if (osd->show & OSD_SHOW_INFO)
		{
		/* redraw when the image was changed,
		   with histogram we have to redraw also when loading is finished */
		if (osd->changed_states & IMAGE_STATE_IMAGE ||
		    (osd->changed_states & IMAGE_STATE_LOADING && osd->show & OSD_SHOW_HISTOGRAM) ||
		    osd->notify & NOTIFY_HISTMAP)
			{
			GdkPixbuf *pixbuf;

			pixbuf = image_osd_info_render(osd);
			if (pixbuf)
				{
				image_osd_info_show(osd, pixbuf);
				g_object_unref(pixbuf);
				}
			else
				{
				image_osd_info_hide(osd);
				}
			}
		}
	else
		{
		image_osd_info_hide(osd);
		}

	if (osd->show & OSD_SHOW_STATUS)
		{
		if (osd->changed_states & IMAGE_STATE_IMAGE)
			image_osd_icons_reset_time(osd);

		if (osd->changed_states & IMAGE_STATE_COLOR_ADJ)
			{
			osd->icon_time[IMAGE_OSD_COLOR] = IMAGE_OSD_DEFAULT_DURATION + 1;
			image_osd_timer_schedule(osd);
			}

		if (osd->changed_states & IMAGE_STATE_ROTATE_AUTO)
			{
			gint n = 0;

			if (osd->imd->state & IMAGE_STATE_ROTATE_AUTO)
				{
				n = 1;
				if (!osd->imd->cm) n += IMAGE_OSD_DEFAULT_DURATION;
				}

			osd->icon_time[IMAGE_OSD_ROTATE_AUTO] = n;
			image_osd_timer_schedule(osd);
			}

		image_osd_icons_update(osd);
		}
	else
		{
		image_osd_icons_hide(osd);
		}

	osd->changed_states = IMAGE_STATE_NONE;
	osd->notify = 0;
	osd->idle_id = 0;
	return FALSE;
}

static void image_osd_update_schedule(OverlayStateData *osd, gboolean force)
{
	if (force) osd->changed_states |= IMAGE_STATE_IMAGE;

	if (!osd->idle_id)
		{
		osd->idle_id = g_idle_add_full(G_PRIORITY_HIGH, image_osd_update_cb, osd, NULL);
		}
}

void image_osd_update(ImageWindow *imd)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	if (!osd) return;

	image_osd_update_schedule(osd, TRUE);
}

static gboolean image_osd_timer_cb(gpointer data)
{
	OverlayStateData *osd = data;
	gboolean done = TRUE;
	gboolean changed = FALSE;
	gint i;

	for (i = 0; i < IMAGE_OSD_COUNT; i++)
		{
		if (osd->icon_time[i] > 1)
			{
			osd->icon_time[i]--;
			if (osd->icon_time[i] < 2)
				{
				osd->icon_time[i] = 0;
				changed = TRUE;
				}
			else
				{
				done = FALSE;
				}
			}
		}

	if (changed) image_osd_update_schedule(osd, FALSE);

	if (done)
		{
		osd->timer_id = 0;
		return FALSE;
		}

	return TRUE;
}

static void image_osd_timer_schedule(OverlayStateData *osd)
{
	if (!osd->timer_id)
		{
		osd->timer_id = g_timeout_add(100, image_osd_timer_cb, osd);
		}
}

static void image_osd_state_cb(ImageWindow *imd, ImageState state, gpointer data)
{
	OverlayStateData *osd = data;

	osd->changed_states |= state;
	image_osd_update_schedule(osd, FALSE);
}

static void image_osd_notify_cb(FileData *fd, NotifyType type, gpointer data)
{
	OverlayStateData *osd = data;

	if ((type & (NOTIFY_HISTMAP)) && osd->imd && fd == osd->imd->image_fd)
		{
		DEBUG_1("Notify osd: %s %04x", fd->path, type);
		osd->notify |= type;
		image_osd_update_schedule(osd, FALSE);
		}
}


static void image_osd_free(OverlayStateData *osd)
{
	if (!osd) return;

	if (osd->idle_id) g_source_remove(osd->idle_id);
	if (osd->timer_id) g_source_remove(osd->timer_id);

	file_data_unregister_notify_func(image_osd_notify_cb, osd);

	if (osd->imd)
		{
		image_set_osd_data(osd->imd, NULL);
		g_signal_handler_disconnect(osd->imd->pr, osd->destroy_id);

		image_set_state_func(osd->imd, NULL, NULL);

		image_osd_info_hide(osd);
		image_osd_icons_hide(osd);
		}

	if (osd->histogram) histogram_free(osd->histogram);

	g_free(osd);
}

static void image_osd_destroy_cb(GtkWidget *widget, gpointer data)
{
	OverlayStateData *osd = data;

	osd->imd = NULL;
	image_osd_free(osd);
}

static void image_osd_enable(ImageWindow *imd, OsdShowFlags show)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	if (!osd)
		{
		osd = g_new0(OverlayStateData, 1);
		osd->imd = imd;
		osd->show = OSD_SHOW_NOTHING;
		osd->x = options->image_overlay.x;
		osd->y = options->image_overlay.y;

		osd->histogram = histogram_new();

		osd->destroy_id = g_signal_connect(G_OBJECT(imd->pr), "destroy",
						   G_CALLBACK(image_osd_destroy_cb), osd);
		image_set_osd_data(imd, osd);

		image_set_state_func(osd->imd, image_osd_state_cb, osd);
		file_data_register_notify_func(image_osd_notify_cb, osd, NOTIFY_PRIORITY_LOW);
		}

	if (show & OSD_SHOW_STATUS)
		image_osd_icon(imd, IMAGE_OSD_ICON, -1);

	if (show != osd->show)
		image_osd_update_schedule(osd, TRUE);

	osd->show = show;
}

void image_osd_set(ImageWindow *imd, OsdShowFlags show)
{
	if (!imd) return;

	image_osd_enable(imd, show);
}

OsdShowFlags image_osd_get(ImageWindow *imd)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	return osd ? osd->show : OSD_SHOW_NOTHING;
}

Histogram *image_osd_get_histogram(ImageWindow *imd)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	return osd ? osd->histogram : NULL;
}

void image_osd_copy_status(ImageWindow *src, ImageWindow *dest)
{
	Histogram *h_src, *h_dest;
	image_osd_set(dest, image_osd_get(src));

	h_src = image_osd_get_histogram(src);
	h_dest = image_osd_get_histogram(dest);

	h_dest->histogram_mode = h_src->histogram_mode;
	h_dest->histogram_channel = h_src->histogram_channel;

}

/* duration:
    0 = hide
    1 = show
   2+ = show for duration tenths of a second
   -1 = use default duration
 */
void image_osd_icon(ImageWindow *imd, ImageOSDFlag flag, gint duration)
{
	OverlayStateData *osd = image_get_osd_data(imd);

	if (!osd) return;

	if (flag >= IMAGE_OSD_COUNT) return;
	if (duration < 0) duration = IMAGE_OSD_DEFAULT_DURATION;
	if (duration > 1) duration += 1;

	osd->icon_time[flag] = duration;

	image_osd_update_schedule(osd, FALSE);
	image_osd_timer_schedule(osd);
}
/* vim: set shiftwidth=8 softtabstop=0 cindent cinoptions={1s: */
