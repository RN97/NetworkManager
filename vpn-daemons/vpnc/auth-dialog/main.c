/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager Wireless Applet -- Display wireless access points and allow user control
 *
 * Dan Williams <dcbw@redhat.com>
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
 * (C) Copyright 2004 - 2008 Red Hat, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gnome-keyring.h>
#include <gnome-keyring-memory.h>
#include <gconf/gconf-client.h>

#include <nm-setting-vpn.h>
#include <nm-setting-connection.h>

#include "common-gnome/keyring-helpers.h"
#include "src/nm-vpnc-service.h"
#include "gnome-two-password-dialog.h"

#define KEYRING_UUID_TAG "connection-uuid"
#define KEYRING_SN_TAG "setting-name"
#define KEYRING_SK_TAG "setting-key"

static gboolean
get_secrets (const char *vpn_uuid,
             const char *vpn_name,
             gboolean retry,
             char **upw,
             const char *upw_type,
             char **gpw,
             const char *gpw_type)
{
	GnomeTwoPasswordDialog *dialog;
	gboolean is_session = TRUE;
	gboolean found_upw = FALSE;
	gboolean found_gpw = FALSE;
	char *prompt;
	gboolean success = FALSE;

	g_return_val_if_fail (vpn_uuid != NULL, FALSE);
	g_return_val_if_fail (vpn_name != NULL, FALSE);
	g_return_val_if_fail (upw != NULL, FALSE);
	g_return_val_if_fail (*upw == NULL, FALSE);
	g_return_val_if_fail (gpw != NULL, FALSE);
	g_return_val_if_fail (*gpw == NULL, FALSE);

	/* Default to 'save' to keep same behavior as previous versions before
	 * password types were added.
	 */
	if (!upw_type)
		upw_type = NM_VPNC_PW_TYPE_SAVE;
	if (!gpw_type)
		gpw_type = NM_VPNC_PW_TYPE_SAVE;

	if (strcmp (upw_type, NM_VPNC_PW_TYPE_ASK))
		found_upw = keyring_helpers_get_one_secret (vpn_uuid, VPNC_USER_PASSWORD, upw, &is_session);

	if (strcmp (gpw_type, NM_VPNC_PW_TYPE_ASK)) 
		found_gpw = keyring_helpers_get_one_secret (vpn_uuid, VPNC_GROUP_PASSWORD, gpw, &is_session);

	if (!retry) {
		gboolean need_upw = TRUE, need_gpw = TRUE;

		/* Don't ask if both passwords are either saved and present, or unused */
		if (   (!strcmp (upw_type, NM_VPNC_PW_TYPE_SAVE) && found_upw && *upw)
		    || (!upw_type && found_upw && *upw)  /* treat unknown type as "save" */
		    || !strcmp (upw_type, NM_VPNC_PW_TYPE_UNUSED))
			need_upw = FALSE;

		if (   (!strcmp (gpw_type, NM_VPNC_PW_TYPE_SAVE) && found_gpw && *gpw)
		    || (!gpw_type && found_gpw && *gpw)  /* treat unknown type as "save" */
		    || !strcmp (gpw_type, NM_VPNC_PW_TYPE_UNUSED))
			need_gpw = FALSE;

		if (!need_upw && !need_gpw)
			return TRUE;
	} else {
		/* Don't ask if both passwords are unused */
		if (   !strcmp (upw_type, NM_VPNC_PW_TYPE_UNUSED)
		    && !strcmp (gpw_type, NM_VPNC_PW_TYPE_UNUSED))
			return TRUE;
	}

	prompt = g_strdup_printf (_("You need to authenticate to access the Virtual Private Network '%s'."), vpn_name);
	dialog = GNOME_TWO_PASSWORD_DIALOG (gnome_two_password_dialog_new (_("Authenticate VPN"), prompt, NULL, NULL, FALSE));
	g_free (prompt);

	gnome_two_password_dialog_set_show_username (dialog, FALSE);
	gnome_two_password_dialog_set_show_userpass_buttons (dialog, FALSE);
	gnome_two_password_dialog_set_show_domain (dialog, FALSE);
	gnome_two_password_dialog_set_show_remember (dialog, FALSE);
	gnome_two_password_dialog_set_password_secondary_label (dialog, _("_Group Password:"));

	if (!strcmp (upw_type, NM_VPNC_PW_TYPE_UNUSED))
		gnome_two_password_dialog_set_show_password (dialog, FALSE);
	else if (!retry && found_upw && strcmp (upw_type, NM_VPNC_PW_TYPE_ASK))
		gnome_two_password_dialog_set_show_password (dialog, FALSE);

	if (!strcmp (gpw_type, NM_VPNC_PW_TYPE_UNUSED))
		gnome_two_password_dialog_set_show_password_secondary (dialog, FALSE);
	else if (!retry && found_gpw && strcmp (gpw_type, NM_VPNC_PW_TYPE_ASK))
		gnome_two_password_dialog_set_show_password_secondary (dialog, FALSE);

	/* On reprompt the first entry of type 'ask' gets the focus */
	if (retry) {
		if (!strcmp (upw_type, NM_VPNC_PW_TYPE_ASK))
			gnome_two_password_dialog_focus_password (dialog);
		else if (!strcmp (gpw_type, NM_VPNC_PW_TYPE_ASK))
			gnome_two_password_dialog_focus_password_secondary (dialog);
	}

	/* if retrying, pre-fill dialog with the password */
	if (*upw) {
		gnome_two_password_dialog_set_password (dialog, *upw);
		gnome_keyring_memory_free (*upw);
		*upw = NULL;
	}
	if (*gpw) {
		gnome_two_password_dialog_set_password_secondary (dialog, *gpw);
		gnome_keyring_memory_free (*gpw);
		*gpw = NULL;
	}

	gtk_widget_show (GTK_WIDGET (dialog));

	success = gnome_two_password_dialog_run_and_block (dialog);
	if (success) {
		*upw = gnome_two_password_dialog_get_password (dialog);
		*gpw = gnome_two_password_dialog_get_password_secondary (dialog);

		if (!strcmp (upw_type, NM_VPNC_PW_TYPE_SAVE))
			keyring_helpers_save_secret (vpn_uuid, vpn_name, NULL, VPNC_USER_PASSWORD, *upw);

		if (!strcmp (gpw_type, NM_VPNC_PW_TYPE_SAVE))
			keyring_helpers_save_secret (vpn_uuid, vpn_name, NULL, VPNC_GROUP_PASSWORD, *gpw);
	}

	gtk_widget_hide (GTK_WIDGET (dialog));
	gtk_widget_destroy (GTK_WIDGET (dialog));

	return success;
}

