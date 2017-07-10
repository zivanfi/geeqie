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

#ifndef FILECLUSTER_H
#define FILECLUSTER_H

#include "main.h"

// FileCluster methods.
FileCluster *filecluster_new();  // internal?
void filecluster_free(FileCluster *fc);

gboolean filecluster_toggle_show_children(FileCluster *fc);
gboolean filecluster_has_head(FileCluster *fc, FileData *fd);
gboolean filecluster_has_child(FileCluster *fc, FileData *fd);


// FileClusterList methods.

FileClusterList *fileclusterlist_new();
void fileclusterlist_free(FileClusterList *fcl);

// Creates a cluster with items that must already be in the cluster list.  Will fail (and make no
// changes) if any of the specified items isn't in the list, or if any of the items is already
// part of a different cluster.
FileCluster *fileclusterlist_create_cluster(FileClusterList *fcl, GList *fd_items);

gboolean fileclusterlist_has_head(FileClusterList *fcl, FileData *fd);
gboolean fileclusterlist_has_child(FileClusterList *fcl, FileData *fd);

GList *fileclusterlist_next_non_child(FileClusterList *fcl, GList *list);
GList *fileclusterlist_remove_children_from_list(FileClusterList *fcl, GList *list);

#endif  // FILECLUSTER_H

/* vim: set shiftwidth=8 softtabstop=0 cindent cinoptions={1s: */
