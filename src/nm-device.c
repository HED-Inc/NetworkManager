/* NetworkManager -- Network link manager
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * (C) Copyright 2005 Red Hat, Inc.
 */

#include <glib.h>
#include <glib/gi18n.h>
#include <dbus/dbus.h>
#include <netinet/in.h>
#include <string.h>

#include "nm-device.h"
#include "nm-device-interface.h"
#include "nm-device-private.h"
#include "nm-device-802-3-ethernet.h"
#include "nm-device-802-11-wireless.h"
#include "NetworkManagerDbus.h"
#include "NetworkManagerPolicy.h"
#include "NetworkManagerUtils.h"
#include "NetworkManagerSystem.h"
#include "nm-vpn-manager.h"
#include "nm-dhcp-manager.h"
#include "nm-dbus-manager.h"
#include "nm-dbus-nmi.h"
#include "nm-utils.h"
#include "autoip.h"

static void device_interface_init (NMDeviceInterface *device_interface_class);

G_DEFINE_TYPE_EXTENDED (NMDevice, nm_device, G_TYPE_OBJECT,
						G_TYPE_FLAG_ABSTRACT,
						G_IMPLEMENT_INTERFACE (NM_TYPE_DEVICE_INTERFACE,
											   device_interface_init))

#define NM_DEVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DEVICE, NMDevicePrivate))

struct _NMDevicePrivate
{
	gboolean	dispose_has_run;

	NMDeviceState state;

	char *			udi;
	char *			iface;
	NMDeviceType		type;
	guint32			capabilities;
	char *			driver;
	gboolean			removed;

	gboolean			link_active;
	guint32			ip4_address;
	struct in6_addr	ip6_address;
	NMData *			app_data;

	NMActRequest *		act_request;
	guint           act_source_id;

	/* IP configuration info */
	void *			system_config_data;	/* Distro-specific config data (parsed config file, etc) */
	NMIP4Config *		ip4_config;			/* Config from DHCP, PPP, or system config files */
	NMDHCPManager *     dhcp_manager;
	gulong              dhcp_signal_id;
};

static void		nm_device_activate_schedule_stage5_ip_config_commit (NMActRequest *req);
static void nm_device_deactivate (NMDeviceInterface *device);
void nm_device_bring_up (NMDevice *dev);
gboolean nm_device_bring_up_wait (NMDevice *self, gboolean cancelable);


static void
nm_device_set_address (NMDevice *device)
{
	if (NM_DEVICE_GET_CLASS (device)->set_hw_address)
		NM_DEVICE_GET_CLASS (device)->set_hw_address (device);
}

static void
device_interface_init (NMDeviceInterface *device_interface_class)
{
	/* interface implementation */
	device_interface_class->deactivate = nm_device_deactivate;
}


static void
nm_device_init (NMDevice * self)
{
	self->priv = NM_DEVICE_GET_PRIVATE (self);
	self->priv->dispose_has_run = FALSE;
	self->priv->udi = NULL;
	self->priv->iface = NULL;
	self->priv->type = DEVICE_TYPE_UNKNOWN;
	self->priv->capabilities = NM_DEVICE_CAP_NONE;
	self->priv->driver = NULL;
	self->priv->removed = FALSE;

	self->priv->link_active = FALSE;
	self->priv->ip4_address = 0;
	memset (&self->priv->ip6_address, 0, sizeof (struct in6_addr));
	self->priv->app_data = NULL;

	self->priv->act_source_id = 0;

	self->priv->system_config_data = NULL;
	self->priv->ip4_config = NULL;

	self->priv->state = NM_DEVICE_STATE_DISCONNECTED;
}


static GObject*
constructor (GType type,
			 guint n_construct_params,
			 GObjectConstructParam *construct_params)
{
	GObject *object;
	NMDevice *dev;
	NMDBusManager *manager;
	char *path;

	object = G_OBJECT_CLASS (nm_device_parent_class)->constructor (type,
																   n_construct_params,
																   construct_params);

	if (!object)
		return NULL;

	dev = NM_DEVICE (object);

	dev->priv->capabilities |= NM_DEVICE_GET_CLASS (dev)->get_generic_capabilities (dev);
	if (!(dev->priv->capabilities & NM_DEVICE_CAP_NM_SUPPORTED))
	{
		g_object_unref (G_OBJECT (dev));
		return NULL;
	}

	/* Have to bring the device up before checking link status and other stuff */
	nm_device_bring_up_wait (dev, FALSE);

	nm_device_update_ip4_address (dev);

	/* Update the device's hardware address */
	nm_device_set_address (dev);

	/* Grab IP config data for this device from the system configuration files */
	dev->priv->system_config_data = nm_system_device_get_system_config (dev, dev->priv->app_data);
	nm_device_set_use_dhcp (dev, nm_system_device_get_use_dhcp (dev));

	/* Allow distributions to flag devices as disabled */
	if (nm_system_device_get_disabled (dev))
	{
		g_object_unref (G_OBJECT (dev));
		return NULL;
	}

	nm_print_device_capabilities (dev);

	/* Call type-specific initialization */
	if (NM_DEVICE_GET_CLASS (dev)->init)
		NM_DEVICE_GET_CLASS (dev)->init (dev);

	NM_DEVICE_GET_CLASS (dev)->start (dev);

	manager = nm_dbus_manager_get ();

	path = nm_dbus_get_object_path_for_device (dev);
	dbus_g_connection_register_g_object (nm_dbus_manager_get_connection (manager),
										 path, object);
	g_free (path);

	return object;
}


static guint32
real_get_generic_capabilities (NMDevice *dev)
{
	return 0;
}


static void
real_start (NMDevice *dev)
{
}


void
nm_device_stop (NMDevice *self)
{
	g_return_if_fail (self != NULL);

	nm_device_interface_deactivate (NM_DEVICE_INTERFACE (self));
	nm_device_bring_down (self);
}


/*
 * nm_get_device_by_udi
 *
 * Search through the device list for a device with a given UDI.
 *
 */
