/* NetworkManager system settings service
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
 *
 * Copyright (C) 2007 - 2018 Red Hat, Inc.
 * Copyright (C) 2008 Novell, Inc.
 */

#include "nm-default.h"

#include "nm-settings-plugin.h"

#include "nm-utils.h"
#include "nm-core-internal.h"

#include "nm-settings-connection.h"

/*****************************************************************************/

enum {
	UNMANAGED_SPECS_CHANGED,
	UNRECOGNIZED_SPECS_CHANGED,

	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (NMSettingsPlugin, nm_settings_plugin, G_TYPE_OBJECT)

/*****************************************************************************/

GSList *
nm_settings_plugin_get_unmanaged_specs (NMSettingsPlugin *self)
{
	NMSettingsPluginClass *klass;

	g_return_val_if_fail (NM_IS_SETTINGS_PLUGIN (self), NULL);

	klass = NM_SETTINGS_PLUGIN_GET_CLASS (self);
	if (!klass->get_unmanaged_specs)
		return NULL;
	return klass->get_unmanaged_specs (self);
}

GSList *
nm_settings_plugin_get_unrecognized_specs (NMSettingsPlugin *self)
{
	NMSettingsPluginClass *klass;

	g_return_val_if_fail (NM_IS_SETTINGS_PLUGIN (self), NULL);

	klass = NM_SETTINGS_PLUGIN_GET_CLASS (self);
	if (!klass->get_unrecognized_specs)
		return NULL;
	return klass->get_unrecognized_specs (self);
}

void
nm_settings_plugin_reload_connections (NMSettingsPlugin *self,
                                       NMSettingsPluginConnectionReloadCallback callback,
                                       gpointer user_data)
{
	NMSettingsPluginClass *klass;

	g_return_if_fail (NM_IS_SETTINGS_PLUGIN (self));
	g_return_if_fail (callback);

	klass = NM_SETTINGS_PLUGIN_GET_CLASS (self);
	if (klass->reload_connections)
		klass->reload_connections (self, callback, user_data);
}

gboolean
nm_settings_plugin_load_connection (NMSettingsPlugin *self,
                                    const char *filename,
                                    NMSettingsStorage **out_storage,
                                    NMConnection **out_connection,
                                    GPtrArray **out_replaced_storages,
                                    GError **error)
{
	NMSettingsPluginClass *klass;

	g_return_val_if_fail (NM_IS_SETTINGS_PLUGIN (self), FALSE);
	g_return_val_if_fail (filename && filename[0] == '/', FALSE);
	g_return_val_if_fail (out_storage && !*out_storage, FALSE);
	g_return_val_if_fail (out_connection && !*out_connection, FALSE);
	g_return_val_if_fail (out_replaced_storages && !*out_replaced_storages, FALSE);

	klass = NM_SETTINGS_PLUGIN_GET_CLASS (self);
	if (!klass->load_connection) {
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_NOT_SUPPORTED,
		                     "settings plugin does not support loading connection");
		return FALSE;
	}
	return klass->load_connection (self, filename, out_storage, out_connection, out_replaced_storages, error);
}

gboolean
nm_settings_plugin_add_connection (NMSettingsPlugin *self,
                                   NMConnection *connection,
                                   NMSettingsStorage **out_storage,
                                   NMConnection **out_connection,
                                   GError **error)
{
	NMSettingsPluginClass *klass;

	g_return_val_if_fail (NM_IS_SETTINGS_PLUGIN (self), FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), FALSE);

#if NM_MORE_ASSERTS > 5
	nm_assert (nm_connection_verify (connection, NULL));
#endif

	NM_SET_OUT (out_storage, NULL);
	NM_SET_OUT (out_connection, NULL);

	klass = NM_SETTINGS_PLUGIN_GET_CLASS (self);
	if (!klass->add_connection) {
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_NOT_SUPPORTED,
		                     "settings plugin does not support adding connections");
		return FALSE;
	}
	return klass->add_connection (self,
	                              connection,
	                              out_storage,
	                              out_connection,
	                              error);
}

