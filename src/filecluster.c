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
	if (!fd_items) return NULL;
	if (!check_list_contains_sublist(fcl->fd_list, fd_items)) return NULL;
	for (work = fd_items; work; work = work->next)
	{
		FileData *fd = work->data;
		if (g_hash_table_contains(fcl->clusters, fd)) return NULL;
	}

	FileCluster *new_fc = filecluster_new();
	new_fc->items = g_list_copy(fd_items);
	new_fc->head = new_fc->items;

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