NMDevice *
nm_get_device_by_udi (NMData *data,
                      const char *udi)
{
	GSList	*elt;
	
	g_return_val_if_fail (data != NULL, NULL);
	g_return_val_if_fail (udi  != NULL, NULL);

	for (elt = data->dev_list; elt; elt = g_slist_next (elt))
	{
		NMDevice	*dev = NULL;
		if ((dev = NM_DEVICE (elt->data)))
		{
			if (nm_null_safe_strcmp (nm_device_get_udi (dev), udi) == 0)
				return dev;
		}
	}

	return NULL;
}


/*
 * nm_get_device_by_iface
 *
 * Search through the device list for a device with a given iface.
 *
 */
NMDevice *
nm_get_device_by_iface (NMData *data,
                        const char *iface)
{
	GSList	*elt;
	
	g_return_val_if_fail (data  != NULL, NULL);
	g_return_val_if_fail (iface != NULL, NULL);

	for (elt = data->dev_list; elt; elt = g_slist_next (elt)) {
		NMDevice	*dev = NM_DEVICE (elt->data);

		g_assert (dev);
		if (nm_null_safe_strcmp (nm_device_get_iface (dev), iface) == 0)
			return dev;
	}
	return NULL;
}


/*
 * Get/set functions for UDI
 */
const char *
nm_device_get_udi (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return self->priv->udi;
}

/*
 * Get/set functions for iface
 */
const char *
nm_device_get_iface (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return self->priv->iface;
}


/*
 * Get/set functions for driver
 */
const char *
nm_device_get_driver (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return self->priv->driver;
}


/*
 * Get/set functions for type
 */
NMDeviceType
nm_device_get_device_type (NMDevice *self)
{
	g_return_val_if_fail (NM_IS_DEVICE (self), DEVICE_TYPE_UNKNOWN);

	return self->priv->type;
}


void
nm_device_set_device_type (NMDevice *dev, NMDeviceType type)
{
	g_return_if_fail (NM_IS_DEVICE (dev));
	g_return_if_fail (NM_DEVICE_GET_PRIVATE (dev)->type == DEVICE_TYPE_UNKNOWN);

	NM_DEVICE_GET_PRIVATE (dev)->type = type;
}


static gboolean
real_is_test_device (NMDevice *dev)
{
	return FALSE;
}

gboolean
nm_device_is_test_device (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, FALSE);

	return NM_DEVICE_GET_CLASS (self)->is_test_device (self);
}

/*
 * Accessor for capabilities
 */
guint32
nm_device_get_capabilities (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NM_DEVICE_CAP_NONE);

	return self->priv->capabilities;
}

/*
 * Accessor for type-specific capabilities
 */
guint32
nm_device_get_type_capabilities (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NM_DEVICE_CAP_NONE);

	return NM_DEVICE_GET_CLASS (self)->get_type_capabilities (self);
}

static guint32
real_get_type_capabilities (NMDevice *self)
{
	return NM_DEVICE_CAP_NONE;
}


/*
 * nm_device_get_app_data
 *
 */
struct NMData *
nm_device_get_app_data (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, FALSE);

	return self->priv->app_data;
}


/*
 * Get/Set for "removed" flag
 */
gboolean
nm_device_get_removed (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, TRUE);

	return self->priv->removed;
}

void
nm_device_set_removed (NMDevice *self,
                       const gboolean removed)
{
	g_return_if_fail (self != NULL);

	self->priv->removed = removed;
}


/*
 * nm_device_get_act_request
 *
 * Return the devices activation request, if any.
 *
 */
NMActRequest *
nm_device_get_act_request (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return self->priv->act_request;
}


/*
 * Get/set functions for link_active
 */
gboolean
nm_device_has_active_link (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, FALSE);

	return self->priv->link_active;
}

void
nm_device_set_active_link (NMDevice *self,
                           const gboolean link_active)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	if (priv->link_active != link_active) {
		priv->link_active = link_active;
		g_signal_emit_by_name (self, "carrier-changed", link_active);
	}
}


/*
 * nm_device_activation_start
 *
 * Tell the device to begin activation.
 */
void
nm_device_activate (NMDevice *device,
					NMActRequest *req)
{
	NMDevicePrivate *priv;
	NMData *data = NULL;

	g_return_if_fail (NM_IS_DEVICE (device));
	g_return_if_fail (req != NULL);

	priv = NM_DEVICE_GET_PRIVATE (device);

	if (priv->state != NM_DEVICE_STATE_DISCONNECTED)
		/* Already activating or activated */
		return;

	nm_info ("Activation (%s) started...", nm_device_get_iface (device));

	data = nm_act_request_get_data (req);
	g_assert (data);

	nm_act_request_ref (req);
	priv->act_request = req;

	nm_act_request_set_stage (req, NM_ACT_STAGE_DEVICE_PREPARE);
	nm_device_activate_schedule_stage1_device_prepare (req);

	nm_schedule_state_change_signal_broadcast (data);
	nm_dbus_schedule_device_status_change_signal (data, device, NULL, DEVICE_ACTIVATING);
}


/*
 * nm_device_activate_stage1_device_prepare
 *
 * Prepare for device activation
 *
 */
static gboolean
nm_device_activate_stage1_device_prepare (gpointer user_data)
{
	NMActRequest *   req = (NMActRequest *) user_data;
	NMDevice *       self;
	NMData *         data;
	const char *     iface;
	NMActStageReturn ret;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	/* Clear the activation source ID now that this stage has run */
	if (self->priv->act_source_id > 0)
		self->priv->act_source_id = 0;

	iface = nm_device_get_iface (self);
	nm_info ("Activation (%s) Stage 1 of 5 (Device Prepare) started...", iface);
	nm_device_state_changed (self, NM_DEVICE_STATE_PREPARE);

	ret = NM_DEVICE_GET_CLASS (self)->act_stage1_prepare (self, req);
	if (ret == NM_ACT_STAGE_RETURN_POSTPONE) {
		goto out;
	} else if (ret == NM_ACT_STAGE_RETURN_FAILURE) {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED);
		nm_policy_schedule_activation_failed (req);
		goto out;
	}
	g_assert (ret == NM_ACT_STAGE_RETURN_SUCCESS);

	nm_device_activate_schedule_stage2_device_config (req);