static gboolean
get_password_types (const char *vpn_uuid,
                    char **out_upw_type,
                    char **out_gpw_type)
{
	GConfClient *gconf_client = NULL;
	GSList *conf_list;
	GSList *iter;
	char *key;
	char *str;
	char *connection_path = NULL;
	gboolean success = FALSE;
	char *upw_type = NULL, *gpw_type = NULL;

	/* FIXME: This whole thing sucks: we should not go around poking gconf
	   directly, but there's nothing that does it for us right now */

	gconf_client = gconf_client_get_default ();

	conf_list = gconf_client_all_dirs (gconf_client, "/system/networking/connections", NULL);
	if (!conf_list)
		goto out;

	for (iter = conf_list; iter; iter = iter->next) {
		const char *path = (const char *) iter->data;

		key = g_strdup_printf ("%s/%s/%s", 
		                       path,
		                       NM_SETTING_CONNECTION_SETTING_NAME,
		                       NM_SETTING_CONNECTION_TYPE);
		str = gconf_client_get_string (gconf_client, key, NULL);
		g_free (key);

		if (!str || strcmp (str, "vpn")) {
			g_free (str);
			continue;
		}
		g_free (str);

		key = g_strdup_printf ("%s/%s/%s", 
		                       path,
		                       NM_SETTING_CONNECTION_SETTING_NAME,
		                       NM_SETTING_CONNECTION_UUID);
		str = gconf_client_get_string (gconf_client, key, NULL);
		g_free (key);

		if (!str || strcmp (str, vpn_uuid)) {
			g_free (str);
			continue;
		}
		g_free (str);

		/* Woo, found the connection */
		connection_path = g_strdup (path);
		break;
	}

	g_slist_foreach (conf_list, (GFunc) g_free, NULL);
	g_slist_free (conf_list);

	if (!connection_path)
		goto out;

	key = g_strdup_printf ("%s/%s/%s", connection_path,
	                       NM_SETTING_VPN_SETTING_NAME,
	                       NM_VPNC_KEY_XAUTH_PASSWORD_TYPE);
	*out_upw_type = gconf_client_get_string (gconf_client, key, NULL);
	g_free (key);

	key = g_strdup_printf ("%s/%s/%s", connection_path,
	                       NM_SETTING_VPN_SETTING_NAME,
	                       NM_VPNC_KEY_SECRET_TYPE);
	*out_gpw_type = gconf_client_get_string (gconf_client, key, NULL);
	g_free (key);
	
	g_free (connection_path);
	success = TRUE;

out:
	g_object_unref (gconf_client);
	return success;
}

