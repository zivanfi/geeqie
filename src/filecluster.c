/*
 * Copyright (C) 2008 - 2016 The Geeqie Team
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

#include "filecluster.h"

#include "filedata.h"

static gboolean check_list_contains_sublist(GList *haystack, GList *needle)
{
	// TODO(xsdg): Optimize this!  Sort, then scan?
	GList *h_work, *n_work;
	for (n_work = needle; n_work; n_work = n_work->next)
	{
		gboolean found = FALSE;
		for (h_work = haystack; h_work; h_work = h_work->next)
		{
			if (n_work == h_work)
				{
				found = TRUE;
				break;
				}
		}

		if (!found) return FALSE;
	}

	return TRUE;
}

static gboolean filecluster_fd_equal(gconstpointer ptr_a, gconstpointer ptr_b)
{
	// TODO(xsdg): Is there anything we can/should do to validate inputs?
	FileData *fd_a = (FileData *)ptr_a;
	FileData *fd_b = (FileData *)ptr_b;
	return !filelist_sort_compare_filedata(fd_a, fd_b);
}

// TODO(xsdg): Move this into filedata.h
static guint filecluster_fd_hash(gconstpointer ptr)
{
	if (!ptr) return 1;
	FileData *fd = (FileData *)ptr;
	return 7 * g_str_hash(fd->original_path);
}

FileClusterList *fileclusterlist_new()
{
	FileClusterList *fcl = g_new0(FileClusterList, 1);
	fcl->clusters = g_hash_table_new(&filecluster_fd_hash, &filecluster_fd_equal);
	return fcl;
}

FileCluster *filecluster_new()
{
	FileCluster *fc = g_new0(FileCluster, 1);
	fc->show_children = FALSE;
	return fc;
}

void fileclusterlist_free(FileClusterList *fcl)
{
	// TODO(xsdg): don't leak stuff
	// if (fcl->fd_list) g_list_free_full(fcl->fd_list, (GDestroyNotify)&filecluster_free);
	g_hash_table_destroy(fcl->clusters);
	g_free(fcl);
}

void filecluster_free(FileCluster *fc)
{
	filelist_free(fc->items);
	g_free(fc);
}

gboolean filecluster_toggle_show_children(FileCluster *fc)
{
	fc->show_children = !fc->show_children;
	return fc->show_children;
}

FileCluster *fileclusterlist_create_cluster(FileClusterList *fcl, GList *fd_items)
{
	GList *work;

	// Check preconditions.
	if (!fd_items) return NULL;
	for (work = fd_items; work; work = work->next)
		{
		FileData *fd = work->data;
		if (g_hash_table_contains(fcl->clusters, fd))
			{
			// TODO(xsdg): Show this warning in the UI.
			g_warning("Tried to create a cluster with a file that is already clustered.");
			return NULL;
			}
		}

	FileCluster *new_fc = filecluster_new();
	new_fc->items = filelist_copy(fd_items);
	new_fc->head = new_fc->items;

	for (GList *item = new_fc->items; item; item = item->next)
		{
		FileData *fd = item->data;
		g_hash_table_insert(fcl->clusters, fd, new_fc);
		}

	return new_fc;
}

gboolean filecluster_has_head(FileCluster *fc, FileData *fd)
{
	if (!fd) return FALSE;
	return filecluster_fd_equal(fc->head->data, fd);
}

gboolean filecluster_has_child(FileCluster *fc, FileData *fd)
{
	if (!fd) return FALSE;
	return !filecluster_fd_equal(fc->head->data, fd);
}

gboolean fileclusterlist_has_head(FileClusterList *fcl, FileData *fd)
{
	FileCluster *fc = g_hash_table_lookup(fcl->clusters, fd);
	if (!fc) return FALSE;
	return filecluster_has_head(fc, fd);
}

gboolean fileclusterlist_has_child(FileClusterList *fcl, FileData *fd)
{
	FileCluster *fc = g_hash_table_lookup(fcl->clusters, fd);
	if (!fc) return FALSE;
	return filecluster_has_child(fc, fd);
}

static gboolean fileclusterlist_should_hide(FileClusterList *fcl, FileData *fd)
{
	FileCluster *fc = g_hash_table_lookup(fcl->clusters, fd);
	if (!fc) return FALSE;
	// Only difference vs. fileclusterlist_has_child.  Basically, if the node is a child, but
	// we're showing children, then don't hide.
	if (fc->show_children) return FALSE;
	return filecluster_has_child(fc, fd);
}

// TODO(xsdg): pick a better name for this function
GList *fileclusterlist_next_non_child(FileClusterList *fcl, GList *list)
{
	// Check for no-ops
	if (!list || !g_hash_table_size(fcl->clusters)) return list;

	// Clusters are being used, so we have to actually check things.
	for (; list; list = list->next)
	{
		FileData *fd = list->data;
		if (!fileclusterlist_has_child(fcl, fd)) return list;
	}

	return NULL;
}

GList *fileclusterlist_remove_children_from_list(FileClusterList *fcl, GList *list)
{
	GList *work = list;

	while (work)
	{
		FileData *fd = work->data;
		GList *link = work;
		// Advance early in case link needs to be removed/freed.
		work = work->next;

		if (fileclusterlist_should_hide(fcl, fd))
		{
			list = g_list_remove_link(list, link);
			file_data_unref(fd);
			g_list_free(link);
		}
	}

	return list;
}