out:
	nm_info ("Activation (%s) Stage 1 of 5 (Device Prepare) complete.", iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage1_device_prepare
 *
 * Prepare a device for activation
 *
 */
void
nm_device_activate_schedule_stage1_device_prepare (NMActRequest *req)
{
	NMDevice * self = NULL;
	guint      id;

	g_return_if_fail (req != NULL);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	nm_act_request_set_stage (req, NM_ACT_STAGE_DEVICE_PREPARE);
	id = g_idle_add (nm_device_activate_stage1_device_prepare, req);
	self->priv->act_source_id = id;

	nm_info ("Activation (%s) Stage 1 of 5 (Device Prepare) scheduled...",
	         nm_device_get_iface (self));
}

static NMActStageReturn
real_act_stage1_prepare (NMDevice *dev, NMActRequest *req)
{
	/* Nothing to do */
	return NM_ACT_STAGE_RETURN_SUCCESS;
}

static NMActStageReturn
real_act_stage2_config (NMDevice *dev, NMActRequest *req)
{
	/* Nothing to do */
	return NM_ACT_STAGE_RETURN_SUCCESS;
}

/*
 * nm_device_activate_stage2_device_config
 *
 * Determine device parameters and set those on the device, ie
 * for wireless devices, set essid, keys, etc.
 *
 */
static gboolean
nm_device_activate_stage2_device_config (gpointer user_data)
{
	NMActRequest *   req = (NMActRequest *) user_data;
	NMDevice *       self;
	NMData *         data;
	const char *     iface;
	NMActStageReturn ret;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	/* Clear the activation source ID now that this stage has run */
	if (self->priv->act_source_id > 0)
		self->priv->act_source_id = 0;

	iface = nm_device_get_iface (self);
	nm_info ("Activation (%s) Stage 2 of 5 (Device Configure) starting...", iface);
	nm_device_state_changed (self, NM_DEVICE_STATE_CONFIG);

	/* Bring the device up */
	if (!nm_device_is_up (self))
		nm_device_bring_up (self);

	ret = NM_DEVICE_GET_CLASS (self)->act_stage2_config (self, req);
	if (ret == NM_ACT_STAGE_RETURN_POSTPONE)
		goto out;
	else if (ret == NM_ACT_STAGE_RETURN_FAILURE)
	{
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED);
		nm_policy_schedule_activation_failed (req);
		goto out;
	}
	g_assert (ret == NM_ACT_STAGE_RETURN_SUCCESS);	

	nm_info ("Activation (%s) Stage 2 of 5 (Device Configure) successful.", iface);

	nm_device_activate_schedule_stage3_ip_config_start (req);

out:
	nm_info ("Activation (%s) Stage 2 of 5 (Device Configure) complete.", iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage2_device_config
 *
 * Schedule setup of the hardware device
 *
 */
void
nm_device_activate_schedule_stage2_device_config (NMActRequest *req)
{
	NMDevice * self = NULL;
	guint      id;

	g_return_if_fail (req != NULL);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	nm_act_request_set_stage (req, NM_ACT_STAGE_DEVICE_CONFIG);
	id = g_idle_add (nm_device_activate_stage2_device_config, req);
	self->priv->act_source_id = id;

	nm_info ("Activation (%s) Stage 2 of 5 (Device Configure) scheduled...",
	         nm_device_get_iface (self));
}


static NMActStageReturn
real_act_stage3_ip_config_start (NMDevice *self,
                                 NMActRequest *req)
{	
	NMData *			data = NULL;
	NMActStageReturn	ret = NM_ACT_STAGE_RETURN_SUCCESS;

	data = nm_act_request_get_data (req);
	g_assert (data);

	/* DHCP devices try DHCP, non-DHCP default to SUCCESS */
	if (nm_device_get_use_dhcp (self))
	{
		/* Begin a DHCP transaction on the interface */
		NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
		gboolean success;

		/* DHCP manager will cancel any transaction already in progress and we do not
		   want to cancel this activation if we get "down" state from that. */
		g_signal_handler_block (priv->dhcp_manager, priv->dhcp_signal_id);

		success = nm_dhcp_manager_begin_transaction (priv->dhcp_manager,
													 nm_device_get_iface (self));

		g_signal_handler_unblock (priv->dhcp_manager, priv->dhcp_signal_id);

		if (success) {
			/* DHCP devices will be notified by the DHCP manager when
			 * stuff happens.	
			 */
			ret = NM_ACT_STAGE_RETURN_POSTPONE;
		} else
			ret = NM_ACT_STAGE_RETURN_FAILURE;
	}

	return ret;
}


/*
 * nm_device_activate_stage3_ip_config_start
 *
 * Begin IP configuration with either DHCP or static IP.
 *
 */
static gboolean
nm_device_activate_stage3_ip_config_start (gpointer user_data)
{
	NMActRequest *   req = (NMActRequest *) user_data;
	NMData *         data = NULL;
	NMDevice *       self = NULL;
	const char *     iface;
	NMActStageReturn ret;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	/* Clear the activation source ID now that this stage has run */
	if (self->priv->act_source_id > 0)
		self->priv->act_source_id = 0;

	iface = nm_device_get_iface (self);
	nm_info ("Activation (%s) Stage 3 of 5 (IP Configure Start) started...", iface);
	nm_device_state_changed (self, NM_DEVICE_STATE_IP_CONFIG);

	ret = NM_DEVICE_GET_CLASS (self)->act_stage3_ip_config_start (self, req);
	if (ret == NM_ACT_STAGE_RETURN_POSTPONE)
		goto out;
	else if (ret == NM_ACT_STAGE_RETURN_FAILURE)
	{
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED);
		nm_policy_schedule_activation_failed (req);
		goto out;
	}
	g_assert (ret == NM_ACT_STAGE_RETURN_SUCCESS);	

	nm_device_activate_schedule_stage4_ip_config_get (req);

out:
	nm_info ("Activation (%s) Stage 3 of 5 (IP Configure Start) complete.", iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage3_ip_config_start
 *
 * Schedule IP configuration start
 */
void
nm_device_activate_schedule_stage3_ip_config_start (NMActRequest *req)
{
	NMDevice * self = NULL;
	guint      id;

	g_return_if_fail (req != NULL);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	nm_act_request_set_stage (req, NM_ACT_STAGE_IP_CONFIG_START);
	id = g_idle_add (nm_device_activate_stage3_ip_config_start, req);
	self->priv->act_source_id = id;

	nm_info ("Activation (%s) Stage 3 of 5 (IP Configure Start) scheduled.",
	         nm_device_get_iface (self));
}


/*
 * nm_device_new_ip4_autoip_config
 *
 * Build up an IP config with a Link Local address
 *
 */
NMIP4Config *
nm_device_new_ip4_autoip_config (NMDevice *self)
{
	struct in_addr		ip;
	NMIP4Config *		config = NULL;

	g_return_val_if_fail (self != NULL, NULL);

	if (get_autoip (self, &ip))
	{
		#define LINKLOCAL_BCAST		0xa9feffff

		config = nm_ip4_config_new ();
		nm_ip4_config_set_address (config, (guint32)(ip.s_addr));
		nm_ip4_config_set_netmask (config, (guint32)(ntohl (0xFFFF0000)));
		nm_ip4_config_set_broadcast (config, (guint32)(ntohl (LINKLOCAL_BCAST)));
		nm_ip4_config_set_gateway (config, 0);
	}

	return config;
}


static NMActStageReturn
real_act_stage4_get_ip4_config (NMDevice *self,
                                NMActRequest *req,
                                NMIP4Config **config)
{
	NMData *			data;
	NMIP4Config *		real_config = NULL;
	NMActStageReturn	ret = NM_ACT_STAGE_RETURN_FAILURE;

	g_return_val_if_fail (config != NULL, NM_ACT_STAGE_RETURN_FAILURE);
	g_return_val_if_fail (*config == NULL, NM_ACT_STAGE_RETURN_FAILURE);

	g_assert (req);
	data = nm_act_request_get_data (req);
	g_assert (data);

	if (nm_device_get_use_dhcp (self)) {
		real_config = nm_dhcp_manager_get_ip4_config (NM_DEVICE_GET_PRIVATE (self)->dhcp_manager,
													  nm_device_get_iface (self));

		if (real_config && nm_ip4_config_get_mtu (real_config) == 0)
			/* If the DHCP server doesn't set the MTU, get it from backend. */
			nm_ip4_config_set_mtu (real_config, nm_system_get_mtu (self));
	} else
		real_config = nm_system_device_new_ip4_system_config (self);

	if (real_config)
	{
		*config = real_config;
		ret = NM_ACT_STAGE_RETURN_SUCCESS;
	}
	else
	{
		/* Make sure device is up even if config fails */
		if (!nm_device_is_up (self))
			nm_device_bring_up (self);
	}

	return ret;
}


/*
 * nm_device_activate_stage4_ip_config_get
 *
 * Retrieve the correct IP config.
 *
 */
static gboolean
nm_device_activate_stage4_ip_config_get (gpointer user_data)
{
	NMActRequest *   req = (NMActRequest *) user_data;
	NMData *         data = NULL;
	NMDevice *       self = NULL;
	NMIP4Config *    ip4_config = NULL;
	NMActStageReturn ret;
	const char *     iface = NULL;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	/* Clear the activation source ID now that this stage has run */
	if (self->priv->act_source_id > 0)
		self->priv->act_source_id = 0;

	iface = nm_device_get_iface (self);
	nm_info ("Activation (%s) Stage 4 of 5 (IP Configure Get) started...", iface);

	ret = NM_DEVICE_GET_CLASS (self)->act_stage4_get_ip4_config (self, req, &ip4_config);
	if (ret == NM_ACT_STAGE_RETURN_POSTPONE)
		goto out;
	else if (!ip4_config || (ret == NM_ACT_STAGE_RETURN_FAILURE))
	{
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED);
		nm_policy_schedule_activation_failed (req);
		goto out;
	}
	g_assert (ret == NM_ACT_STAGE_RETURN_SUCCESS);	

	nm_act_request_set_ip4_config (req, ip4_config);
	nm_device_activate_schedule_stage5_ip_config_commit (req);

out:
	nm_info ("Activation (%s) Stage 4 of 5 (IP Configure Get) complete.", iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage4_ip_config_get
 *
 * Schedule creation of the IP config
 *
 */
void
nm_device_activate_schedule_stage4_ip_config_get (NMActRequest *req)
{
	NMDevice * self = NULL;
	guint      id;

	g_return_if_fail (req != NULL);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	nm_act_request_set_stage (req, NM_ACT_STAGE_IP_CONFIG_GET);
	id = g_idle_add (nm_device_activate_stage4_ip_config_get, req);
	self->priv->act_source_id = id;

	nm_info ("Activation (%s) Stage 4 of 5 (IP Configure Get) scheduled...",
	         nm_device_get_iface (self));
}


static NMActStageReturn
real_act_stage4_ip_config_timeout (NMDevice *self,
                                   NMActRequest *req,
                                   NMIP4Config **config)
{
	g_return_val_if_fail (config != NULL, NM_ACT_STAGE_RETURN_FAILURE);
	g_return_val_if_fail (*config == NULL, NM_ACT_STAGE_RETURN_FAILURE);

	g_assert (req);

	/* Wired network, no DHCP reply.  Let's get an IP via Zeroconf. */
	nm_info ("No DHCP reply received.  Automatically obtaining IP via Zeroconf.");
	*config = nm_device_new_ip4_autoip_config (self);

	return NM_ACT_STAGE_RETURN_SUCCESS;
}


/*
 * nm_device_activate_stage4_ip_config_timeout
 *
 * Retrieve the correct IP config.
 *
 */
static gboolean
nm_device_activate_stage4_ip_config_timeout (gpointer user_data)
{
	NMActRequest *   req = (NMActRequest *) user_data;
	NMData *         data = NULL;
	NMDevice *       self = NULL;
	NMIP4Config *    ip4_config = NULL;
	const char *     iface;
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_FAILURE;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	/* Clear the activation source ID now that this stage has run */
	if (self->priv->act_source_id > 0)
		self->priv->act_source_id = 0;

	iface = nm_device_get_iface (self);
	nm_info ("Activation (%s) Stage 4 of 5 (IP Configure Timeout) started...", iface);

	ret = NM_DEVICE_GET_CLASS (self)->act_stage4_ip_config_timeout (self, req, &ip4_config);
	if (ret == NM_ACT_STAGE_RETURN_POSTPONE) {
		goto out;
	} else if (!ip4_config || (ret == NM_ACT_STAGE_RETURN_FAILURE)) {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED);
		nm_policy_schedule_activation_failed (req);
		goto out;
	}
	g_assert (ret == NM_ACT_STAGE_RETURN_SUCCESS);	
	g_assert (ip4_config);

	nm_act_request_set_ip4_config (req, ip4_config);
	nm_device_activate_schedule_stage5_ip_config_commit (req);

out:
	nm_info ("Activation (%s) Stage 4 of 5 (IP Configure Timeout) complete.", iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage4_ip_config_timeout
 *
 * Deal with a timed out DHCP transaction
 *
 */
void
nm_device_activate_schedule_stage4_ip_config_timeout (NMActRequest *req)
{
	NMDevice * self = NULL;
	guint      id;

	g_return_if_fail (req != NULL);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	nm_act_request_set_stage (req, NM_ACT_STAGE_IP_CONFIG_GET);
	id = g_idle_add (nm_device_activate_stage4_ip_config_timeout, req);
	self->priv->act_source_id = id;

	nm_info ("Activation (%s) Stage 4 of 5 (IP Configure Timeout) scheduled...",
	         nm_device_get_iface (self));
}


/*
 * nm_device_activate_stage5_ip_config_commit
 *
 * Commit the IP config on the device
 *
 */
static gboolean
nm_device_activate_stage5_ip_config_commit (gpointer user_data)
{
	NMActRequest * req = (NMActRequest *) user_data;
	NMData *       data = NULL;
	NMDevice *     self = NULL;
	NMIP4Config *  ip4_config = NULL;
	const char *   iface;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	ip4_config = nm_act_request_get_ip4_config (req);
	g_assert (ip4_config);

	/* Clear the activation source ID now that this stage has run */
	if (self->priv->act_source_id > 0)
		self->priv->act_source_id = 0;

	iface = nm_device_get_iface (self);
	nm_info ("Activation (%s) Stage 5 of 5 (IP Configure Commit) started...",
	         iface);

	nm_device_set_ip4_config (self, ip4_config);
	if (nm_system_device_set_from_ip4_config (self)) {
		nm_device_update_ip4_address (self);
		nm_system_device_add_ip6_link_address (self);
		nm_system_restart_mdns_responder ();
		nm_system_set_hostname (self->priv->ip4_config);
		nm_system_activate_nis (self->priv->ip4_config);
		nm_system_set_mtu (self);

		if (NM_DEVICE_GET_CLASS (self)->update_link)
			NM_DEVICE_GET_CLASS (self)->update_link (self);

		nm_device_state_changed (self, NM_DEVICE_STATE_ACTIVATED);
		nm_policy_schedule_activation_finish (req);
	} else {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED);
		nm_policy_schedule_activation_failed (req);
	}

	nm_info ("Activation (%s) Stage 5 of 5 (IP Configure Commit) complete.",
	         iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage5_ip_config_commit
 *
 * Schedule commit of the IP config
 */
static void
nm_device_activate_schedule_stage5_ip_config_commit (NMActRequest *req)
{
	NMDevice * self = NULL;
	guint      id;

	g_return_if_fail (req != NULL);

	self = nm_act_request_get_dev (req);
	g_assert (self);

	nm_act_request_set_stage (req, NM_ACT_STAGE_IP_CONFIG_COMMIT);
	id = g_idle_add (nm_device_activate_stage5_ip_config_commit, req);
	self->priv->act_source_id = id;

	nm_info ("Activation (%s) Stage 5 of 5 (IP Configure Commit) scheduled...",
	         nm_device_get_iface (self));
}


static void
real_activation_cancel_handler (NMDevice *self,
                                NMActRequest *req)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (req != NULL);

	if (nm_device_get_state (self) == NM_DEVICE_STATE_IP_CONFIG)
		nm_dhcp_manager_cancel_transaction (NM_DEVICE_GET_PRIVATE (self)->dhcp_manager,
											nm_device_get_iface (self),
											TRUE);
}


/*
 * nm_device_activation_cancel
 *
 * Signal activation worker that it should stop and die.
 *
 */
void
nm_device_activation_cancel (NMDevice *self)
{
	NMDeviceClass *klass;

	g_return_if_fail (self != NULL);

	if (!nm_device_is_activating (self))
		return;

	g_assert (self->priv->app_data);

	nm_info ("Activation (%s): cancelling...", nm_device_get_iface (self));

	/* Break the activation chain */
	if (self->priv->act_source_id) {
		g_source_remove (self->priv->act_source_id);
		self->priv->act_source_id = 0;
	}

	klass = NM_DEVICE_CLASS (g_type_class_peek (NM_TYPE_DEVICE));
	if (klass->activation_cancel_handler)
		klass->activation_cancel_handler (self, self->priv->act_request);

	nm_act_request_unref (self->priv->act_request);
	self->priv->act_request = NULL;

	nm_schedule_state_change_signal_broadcast (self->priv->app_data);
	nm_info ("Activation (%s): cancelled.", nm_device_get_iface (self));
}


/*
 * nm_device_deactivate_quickly
 *
 * Quickly deactivate a device, for things like sleep, etc.  Doesn't
 * clean much stuff up, and nm_device_deactivate() should be called
 * on the device eventually.
 *
 */
gboolean
nm_device_deactivate_quickly (NMDevice *self)
{
	NMData *		app_data;
	NMActRequest *	act_request;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (self->priv->app_data != NULL, FALSE);

	nm_system_shutdown_nis ();

	app_data = self->priv->app_data;
	nm_vpn_manager_deactivate_vpn_connection (app_data->vpn_manager, self);

	if (nm_device_get_state (self) == NM_DEVICE_STATE_ACTIVATED)
		nm_dbus_schedule_device_status_change_signal (app_data, self, NULL, DEVICE_NO_LONGER_ACTIVE);
	else if (nm_device_is_activating (self))
		nm_device_activation_cancel (self);

	/* Tear down an existing activation request, which may not have happened
	 * in nm_device_activation_cancel() above, for various reasons.
	 */
	if ((act_request = nm_device_get_act_request (self)))
	{
		nm_dhcp_manager_cancel_transaction (NM_DEVICE_GET_PRIVATE (self)->dhcp_manager,
											nm_device_get_iface (self),
											FALSE);
		nm_act_request_unref (act_request);
		self->priv->act_request = NULL;
	}

	/* Call device type-specific deactivation */
	if (NM_DEVICE_GET_CLASS (self)->deactivate_quickly)
		NM_DEVICE_GET_CLASS (self)->deactivate_quickly (self);

	return TRUE;
}

/*
 * nm_device_deactivate
 *
 * Remove a device's routing table entries and IP address.
 *
 */
static void
nm_device_deactivate (NMDeviceInterface *device)
{
	NMDevice *self = NM_DEVICE (device);
	NMData *		app_data;
	NMIP4Config *	config;

	g_return_if_fail (self != NULL);
	g_return_if_fail (self->priv->app_data != NULL);

	nm_info ("Deactivating device %s.", nm_device_get_iface (self));

	nm_device_deactivate_quickly (self);

	app_data = self->priv->app_data;

	/* Remove any device nameservers and domains */
	if ((config = nm_device_get_ip4_config (self)))
	{
		nm_named_manager_remove_ip4_config (app_data->named_manager, config);
		nm_device_set_ip4_config (self, NULL);
	}

	/* Take out any entries in the routing table and any IP address the device had. */
	nm_system_device_flush_routes (self);
	nm_system_device_flush_addresses (self);
	nm_device_update_ip4_address (self);	

	/* Call device type-specific deactivation */
	if (NM_DEVICE_GET_CLASS (self)->deactivate)
		NM_DEVICE_GET_CLASS (self)->deactivate (self);

	nm_device_state_changed (self, NM_DEVICE_STATE_DISCONNECTED);
	nm_schedule_state_change_signal_broadcast (self->priv->app_data);
}


/*
 * nm_device_is_activating
 *
 * Return whether or not the device is currently activating itself.
 *
 */
gboolean
nm_device_is_activating (NMDevice *device)
{
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	switch (nm_device_get_state (device)) {
	case NM_DEVICE_STATE_PREPARE:
	case NM_DEVICE_STATE_CONFIG:
	case NM_DEVICE_STATE_NEED_AUTH:
	case NM_DEVICE_STATE_IP_CONFIG:
		return TRUE;
		break;
	default:
		break;
	}

	return FALSE;
}


/*
 * nm_device_is_activated
 *
 * Return whether or not the device is successfully activated.
 *
 */
gboolean
nm_device_is_activated (NMDevice *dev)
{
	NMActRequest *	req;
	NMActStage	stage;
	gboolean		activated = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);

	if (!(req = nm_device_get_act_request (dev)))
		return FALSE;

	stage = nm_act_request_get_stage (req);
	switch (stage)
	{
		case NM_ACT_STAGE_ACTIVATED:
			activated = TRUE;
			break;

		case NM_ACT_STAGE_DEVICE_PREPARE:
		case NM_ACT_STAGE_DEVICE_CONFIG:
		case NM_ACT_STAGE_NEED_USER_KEY:
		case NM_ACT_STAGE_IP_CONFIG_START:
		case NM_ACT_STAGE_IP_CONFIG_GET:
		case NM_ACT_STAGE_IP_CONFIG_COMMIT:
		case NM_ACT_STAGE_FAILED:
		case NM_ACT_STAGE_CANCELLED:
		case NM_ACT_STAGE_UNKNOWN:
		default:
			break;
	}

	return activated;
}


gboolean
nm_device_can_interrupt_activation (NMDevice *self)
{
	gboolean	interrupt = FALSE;

	g_return_val_if_fail (self != NULL, FALSE);

	if (NM_DEVICE_GET_CLASS (self)->can_interrupt_activation)
		interrupt = NM_DEVICE_GET_CLASS (self)->can_interrupt_activation (self);
	return interrupt;
}

/* IP Configuration stuff */

static void
dhcp_state_changed (NMDHCPManager *dhcp_manager,
					const char *iface,
					NMDHCPState state,
					gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);
	NMActRequest *req;

	req = nm_device_get_act_request (device);
	if (!req)
		return;

	if (!strcmp (nm_device_get_iface (device), iface) &&
		nm_act_request_get_stage (req) == NM_ACT_STAGE_IP_CONFIG_START) {
		switch (state) {
		case DHCDBD_BOUND:	/* lease obtained */
		case DHCDBD_RENEW:	/* lease renewed */
		case DHCDBD_REBOOT:	/* have valid lease, but now obtained a different one */
		case DHCDBD_REBIND:	/* new, different lease */
			nm_device_activate_schedule_stage4_ip_config_get (req);
			break;
		case DHCDBD_TIMEOUT: /* timed out contacting DHCP server */
			nm_device_activate_schedule_stage4_ip_config_timeout (req);
			break;
		case DHCDBD_FAIL: /* all attempts to contact server timed out, sleeping */
		case DHCDBD_ABEND: /* dhclient exited abnormally */
		case DHCDBD_END: /* dhclient exited normally */
			nm_policy_schedule_activation_failed (req);
			break;
		default:
			break;
		}
	}
}

gboolean
nm_device_get_use_dhcp (NMDevice *self)
{
	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);

	return NM_DEVICE_GET_PRIVATE (self)->dhcp_manager ? TRUE : FALSE;
}

void
nm_device_set_use_dhcp (NMDevice *self,
                        gboolean use_dhcp)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);

	if (use_dhcp) {
		if (!priv->dhcp_manager) {
			priv->dhcp_manager = nm_dhcp_manager_get ();
			priv->dhcp_signal_id = g_signal_connect (priv->dhcp_manager, "state-changed",
													 G_CALLBACK (dhcp_state_changed),
													 self);
		}
	} else if (priv->dhcp_manager) {
		g_signal_handler_disconnect (priv->dhcp_manager, priv->dhcp_signal_id);
		g_object_unref (priv->dhcp_manager);
		priv->dhcp_manager = NULL;
	}
}


