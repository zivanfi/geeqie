/*
 * (SLIK) SimpLIstic sKin functions
 * (C) 2004 John Ellis
 * Copyright (C) 2008 The Geeqie Team
 *
 * Author: John Ellis
 *
 * This software is released under the GNU General Public License (GNU GPL).
 * Please read the included file COPYING for more information.
 * This software comes with no warranty of any kind, use at your own risk!
 */


#ifndef UI_FILEOPS_H
#define UI_FILEOPS_H


#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>



void print_term(const gchar *text_utf8);

#define printf_term(...) \
	do { \
		gchar *msg = g_strdup_printf(__VA_ARGS__); \
		print_term(msg); \
		g_free(msg); \
	} while (0)

gchar *path_to_utf8(const gchar *path);
gchar *path_from_utf8(const gchar *path);

const gchar *homedir(void);
gint stat_utf8(const gchar *s, struct stat *st);
gint lstat_utf8(const gchar *s, struct stat *st);
gint isname(const gchar *s);
gint isfile(const gchar *s);
gint isdir(const gchar *s);
gint islink(const gchar *s);
gint64 filesize(const gchar *s);
time_t filetime(const gchar *s);
gint filetime_set(const gchar *s, time_t tval);
gint access_file(const gchar *s, int mode);
gint unlink_file(const gchar *s);
gint symlink_utf8(const gchar *source, const gchar *target);
gint mkdir_utf8(const gchar *s, int mode);
gint rmdir_utf8(const gchar *s);
gint copy_file_attributes(const gchar *s, const gchar *t, gint perms, gint mtime);
gint copy_file(const gchar *s, const gchar *t);
gint move_file(const gchar *s, const gchar *t);
gint rename_file(const gchar *s, const gchar *t);
gchar *get_current_dir(void);

/* return True on success, it is up to you to free
 * the lists with string_list_free()
 */
gint path_list(const gchar *path, GList **files, GList **dirs);
gint path_list_lstat(const gchar *path, GList **files, GList **dirs);
void string_list_free(GList *list);
#define path_list_free string_list_free
GList *string_list_copy(GList *list);
#define path_list_copy string_list_copy

long checksum_simple(const gchar *path);


gchar *unique_filename(const gchar *path, const gchar *ext, const gchar *divider, gint pad);
gchar *unique_filename_simple(const gchar *path);

const gchar *filename_from_path(const gchar *path);
gchar *remove_level_from_path(const gchar *path);
gchar *concat_dir_and_file(const gchar *base, const gchar *name);

const gchar *extension_from_path(const gchar *path);
gchar *remove_extension_from_path(const gchar *path);

gint file_extension_match(const gchar *path, const gchar *ext);

/* warning note: this modifies path string! */
void parse_out_relatives(gchar *path);

gint file_in_path(const gchar *name);

#endif
