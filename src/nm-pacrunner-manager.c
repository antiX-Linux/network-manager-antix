/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager
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
 * (C) Copyright 2016 Atul Anand <atulhjp@gmail.com>.
 */

#include "nm-default.h"

#include "nm-pacrunner-manager.h"

#include "nm-utils.h"
#include "platform/nm-platform.h"
#include "nm-proxy-config.h"
#include "nm-ip4-config.h"
#include "nm-ip6-config.h"

#define PACRUNNER_DBUS_SERVICE "org.pacrunner"
#define PACRUNNER_DBUS_INTERFACE "org.pacrunner.Manager"
#define PACRUNNER_DBUS_PATH "/org/pacrunner/manager"

/*****************************************************************************/

struct remove_data {
	char *iface;
	char *path;
};

typedef struct {
	char *iface;
	GPtrArray *domains;
	GDBusProxy *pacrunner;
	GCancellable *pacrunner_cancellable;
	GList *args;
	GList *remove;
} NMPacrunnerManagerPrivate;

struct _NMPacrunnerManager {
	GObject parent;
	NMPacrunnerManagerPrivate _priv;
};

struct _NMPacrunnerManagerClass {
	GObjectClass parent;
};

G_DEFINE_TYPE (NMPacrunnerManager, nm_pacrunner_manager, G_TYPE_OBJECT)

#define NM_PACRUNNER_MANAGER_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMPacrunnerManager, NM_IS_PACRUNNER_MANAGER)

/*****************************************************************************/

NM_DEFINE_SINGLETON_GETTER (NMPacrunnerManager, nm_pacrunner_manager_get, NM_TYPE_PACRUNNER_MANAGER);

/*****************************************************************************/

#define _NMLOG_DOMAIN      LOGD_PROXY
#define _NMLOG(level, ...) __NMLOG_DEFAULT_WITH_ADDR (level, _NMLOG_DOMAIN, "pacrunner", __VA_ARGS__)

/*****************************************************************************/

static void
remove_data_destroy (struct remove_data *data)
{
	g_return_if_fail (data != NULL);

	g_free (data->iface);
	g_free (data->path);
	memset (data, 0, sizeof (struct remove_data));
	g_free (data);
}

static void
add_proxy_config (NMPacrunnerManager *self, GVariantBuilder *proxy_data, const NMProxyConfig *proxy_config)
{
	const char *pac_url, *pac_script;
	NMProxyConfigMethod method;

	method = nm_proxy_config_get_method (proxy_config);

	if (method == NM_PROXY_CONFIG_METHOD_AUTO) {
		pac_url = nm_proxy_config_get_pac_url (proxy_config);
		if (pac_url) {
			g_variant_builder_add (proxy_data, "{sv}",
			                       "URL",
			                       g_variant_new_string (pac_url));
		}

		pac_script = nm_proxy_config_get_pac_script (proxy_config);
		if (pac_script) {
			g_variant_builder_add (proxy_data, "{sv}",
			                       "Script",
			                       g_variant_new_string (pac_script));
		}
	}

	g_variant_builder_add (proxy_data, "{sv}",
	                       "BrowserOnly",
	                       g_variant_new_boolean (nm_proxy_config_get_browser_only (proxy_config)));
}