NMIP4Config *
nm_device_get_ip4_config (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return self->priv->ip4_config;
}


void
nm_device_set_ip4_config (NMDevice *self, NMIP4Config *config)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	g_return_if_fail (NM_IS_DEVICE (self));

	if (priv->ip4_config) {
		g_object_unref (priv->ip4_config);
		priv->ip4_config = NULL;
	}

	if (config)
		priv->ip4_config = g_object_ref (config);
}


/*
 * nm_device_get_ip4_address
 *
 * Get a device's IPv4 address
 *
 */
guint32
nm_device_get_ip4_address (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, 0);

	return self->priv->ip4_address;
}


void
nm_device_update_ip4_address (NMDevice *self)
{
	guint32		new_address;
	struct ifreq	req;
	NMSock *		sk;
	int			err;
	const char *	iface;
	
	g_return_if_fail (self  != NULL);
	g_return_if_fail (self->priv->app_data != NULL);
	g_return_if_fail (nm_device_get_iface (self) != NULL);

	if ((sk = nm_dev_sock_open (self, DEV_GENERAL, __func__, NULL)) == NULL)
		return;

	iface = nm_device_get_iface (self);
	memset (&req, 0, sizeof (struct ifreq));
	strncpy (req.ifr_name, iface, sizeof (req.ifr_name) - 1);

	nm_ioctl_info ("%s: About to GET IFADDR.", iface);
	err = ioctl (nm_dev_sock_get_fd (sk), SIOCGIFADDR, &req);
	nm_ioctl_info ("%s: Done with GET IFADDR.", iface);

	nm_dev_sock_close (sk);
	if (err != 0)
		return;

	new_address = ((struct sockaddr_in *)(&req.ifr_addr))->sin_addr.s_addr;
	if (new_address != nm_device_get_ip4_address (self))
		self->priv->ip4_address = new_address;
}