gboolean
nm_settings_plugin_update_connection (NMSettingsPlugin *self,
                                      NMSettingsStorage *storage,
                                      NMConnection *connection,
                                      NMSettingsStorage **out_storage,
                                      NMConnection **out_connection,
                                      GError **error)
{
	NMSettingsPluginClass *klass = NULL;

	g_return_val_if_fail (NM_IS_SETTINGS_PLUGIN (self), FALSE);
	g_return_val_if_fail (NM_IS_SETTINGS_STORAGE (storage), FALSE);
	g_return_val_if_fail (nm_settings_storage_get_plugin (storage) == self, FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), FALSE);

#if NM_MORE_ASSERTS > 5
	nm_assert (nm_connection_verify (connection, NULL));
	nm_assert (nm_streq (nm_connection_get_uuid (connection), nm_settings_storage_get_uuid (storage)));
#endif

	klass = NM_SETTINGS_PLUGIN_GET_CLASS (self);

	NM_SET_OUT (out_storage, NULL);
	NM_SET_OUT (out_connection, NULL);

	if (!klass->update_connection) {
		g_set_error (error,
		             NM_SETTINGS_ERROR,
		             NM_SETTINGS_ERROR_NOT_SUPPORTED,
		             "settings plugin does not support modifying connections");
		return FALSE;
	}
	return klass->update_connection (self,
	                                 storage,
	                                 connection,
	                                 out_storage,
	                                 out_connection,
	                                 error);
}

gboolean
nm_settings_plugin_delete_connection (NMSettingsPlugin *self,
                                      NMSettingsStorage *storage,
                                      gboolean remove_from_disk,
                                      GError **error)
{
	NMSettingsPluginClass *klass = NULL;
	gs_unref_object NMConnection *connection_clone = NULL;

	g_return_val_if_fail (NM_IS_SETTINGS_PLUGIN (self), FALSE);
	g_return_val_if_fail (NM_IS_SETTINGS_STORAGE (storage), FALSE);
	g_return_val_if_fail (nm_settings_storage_get_plugin (storage) == self, FALSE);

	klass = NM_SETTINGS_PLUGIN_GET_CLASS (self);

	if (!klass->delete_connection) {
		g_set_error (error,
		             NM_SETTINGS_ERROR,
		             NM_SETTINGS_ERROR_NOT_SUPPORTED,
		             "settings plugin does not support deleting connections");
		return FALSE;
	}

	return klass->delete_connection (self,
	                                 storage,
	                                 remove_from_disk,
	                                 error);
}

/*****************************************************************************/

void
_nm_settings_plugin_emit_signal_unmanaged_specs_changed (NMSettingsPlugin *self)
{
	nm_assert (NM_IS_SETTINGS_PLUGIN (self));

	g_signal_emit (self, signals[UNMANAGED_SPECS_CHANGED], 0);
}

void
_nm_settings_plugin_emit_signal_unrecognized_specs_changed (NMSettingsPlugin *self)
{
	nm_assert (NM_IS_SETTINGS_PLUGIN (self));

	g_signal_emit (self, signals[UNRECOGNIZED_SPECS_CHANGED], 0);
}

/*****************************************************************************/

static void
nm_settings_plugin_init (NMSettingsPlugin *self)
{
}

static void
nm_settings_plugin_class_init (NMSettingsPluginClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	signals[UNMANAGED_SPECS_CHANGED] =
	    g_signal_new (NM_SETTINGS_PLUGIN_UNMANAGED_SPECS_CHANGED,
	                  G_OBJECT_CLASS_TYPE (object_class),
	                  G_SIGNAL_RUN_FIRST,
	                  0, NULL, NULL,
	                  g_cclosure_marshal_VOID__VOID,
	                  G_TYPE_NONE, 0);

	signals[UNRECOGNIZED_SPECS_CHANGED] =
	    g_signal_new (NM_SETTINGS_PLUGIN_UNRECOGNIZED_SPECS_CHANGED,
	                  G_OBJECT_CLASS_TYPE (object_class),
	                  G_SIGNAL_RUN_FIRST,
	                  0, NULL, NULL,
	                  g_cclosure_marshal_VOID__VOID,
	                  G_TYPE_NONE, 0);
}