static void
add_ip4_config (NMPacrunnerManager *self, GVariantBuilder *proxy_data, NMIP4Config *ip4)
{
	NMPacrunnerManagerPrivate *priv = NM_PACRUNNER_MANAGER_GET_PRIVATE (self);
	int i;
	char *cidr = NULL;

	/* Extract searches */
	for (i = 0; i < nm_ip4_config_get_num_searches (ip4); i++)
		g_ptr_array_add (priv->domains, g_strdup (nm_ip4_config_get_search (ip4, i)));

	/* Extract domains */
	for (i = 0; i < nm_ip4_config_get_num_domains (ip4); i++)
		g_ptr_array_add (priv->domains, g_strdup (nm_ip4_config_get_domain (ip4, i)));

	/* Add addresses and routes in CIDR form */
	for (i = 0; i < nm_ip4_config_get_num_addresses (ip4); i++) {
		const NMPlatformIP4Address *address = nm_ip4_config_get_address (ip4, i);

		cidr = g_strdup_printf ("%s/%u",
		                        nm_utils_inet4_ntop (address->address, NULL),
		                        address->plen);
		g_ptr_array_add (priv->domains, g_strdup (cidr));
		g_free (cidr);
	}

	for (i = 0; i < nm_ip4_config_get_num_routes (ip4); i++) {
		const NMPlatformIP4Route *routes = nm_ip4_config_get_route (ip4, i);

		cidr = g_strdup_printf ("%s/%u",
		                        nm_utils_inet4_ntop (routes->network, NULL),
		                        routes->plen);
		g_ptr_array_add (priv->domains, g_strdup (cidr));
		g_free (cidr);
	}
}

static void
add_ip6_config (NMPacrunnerManager *self, GVariantBuilder *proxy_data, NMIP6Config *ip6)
{
	NMPacrunnerManagerPrivate *priv = NM_PACRUNNER_MANAGER_GET_PRIVATE (self);
	int i;
	char *cidr = NULL;

	/* Extract searches */
	for (i = 0; i < nm_ip6_config_get_num_searches (ip6); i++)
		g_ptr_array_add (priv->domains, g_strdup (nm_ip6_config_get_search (ip6, i)));

	/* Extract domains */
	for (i = 0; i < nm_ip6_config_get_num_domains (ip6); i++)
		g_ptr_array_add (priv->domains, g_strdup (nm_ip6_config_get_domain (ip6, i)));

	/* Add addresses and routes in CIDR form */
	for (i = 0; i < nm_ip6_config_get_num_addresses (ip6); i++) {
		const NMPlatformIP6Address *address = nm_ip6_config_get_address (ip6, i);

		cidr = g_strdup_printf ("%s/%u",
		                        nm_utils_inet6_ntop (&address->address, NULL),
		                        address->plen);
		g_ptr_array_add (priv->domains, g_strdup (cidr));
		g_free (cidr);
	}

	for (i = 0; i < nm_ip6_config_get_num_routes (ip6); i++) {
		const NMPlatformIP6Route *routes = nm_ip6_config_get_route (ip6, i);

		cidr = g_strdup_printf ("%s/%u",
		                        nm_utils_inet6_ntop (&routes->network, NULL),
		                        routes->plen);
		g_ptr_array_add (priv->domains, g_strdup (cidr));
		g_free (cidr);
	}
}

static void
pacrunner_send_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
	NMPacrunnerManager *self = NM_PACRUNNER_MANAGER (user_data);
	NMPacrunnerManagerPrivate *priv = NM_PACRUNNER_MANAGER_GET_PRIVATE (self);
	gs_free_error GError *error = NULL;
	gs_unref_variant GVariant *variant = NULL;
	const char *path = NULL;
	GList *iter = NULL;
	gboolean found = FALSE;

	variant = g_dbus_proxy_call_finish (priv->pacrunner, res, &error);
	if (!variant) {
		_LOGD ("sending proxy config to pacrunner failed: %s", error->message);
	} else {
		struct remove_data *data;
		g_variant_get (variant, "(&o)", &path);

		/* Replace the old path (if any) of proxy config with the new one returned
		 * from CreateProxyConfiguration() DBus method on pacrunner.
		 */
		for (iter = g_list_first (priv->remove); iter; iter = g_list_next (iter)) {
			struct remove_data *r = iter->data;
			if (g_strcmp0 (priv->iface, r->iface) == 0) {
				g_free (r->path);
				r->path = g_strdup (path);
				found = TRUE;
				break;
			}
		}

		if (!found) {
			data = g_malloc0 (sizeof (struct remove_data));
			data->iface = g_strdup (priv->iface);
			data->path = g_strdup (path);
			priv->remove = g_list_append (priv->remove, data);
			_LOGD ("proxy config sent to pacrunner");
		}
	}
}