/*
 * nm_device_set_up_down
 *
 * Set the up flag on the device on or off
 *
 */
static void
nm_device_set_up_down (NMDevice *self,
                       gboolean up)
{
	g_return_if_fail (self != NULL);

	nm_system_device_set_up_down (self, up);

	/*
	 * Make sure that we have a valid MAC address, some cards reload firmware when they
	 * are brought up.
	 */
	nm_device_set_address (self);
}


/*
 * Interface state functions: bring up, down, check
 *
 */
gboolean
nm_device_is_up (NMDevice *self)
{
	NMSock *		sk;
	struct ifreq	ifr;
	int			err;

	g_return_val_if_fail (self != NULL, FALSE);

	if ((sk = nm_dev_sock_open (self, DEV_GENERAL, __FUNCTION__, NULL)) == NULL)
		return (FALSE);

	/* Get device's flags */
	strncpy (ifr.ifr_name, nm_device_get_iface (self), sizeof (ifr.ifr_name) - 1);

	nm_ioctl_info ("%s: About to GET IFFLAGS.", nm_device_get_iface (self));
	err = ioctl (nm_dev_sock_get_fd (sk), SIOCGIFFLAGS, &ifr);
	nm_ioctl_info ("%s: Done with GET IFFLAGS.", nm_device_get_iface (self));

	nm_dev_sock_close (sk);
	if (!err)
		return (!((ifr.ifr_flags^IFF_UP) & IFF_UP));

	if (errno != ENODEV)
	{
		nm_warning ("nm_device_is_up() could not get flags for device %s.  errno = %d",
				nm_device_get_iface (self), errno );
	}

	return FALSE;
}

