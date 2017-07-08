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

FileClusterList *fileclusterlist_new()
{
	FileClusterList *fcl = g_new0(FileClusterList, 1);
	fcl->clusters = g_hash_table_new(&g_direct_hash, &g_direct_equal);
}

FileCluster *filecluster_new()
{
	FileCluster *fc = g_new0(FileCluster, 1);
}

void fileclusterlist_free(FileClusterList *fcl)
{
	if (fcl->fd_list) g_list_free_full(fcl->fd_list, (GDestroyNotify)&filecluster_free);
	g_hash_table_destroy(fcl->clusters);
	g_free(fcl);
}

void filecluster_free(FileCluster *fc)
{
	g_free(fc);
}

FileCluster *fileclusterlist_create_cluster(FileClusterList *fcl, GList *fd_items)
{
	GList *work;

	// Check preconditions.
	if (!fd_items) return NULL;
	// TODO(xsdg): Is this actually necessary?
	// if (!check_list_contains_sublist(fcl->fd_list, fd_items)) return NULL;
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
	new_fc->items = g_list_copy(fd_items);
	new_fc->head = new_fc->items;

	for (GList *item = new_fc->items; item; item = item->next)
		{
		FileData *fd = item->data;
		g_hash_table_insert(fcl->clusters, fd, new_fc);
		}

	return new_fc;
}

gboolean fileclusterlist_has_head(FileClusterList *fcl, FileData *fd)
{
	FileCluster *fc = g_hash_table_lookup(fcl->clusters, fd);
	if (!fc) return FALSE;
	return fc->head->data == fd;
}

gboolean fileclusterlist_has_child(FileClusterList *fcl, FileData *fd)
{
	FileCluster *fc = g_hash_table_lookup(fcl->clusters, fd);
	if (!fc) return FALSE;
	return fc->head->data != fd;
}

GList *filecluster_remove_children_from_list(FileClusterList *fcl, GList *list)
{
	GList *work = list;

	while (work)
	{
		FileData *fd = work->data;
		GList *link = work;
		// Advance early in case link needs to be removed/freed.
		work = work->next;

		if (fileclusterlist_has_child(fcl, fd))
		{
			list = g_list_remove_link(list, link);
			file_data_unref(fd);
			g_list_free(link);
		}
	}

	return list;
}
