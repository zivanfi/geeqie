/*
 * Geeqie
 * Copyright (C) 2008 - 2010 The Geeqie Team
 *
 * Authors: Vladimir Nadvornik / Laurent Monin
 *
 * This software is released under the GNU General Public License (GNU GPL).
 * Please read the included file COPYING for more information.
 * This software comes with no warranty of any kind, use at your own risk!
 */

#include "main.h"
#include "compat.h"

/* gtk_radio_action_set_current_value() replacement for GTK+ < 2.10 */
void radio_action_set_current_value(GtkRadioAction *action, gint current_value)
{
#if GTK_CHECK_VERSION(2, 10, 0)
	gtk_radio_action_set_current_value(action, current_value);
#else
	GSList *group;
	gint value;

	group = gtk_radio_action_get_group(action);
	while (group)
		{
		action = GTK_RADIO_ACTION(group->data);
		g_object_get(G_OBJECT(action), "value", &value, NULL);
		if (value == current_value)
			{
			gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), TRUE);
			return;
			}
		group = g_slist_next(group);
		}
#endif
}

#if !GLIB_CHECK_VERSION(2, 14, 0)
static void hash_table_add(gpointer key, gpointer value, gpointer user_data)
{
	GList **list = user_data;
	*list = g_list_prepend(*list, key);
}
#endif

GList* hash_table_get_keys(GHashTable *hash_table)
{
#if GLIB_CHECK_VERSION(2, 14, 0)
	return g_hash_table_get_keys(hash_table);
#else
	GList *list = NULL;
	g_hash_table_foreach(hash_table, hash_table_add, &list);
	return list;
#endif
}


void setWidgetHasFocus(GtkWidget *widget, gboolean hasFocus)
{
	g_object_set(widget, "has-focus", hasFocus, NULL);

	// These functions are deprecated in GTK+ 2.22, yet theme engines still look
	// at these flags when determining if a widget has focus, so we must use them.
	if (hasFocus)
		(GTK_OBJECT (widget)->flags) |= GTK_HAS_FOCUS;
	else
		(GTK_OBJECT (widget)->flags) &= ~(GTK_HAS_FOCUS);
}

void setWidgetCanFocus(GtkWidget *widget, gboolean canFocus)
{
#if GTK_CHECK_VERSION(2,18,0)
	gtk_widget_set_can_focus(widget, canFocus);
#else
	if (canFocus)
 		GTK_WIDGET_SET_FLAGS(widget, GTK_CAN_FOCUS);
	else
		GTK_WIDGET_UNSET_FLAGS(widget, GTK_CAN_FOCUS);
#endif
}

void setWidgetCanDefault(GtkWidget *widget, gboolean canDefault)
{
#if GTK_CHECK_VERSION(2,18,0)
	gtk_widget_set_can_default(widget, canDefault);
#else
	if (canDefault)
 		GTK_WIDGET_SET_FLAGS(widget, GTK_CAN_DEFAULT);
	else
		GTK_WIDGET_UNSET_FLAGS(widget, GTK_CAN_DEFAULT);
#endif
}

/* vim: set shiftwidth=8 softtabstop=0 cindent cinoptions={1s: */