/* I really wish nm_v_wait_for_completion_or_timeout could translate these
 * to first class args instead of a all this void * arg stuff, so these
 * helpers could be nice and _tiny_. */
static gboolean
nm_completion_device_is_up_test (int tries,
                                 nm_completion_args args)
{
	NMDevice *self = NM_DEVICE (args[0]);
	gboolean *err = args[1];
	gboolean cancelable = GPOINTER_TO_INT (args[2]);

	g_return_val_if_fail (self != NULL, TRUE);
	g_return_val_if_fail (err != NULL, TRUE);

	*err = FALSE;
	if (cancelable /* && nm_device_activation_should_cancel (self) */) {
		*err = TRUE;
		return TRUE;
	}
	if (nm_device_is_up (self))
		return TRUE;
	return FALSE;
}

void
nm_device_bring_up (NMDevice *self)
{
	g_return_if_fail (self != NULL);

	nm_device_set_up_down (self, TRUE);
}

gboolean
nm_device_bring_up_wait (NMDevice *self,
                         gboolean cancelable)
{
	gboolean err = FALSE;
	nm_completion_args args;

	g_return_val_if_fail (self != NULL, TRUE);

	nm_device_bring_up (self);

	args[0] = self;
	args[1] = &err;
	args[2] = GINT_TO_POINTER (cancelable);
	nm_wait_for_completion (400, G_USEC_PER_SEC / 200, NULL, nm_completion_device_is_up_test, args);
	if (err)
		nm_info ("failed to bring up device %s", self->priv->iface);
	return err;
}