int 
main (int argc, char *argv[])
{
	gboolean retry = FALSE;
	gchar *vpn_name = NULL;
	gchar *vpn_uuid = NULL;
	gchar *vpn_service = NULL;
	char *password = NULL, *group_password = NULL;
	char *upw_type = NULL, *gpw_type = NULL;
	char buf[1];
	int ret;
	GError *error = NULL;
	GOptionContext *context;
	GOptionEntry entries[] = {
			{ "reprompt", 'r', 0, G_OPTION_ARG_NONE, &retry, "Reprompt for passwords", NULL},
			{ "uuid", 'u', 0, G_OPTION_ARG_STRING, &vpn_uuid, "UUID of VPN connection", NULL},
			{ "name", 'n', 0, G_OPTION_ARG_STRING, &vpn_name, "Name of VPN connection", NULL},
			{ "service", 's', 0, G_OPTION_ARG_STRING, &vpn_service, "VPN service type", NULL},
			{ NULL }
		};

	bindtextdomain (GETTEXT_PACKAGE, NULL);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	gtk_init (&argc, &argv);
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new ("- vpnc auth dialog");
	g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);

	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_warning ("Error parsing options: %s\n", error->message);
		g_error_free (error);
		return 1;
	}

	g_option_context_free (context);

	if (vpn_uuid == NULL || vpn_name == NULL || vpn_service == NULL) {
		fprintf (stderr, "Have to supply UUID, name, and service\n");
		return 1;
	}

	if (strcmp (vpn_service, NM_DBUS_SERVICE_VPNC) != 0) {
		fprintf (stderr, "This dialog only works with the '%s' service\n", NM_DBUS_SERVICE_VPNC);
		return 1;
	}

	if (!get_password_types (vpn_uuid, &upw_type, &gpw_type)) {
		g_free (upw_type);
		g_free (gpw_type);
		fprintf (stderr, "This VPN connection '%s' (%s) could not be found in GConf.", vpn_name, vpn_uuid);
		return 1;
	}

	if (!get_secrets (vpn_uuid, vpn_name, retry, &password, upw_type, &group_password, gpw_type)) {
		g_free (upw_type);
		g_free (gpw_type);
		return 1;
	}
	g_free (upw_type);
	g_free (gpw_type);

	/* dump the passwords to stdout */
	if (password)
		printf ("%s\n%s\n", NM_VPNC_KEY_XAUTH_PASSWORD, password);
	if (group_password)
		printf ("%s\n%s\n", NM_VPNC_KEY_SECRET, group_password);
	printf ("\n\n");

	if (password) {
		memset (password, 0, strlen (password));
		gnome_keyring_memory_free (password);
	}
	if (group_password) {
		memset (group_password, 0, strlen (group_password));
		gnome_keyring_memory_free (group_password);
	}

	/* for good measure, flush stdout since Kansas is going Bye-Bye */
	fflush (stdout);

	/* wait for data on stdin  */
	ret = fread (buf, sizeof (char), sizeof (buf), stdin);
	return 0;
}