static void
send_pacrunner_proxy_data (NMPacrunnerManager *self, GVariant *pacrunner_manager_args)
{
	NMPacrunnerManagerPrivate *priv = NM_PACRUNNER_MANAGER_GET_PRIVATE (self);

	if (!pacrunner_manager_args)
		return;

	if (priv->pacrunner)
		g_dbus_proxy_call (priv->pacrunner,
		                   "CreateProxyConfiguration",
		                   pacrunner_manager_args,
		                   G_DBUS_CALL_FLAGS_NONE,
		                   -1,
		                   NULL,
		                  (GAsyncReadyCallback) pacrunner_send_done,
		                   self);
}

static void
name_owner_changed (GObject *object,
                    GParamSpec *pspec,
                    gpointer user_data)
{
	NMPacrunnerManager *self = NM_PACRUNNER_MANAGER (user_data);
	NMPacrunnerManagerPrivate *priv = NM_PACRUNNER_MANAGER_GET_PRIVATE (self);
	gs_free char *owner = NULL;
	GList *iter = NULL;

	owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (object));
	if (owner) {
		_LOGD ("pacrunner appeared as %s", owner);
		for (iter = g_list_first(priv->args); iter; iter = g_list_next(iter)) {
			send_pacrunner_proxy_data (self, iter->data);
		}
	} else {
		_LOGD ("pacrunner disappeared");
	}
}

static void
pacrunner_proxy_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
	NMPacrunnerManager *self = user_data;
	NMPacrunnerManagerPrivate *priv;
	GError *error = NULL;
	GDBusProxy *proxy;

	proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
	if (!proxy) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			_LOGW ("failed to connect to pacrunner via DBus: %s", error->message);
		g_error_free (error);
		return;
	}

	priv = NM_PACRUNNER_MANAGER_GET_PRIVATE (self);

	priv->pacrunner = proxy;
	nm_clear_g_cancellable (&priv->pacrunner_cancellable);

	g_signal_connect (priv->pacrunner, "notify::g-name-owner",
	                  G_CALLBACK (name_owner_changed), self);
}

/**
 * nm_pacrunner_manager_send:
 * @self: the #NMPacrunnerManager
 * @iface: the iface for the connection or %NULL
 * @proxy_config: proxy config of the connection
 * @ip4_config: IP4 config of the connection
 * @ip6_config: IP6 config of the connection
 */
void
nm_pacrunner_manager_send (NMPacrunnerManager *self,
                           const char *iface,
                           NMProxyConfig *proxy_config,
                           NMIP4Config *ip4_config,
                           NMIP6Config *ip6_config)
{
	char **strv = NULL;
	NMProxyConfigMethod method;
	NMPacrunnerManagerPrivate *priv;
	GVariantBuilder proxy_data;
	GVariant *pacrunner_manager_args;

	g_return_if_fail (NM_IS_PACRUNNER_MANAGER (self));
	g_return_if_fail (proxy_config);

	priv = NM_PACRUNNER_MANAGER_GET_PRIVATE (self);

	g_free (priv->iface);
	priv->iface = g_strdup (iface);

	g_variant_builder_init (&proxy_data, G_VARIANT_TYPE_VARDICT);

	if (iface) {
		g_variant_builder_add (&proxy_data, "{sv}",
		                       "Interface",
		                       g_variant_new_string (iface));
	}

	method = nm_proxy_config_get_method (proxy_config);
	switch (method) {
	case NM_PROXY_CONFIG_METHOD_AUTO:
		g_variant_builder_add (&proxy_data, "{sv}",
		                       "Method",
		                       g_variant_new_string ("auto"));

		break;
	case NM_PROXY_CONFIG_METHOD_NONE:
		g_variant_builder_add (&proxy_data, "{sv}",
		                       "Method",
		                       g_variant_new_string ("direct"));
	}

	priv->domains = g_ptr_array_new_with_free_func (g_free);

	/* Extract stuff from configs */
	add_proxy_config (self, &proxy_data, proxy_config);

	if (ip4_config)
		add_ip4_config (self, &proxy_data, ip4_config);
	if (ip6_config)
		add_ip6_config (self, &proxy_data, ip6_config);

	g_ptr_array_add (priv->domains, NULL);
	strv = (char **) g_ptr_array_free (priv->domains, (priv->domains->len == 1));

	if (strv) {
		g_variant_builder_add (&proxy_data, "{sv}",
		                       "Domains",
		                       g_variant_new_strv ((const char *const *) strv, -1));
		g_strfreev (strv);
	}

	pacrunner_manager_args = g_variant_ref_sink (g_variant_new ("(a{sv})", &proxy_data));
	priv->args = g_list_append (priv->args, pacrunner_manager_args);

	/* Send if pacrunner is available on Bus, otherwise
	 * argument has already been appended above to be
	 * sent when pacrunner appears.
	 */
	send_pacrunner_proxy_data (self, pacrunner_manager_args);
}