void
nm_device_bring_down (NMDevice *self)
{
	g_return_if_fail (self != NULL);

	nm_device_set_up_down (self, FALSE);
}

/*
 * nm_device_get_system_config_data
 *
 * Return distro-specific system configuration data for this device.
 *
 */
void *
nm_device_get_system_config_data (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return self->priv->system_config_data;
}


static void
nm_device_dispose (GObject *object)
{
	NMDevice *self = NM_DEVICE (object);

	if (self->priv->dispose_has_run)
		/* If dispose did already run, return. */
		return;

	/* Make sure dispose does not run twice. */
	self->priv->dispose_has_run = TRUE;

	/* 
	 * In dispose, you are supposed to free all types referenced from this
	 * object which might themselves hold a reference to self. Generally,
	 * the most simple solution is to unref all members on which you own a 
	 * reference.
	 */

	nm_system_device_free_system_config (self, self->priv->system_config_data);
	nm_device_set_ip4_config (self, NULL);

	if (self->priv->act_request)
	{
		nm_act_request_unref (self->priv->act_request);
		self->priv->act_request = NULL;
	}

	if (self->priv->act_source_id) {
		g_source_remove (self->priv->act_source_id);
		self->priv->act_source_id = 0;
	}

	nm_device_set_use_dhcp (self, FALSE);

	G_OBJECT_CLASS (nm_device_parent_class)->dispose (object);
}