static void
pacrunner_remove_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
	/* @self may be a dangling pointer. However, we don't use it as the
	 * logging macro below does not dereference @self. */
	NMPacrunnerManager *self = user_data;
	gs_free_error GError *error = NULL;
	gs_unref_variant GVariant *ret = NULL;

	ret = g_dbus_proxy_call_finish ((GDBusProxy *) source, res, &error);

	if (!ret)
		_LOGD ("Couldn't remove proxy config from pacrunner: %s", error->message);
	else
		_LOGD ("Successfully removed proxy config from pacrunner");
}

/**
 * nm_pacrunner_manager_remove:
 * @self: the #NMPacrunnerManager
 * @iface: the iface for the connection to be removed
 * from pacrunner
 */
void
nm_pacrunner_manager_remove (NMPacrunnerManager *self, const char *iface)
{
	NMPacrunnerManagerPrivate *priv = NM_PACRUNNER_MANAGER_GET_PRIVATE (self);
	GList *list;

	for (list = g_list_first(priv->remove); list; list = g_list_next(list)) {
		struct remove_data *data = list->data;
		if (g_strcmp0 (data->iface, iface) == 0) {
			if (priv->pacrunner && data->path)
				g_dbus_proxy_call (priv->pacrunner,
				                   "DestroyProxyConfiguration",
				                   g_variant_new ("(o)", data->path),
				                   G_DBUS_CALL_FLAGS_NONE,
				                   -1,
				                   NULL,
				                  (GAsyncReadyCallback) pacrunner_remove_done,
				                   self);
			break;
		}
	}
}

/*****************************************************************************/

static void
nm_pacrunner_manager_init (NMPacrunnerManager *self)
{
	NMPacrunnerManagerPrivate *priv = NM_PACRUNNER_MANAGER_GET_PRIVATE (self);

	priv->pacrunner_cancellable = g_cancellable_new ();

	g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
	                          G_DBUS_PROXY_FLAGS_NONE,
	                          NULL,
	                          PACRUNNER_DBUS_SERVICE,
	                          PACRUNNER_DBUS_PATH,
	                          PACRUNNER_DBUS_INTERFACE,
	                          priv->pacrunner_cancellable,
	                          (GAsyncReadyCallback) pacrunner_proxy_cb,
	                          self);
}

static void
dispose (GObject *object)
{
	NMPacrunnerManagerPrivate *priv = NM_PACRUNNER_MANAGER_GET_PRIVATE ((NMPacrunnerManager *) object);

	g_clear_pointer (&priv->iface, g_free);

	nm_clear_g_cancellable (&priv->pacrunner_cancellable);

	g_clear_object (&priv->pacrunner);

	g_list_free_full (priv->args, (GDestroyNotify) g_variant_unref);
	priv->args = NULL;

	g_list_free_full (priv->remove, (GDestroyNotify) remove_data_destroy);
	priv->remove = NULL;

	G_OBJECT_CLASS (nm_pacrunner_manager_parent_class)->dispose (object);
}

static void
nm_pacrunner_manager_class_init (NMPacrunnerManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = dispose;
}