static void
nm_device_finalize (GObject *object)
{
	NMDevice *self = NM_DEVICE (object);

	g_free (self->priv->udi);
	g_free (self->priv->iface);
	g_free (self->priv->driver);

	G_OBJECT_CLASS (nm_device_parent_class)->finalize (object);
}


static void
set_property (GObject *object, guint prop_id,
			  const GValue *value, GParamSpec *pspec)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (object);
 
	switch (prop_id) {
	case NM_DEVICE_INTERFACE_PROP_UDI:
		/* construct-only */
		priv->udi = g_strdup (g_value_get_string (value));
		break;
	case NM_DEVICE_INTERFACE_PROP_IFACE:
		priv->iface = g_strdup (g_value_get_string (value));
		break;
	case NM_DEVICE_INTERFACE_PROP_DRIVER:
		priv->driver = g_strdup (g_value_get_string (value));
		break;
	case NM_DEVICE_INTERFACE_PROP_APP_DATA:
		priv->app_data = g_value_get_pointer (value);
		break;
	case NM_DEVICE_INTERFACE_PROP_CAPABILITIES:
		priv->capabilities = g_value_get_uint (value);
		break;
	case NM_DEVICE_INTERFACE_PROP_IP4_ADDRESS:
		priv->ip4_address = g_value_get_uint (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (object);

	switch (prop_id) {
	case NM_DEVICE_INTERFACE_PROP_UDI:
		g_value_set_string (value, priv->udi);
		break;
	case NM_DEVICE_INTERFACE_PROP_IFACE:
		g_value_set_string (value, priv->iface);
		break;
	case NM_DEVICE_INTERFACE_PROP_DRIVER:
		g_value_set_string (value, priv->driver);
		break;
	case NM_DEVICE_INTERFACE_PROP_APP_DATA:
		g_value_set_pointer (value, priv->app_data);
		break;
	case NM_DEVICE_INTERFACE_PROP_CAPABILITIES:
		g_value_set_uint (value, priv->capabilities);
		break;
	case NM_DEVICE_INTERFACE_PROP_IP4_ADDRESS:
		g_value_set_uint (value, priv->ip4_address);
		break;
	case NM_DEVICE_INTERFACE_PROP_IP4_CONFIG:
		g_value_set_object (value, priv->ip4_config);
		break;
	case NM_DEVICE_INTERFACE_PROP_STATE:
		g_value_set_uint (value, priv->state);
		break;
	case NM_DEVICE_INTERFACE_PROP_DEVICE_TYPE:
		g_value_set_uint (value, priv->type);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}


static void
nm_device_class_init (NMDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMDevicePrivate));

	/* Virtual methods */
	object_class->dispose = nm_device_dispose;
	object_class->finalize = nm_device_finalize;
	object_class->set_property = set_property;
	object_class->get_property = get_property;
	object_class->constructor = constructor;

	klass->is_test_device = real_is_test_device;
	klass->activation_cancel_handler = real_activation_cancel_handler;
	klass->get_type_capabilities = real_get_type_capabilities;
	klass->get_generic_capabilities = real_get_generic_capabilities;
	klass->start = real_start;
	klass->act_stage1_prepare = real_act_stage1_prepare;
	klass->act_stage2_config = real_act_stage2_config;
	klass->act_stage3_ip_config_start = real_act_stage3_ip_config_start;
	klass->act_stage4_get_ip4_config = real_act_stage4_get_ip4_config;
	klass->act_stage4_ip_config_timeout = real_act_stage4_ip_config_timeout;

	/* Properties */

	g_object_class_override_property (object_class,
									  NM_DEVICE_INTERFACE_PROP_UDI,
									  NM_DEVICE_INTERFACE_UDI);

	g_object_class_override_property (object_class,
									  NM_DEVICE_INTERFACE_PROP_IFACE,
									  NM_DEVICE_INTERFACE_IFACE);

	g_object_class_override_property (object_class,
									  NM_DEVICE_INTERFACE_PROP_DRIVER,
									  NM_DEVICE_INTERFACE_DRIVER);

	g_object_class_override_property (object_class,
									  NM_DEVICE_INTERFACE_PROP_CAPABILITIES,
									  NM_DEVICE_INTERFACE_CAPABILITIES);

	g_object_class_override_property (object_class,
									  NM_DEVICE_INTERFACE_PROP_IP4_ADDRESS,
									  NM_DEVICE_INTERFACE_IP4_ADDRESS);

	g_object_class_override_property (object_class,
									  NM_DEVICE_INTERFACE_PROP_IP4_CONFIG,
									  NM_DEVICE_INTERFACE_IP4_CONFIG);

	g_object_class_override_property (object_class,
									  NM_DEVICE_INTERFACE_PROP_STATE,
									  NM_DEVICE_INTERFACE_STATE);

	g_object_class_override_property (object_class,
									  NM_DEVICE_INTERFACE_PROP_APP_DATA,
									  NM_DEVICE_INTERFACE_APP_DATA);

	g_object_class_override_property (object_class,
									  NM_DEVICE_INTERFACE_PROP_DEVICE_TYPE,
									  NM_DEVICE_INTERFACE_DEVICE_TYPE);
}

void
nm_device_state_changed (NMDevice *device, NMDeviceState state)
{
	g_return_if_fail (NM_IS_DEVICE (device));

	device->priv->state = state;

	switch (state) {
	case NM_DEVICE_STATE_ACTIVATED:
		nm_info ("Activation (%s) successful, device activated.", nm_device_get_iface (device));
		break;
	case NM_DEVICE_STATE_FAILED:
		nm_info ("Activation (%s) failed.", nm_device_get_iface (device));
		nm_device_interface_deactivate (NM_DEVICE_INTERFACE (device));
		break;
	default:
		break;
	}

	g_signal_emit_by_name (device, "state-changed", state);
}


NMDeviceState
nm_device_get_state (NMDevice *device)
{
	g_return_val_if_fail (NM_IS_DEVICE (device), NM_DEVICE_STATE_UNKNOWN);

	return NM_DEVICE_GET_PRIVATE (device)->state;
}
