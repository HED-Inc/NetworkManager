/* NetworkManager -- Network link manager
 *
 * Matthew Garrett <mjg59@srcf.ucam.org>
 *
 * Heavily based on NetworkManagerRedhat.c by Dan Williams <dcbw@redhat.com>
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
 * (C) Copyright 2004 Tom Parker
 * (C) Copyright 2004 Matthew Garrett
 * (C) Copyright 2004 Red Hat, Inc.
 */

#include <stdio.h>
#include <sys/types.h>
#include <signal.h>
#include <arpa/inet.h>
#include "NetworkManagerSystem.h"
#include "NetworkManagerUtils.h"
#include "NetworkManagerDevice.h"
#include "interface_parser.h"
#include "nm-utils.h"

#define ARPING "/usr/sbin/arping"

/*
 * nm_system_init
 *
 * Initializes the distribution-specific system backend
 *
 */
void nm_system_init (void)
{
}

/*
 * nm_system_device_add_default_route_via_device
 *
 * Add default route to the given device
 *
 */
void nm_system_device_add_default_route_via_device (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	/* Not really applicable for test devices */
	if (nm_device_is_test_device (dev))
		return;

	nm_system_device_add_default_route_via_device_with_iface (nm_device_get_iface (dev));
}


/*
 * nm_system_device_add_default_route_via_device_with_iface
 *
 * Add default route to the given device
 *
 */
void nm_system_device_add_default_route_via_device_with_iface (const char *iface)
{
	char	*buf;

	g_return_if_fail (iface != NULL);

	/* Add default gateway */
	buf = g_strdup_printf ("/sbin/ip route add default dev %s", iface);
	nm_spawn_process (buf);
	g_free (buf);
}

/*
 * nm_system_device_add_route_via_device_with_iface
 *
 * Add route to the given device
 *
 */
void nm_system_device_add_route_via_device_with_iface (const char *iface, const char *route)
{
	char	*buf;

	g_return_if_fail (iface != NULL);

	/* Add default gateway */
	buf = g_strdup_printf ("/sbin/ip route add %s dev %s", route, iface);
	nm_spawn_process (buf);
	g_free (buf);
}


/*
 * nm_system_device_flush_addresses
 *
 * Flush all network addresses associated with a network device
 *
 */
void nm_system_device_flush_routes (NMDevice *dev)
{
	char	*buf;

	g_return_if_fail (dev != NULL);

	/* Not really applicable for test devices */
	if (nm_device_is_test_device (dev))
		return;

	nm_system_device_flush_routes_with_iface (nm_device_get_iface (dev));
}

/*
 * nm_system_device_flush_routes_with_iface
 *
 * Flush all routes associated with a network device
 *
 */
void nm_system_device_flush_routes_with_iface (const char *iface)
{
	char	*buf;

	g_return_if_fail (iface != NULL);

	/* Remove routing table entries */
	buf = g_strdup_printf ("/sbin/ip route flush dev %s", iface);
	nm_spawn_process (buf);
	g_free (buf);
}

/*
 * nm_system_device_flush_addresses
 *
 * Flush all network addresses associated with a network device
 *
 */
void nm_system_device_flush_addresses (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	/* Not really applicable for test devices */
	if (nm_device_is_test_device (dev))
		return;

	nm_system_device_flush_addresses_with_iface (nm_device_get_iface (dev));
}


/*
 * nm_system_device_flush_addresses_with_iface
 *
 * Flush all network addresses associated with a network device
 *
 */
void nm_system_device_flush_addresses_with_iface (const char *iface)
{
	char	*buf;

	g_return_if_fail (iface != NULL);

	/* Remove all IP addresses for a device */
	buf = g_strdup_printf ("/sbin/ip address flush dev %s", iface);
	nm_spawn_process (buf);
	g_free (buf);
}

/*
 * nm_system_device_setup_static_ip4_config
 *
 * Set up the device with a particular IPv4 address/netmask/gateway.
 *
 * Returns:	TRUE	on success
 *			FALSE on error
 *
 */
#if 0
gboolean nm_system_device_setup_static_ip4_config (NMDevice *dev)
{
#define IPBITS (sizeof (guint32) * 8)
        struct in_addr  temp_addr;
        struct in_addr  temp_addr2;
        char            *s_tmp;
        char            *s_tmp2;
        int             i;
        guint32         addr;
        guint32         netmask;
        guint32         prefix = IPBITS;    /* initialize with # bits in ipv4 address */
        guint32         broadcast;
        char            *buf;
        int             err;
        const char            *iface;

        g_return_val_if_fail (dev != NULL, FALSE);
        g_return_val_if_fail (!nm_device_config_get_use_dhcp (dev), FALSE);

        addr = nm_device_config_get_ip4_address (dev);
        netmask = nm_device_config_get_ip4_netmask (dev);
        iface = nm_device_get_iface (dev);
        broadcast = nm_device_config_get_ip4_broadcast (dev);

        /* get the prefix from the netmask */
        for (i = 0; i < IPBITS; i++)
        {
                if (!(ntohl (netmask) & ((2 << i) - 1)))
                       prefix--;
        }

        /* Calculate the broadcast address if the user didn't specify one */
        if (!broadcast)
                broadcast = ((addr & (int)netmask) | ~(int)netmask);

        /* 
         * Try and work out if someone else has our IP
         * using RFC 2131 Duplicate Address Detection
         */
        temp_addr.s_addr = addr;
        buf = g_strdup_printf ("%s -q -D -c 1 -I %s %s",ARPING, 
                               iface, inet_ntoa (temp_addr));
        if ((err = nm_spawn_process (buf)))
        {
            nm_warning ("Error: Duplicate address '%s' detected for " 
                             "device '%s' \n", iface, inet_ntoa (temp_addr));
            goto error;
        }
        g_free (buf);

        /* set our IP address */
        temp_addr.s_addr = addr;
        temp_addr2.s_addr = broadcast;
        s_tmp = g_strdup (inet_ntoa (temp_addr));
        s_tmp2 = g_strdup (inet_ntoa (temp_addr2));
        buf = g_strdup_printf ("/sbin/ip addr add %s/%d brd %s dev %s label %s",
                               s_tmp, prefix, s_tmp2, iface, iface);
        g_free (s_tmp);
        g_free (s_tmp2);
        if ((err = nm_spawn_process (buf)))
        {
            nm_warning ("Error: could not set network configuration for "
                             "device '%s' using command:\n      '%s'",
                             iface, buf);
            goto error;
        }
        g_free (buf);

        /* Alert other computers of our new address */
        temp_addr.s_addr = addr;
        buf = g_strdup_printf ("%s -q -A -c 1 -I %s %s", ARPING,iface,
                               inet_ntoa (temp_addr));
        nm_spawn_process (buf);
        g_free (buf);
        g_usleep (G_USEC_PER_SEC * 2);
        buf = g_strdup_printf ("%s -q -U -c 1 -I %s %s", ARPING, iface,
                                inet_ntoa (temp_addr));
        nm_spawn_process (buf);
        g_free (buf);

        /* set the default route to be this device's gateway */
        temp_addr.s_addr = nm_device_config_get_ip4_gateway (dev);
        buf = g_strdup_printf ("/sbin/ip route replace default via %s dev %s",
                               inet_ntoa (temp_addr), iface);
        if ((err = nm_spawn_process (buf)))
        {
                nm_warning ("Error: could not set default route using "
                                 "command:\n    '%s'", buf);
                goto error;
        }
        g_free (buf);
        return (TRUE);
        
error:
        g_free (buf);
        nm_system_device_flush_addresses (dev);
        nm_system_device_flush_routes (dev);
        return (FALSE);
}
#endif

/*
 * nm_system_enable_loopback
 *
 * Bring up the loopback interface
 *
 */
void nm_system_enable_loopback (void)
{
	nm_spawn_process ("/sbin/ifup lo");
}


/*
 * nm_system_flush_loopback_routes
 *
 * Flush all routes associated with the loopback device, because it
 * sometimes gets the first route for ZeroConf/Link-Local traffic.
 *
 */
void nm_system_flush_loopback_routes (void)
{
	nm_spawn_process ("/sbin/ip route flush dev lo");
}


/*
 * nm_system_delete_default_route
 *
 * Remove the old default route in preparation for a new one
 *
 */
void nm_system_delete_default_route (void)
{
	nm_spawn_process ("/sbin/ip route del default");
}


/*
 * nm_system_flush_arp_cache
 *
 * Flush all entries in the arp cache.
 *
 */
void nm_system_flush_arp_cache (void)
{
	nm_spawn_process ("/sbin/ip neigh flush all");
}


/*
 * nm_system_kill_all_dhcp_daemons
 *
 * Kill all DHCP daemons currently running, done at startup.
 *
 */
void nm_system_kill_all_dhcp_daemons (void)
{
	nm_spawn_process ("/usr/bin/killall -q dhclient");
}


/*
 * nm_system_update_dns
 *
 * Make glibc/nscd aware of any changes to the resolv.conf file by
 * restarting nscd.
 *
 */
void nm_system_update_dns (void)
{
	nm_spawn_process ("/usr/sbin/invoke-rc.d nscd restart");

}


/*
 * nm_system_load_device_modules
 *
 * This is a null op - all our drivers should already be loaded.
 *
 */
void nm_system_load_device_modules (void)
{
	return;
}


/*
 * nm_system_restart_mdns_responder
 *
 * Restart the multicast DNS responder so that it knows about new
 * network interfaces and IP addresses.
 *
 */
void nm_system_restart_mdns_responder (void)
{
	nm_spawn_process ("/usr/bin/killall -q -USR1 mDNSResponder");
}


/*
 * nm_system_device_add_ip6_link_address
 *
 * Add a default link-local IPv6 address to a device.
 *
 */
void nm_system_device_add_ip6_link_address (NMDevice *dev)
{
  char *buf;
  char *addr;
  struct ether_addr hw_addr;
  unsigned char eui[8];

  nm_device_get_hw_address(dev, &hw_addr);

  memcpy (eui, &(hw_addr.ether_addr_octet), sizeof (hw_addr.ether_addr_octet));
  memmove(eui+5, eui+3, 3);
  eui[3] = 0xff;
  eui[4] = 0xfe;
  eui[0] ^= 2;

  /* Add the default link-local IPv6 address to a device */
  buf = g_strdup_printf ("/sbin/ip -6 address add fe80::%x%02x:%x%02x:%x%02x:%x%02x/64 dev %s",
			 eui[0], eui[1], eui[2], eui[3],
			 eui[4], eui[5],
			 eui[6], eui[7], nm_device_get_iface (dev));
  nm_spawn_process (buf);
  g_free (buf);
}

typedef struct DebSystemConfigData
{
	NMIP4Config *	config;
	gboolean		use_dhcp;
} DebSystemConfigData;

/*
 * set_ip4_config_from_resolv_conf
 *
 * Add nameservers and search names from a resolv.conf format file.
 *
 */
static void set_ip4_config_from_resolv_conf (const char *filename, NMIP4Config *ip4_config)
{
	char *	contents = NULL;
	char **	split_contents = NULL;
	int		i, len;

	g_return_if_fail (filename != NULL);
	g_return_if_fail (ip4_config != NULL);

	if (!g_file_get_contents (filename, &contents, NULL, NULL) || (contents == NULL))
		return;

	if (!(split_contents = g_strsplit (contents, "\n", 0)))
		goto out;
	
	len = g_strv_length (split_contents);
	for (i = 0; i < len; i++)
	{
		char *line = split_contents[i];

		/* Ignore comments */
		if (!line || (line[0] == ';'))
			continue;

		line = g_strstrip (line);
		if ((strncmp (line, "search", 6) == 0) && (strlen (line) > 6))
		{
			char *searches = g_strdup (line + 7);
			char **split_searches = NULL;

			if (!searches || !strlen (searches))
				continue;

			/* Allow space-separated search domains */
			if (split_searches == g_strsplit (searches, " ", 0))
			{
				int m, srch_len;

				srch_len = g_strv_length (split_searches);
				for (m = 0; m < srch_len; m++)
				{
					if (split_searches[m])
						nm_ip4_config_add_domain	(ip4_config, split_searches[m]);
				}
				g_strfreev (split_searches);
			}
			else
			{
				/* Only 1 item, add the whole line */
				nm_ip4_config_add_domain	(ip4_config, searches);
			}

			g_free (searches);
		}
		else if ((strncmp (line, "nameserver", 10) == 0) && (strlen (line) > 10))
		{
			guint32	addr = (guint32) (inet_addr (line + 11));

			if (addr != (guint32) -1)
				nm_ip4_config_add_nameserver (ip4_config, addr);
		}
	}

	g_strfreev (split_contents);

out:
	g_free (contents);
}


/*
 * nm_system_device_get_system_config
 *
 * Retrieve any relevant configuration info for a particular device
 * from the system network configuration information.  Clear out existing
 * info before setting stuff too.
 *
 */
void* nm_system_device_get_system_config (NMDevice *dev)
{
	DebSystemConfigData *	sys_data = NULL;
	if_block *curr_device;
	const char *buf;
	gboolean				error = FALSE;

	g_return_val_if_fail (dev != NULL, NULL);

	sys_data = g_malloc0 (sizeof (DebSystemConfigData));
	sys_data->use_dhcp = TRUE;

	ifparser_init();

	/* Make sure this config file is for this device */
	curr_device = ifparser_getif(nm_device_get_iface (dev));
	if (curr_device == NULL)
		goto out;

	buf = ifparser_getkey(curr_device, "inet");
	if (buf)
	{
		if (strcmp (buf, "dhcp")!=0)
			sys_data->use_dhcp = FALSE;
	}

	buf = ifparser_getkey (curr_device, "address");
	if (buf)
		nm_ip4_config_set_address (sys_data->config, inet_addr (buf));

	buf = ifparser_getkey (curr_device, "gateway");
	if (buf)
		nm_ip4_config_set_gateway (sys_data->config, inet_addr (buf));

	buf = ifparser_getkey (curr_device, "netmask");
	if (buf)
		nm_ip4_config_set_netmask (sys_data->config, inet_addr (buf));
	else
	{
		guint32	addr = nm_ip4_config_get_address (sys_data->config);

		/* Make a default netmask if we have an IP address */
		if (((ntohl (addr) & 0xFF000000) >> 24) <= 127)
			nm_ip4_config_set_netmask (sys_data->config, htonl (0xFF000000));
		else if (((ntohl (addr) & 0xFF000000) >> 24) <= 191)
			nm_ip4_config_set_netmask (sys_data->config, htonl (0xFFFF0000));
		else
			nm_ip4_config_set_netmask (sys_data->config, htonl (0xFFFFFF00));
	}

	buf = ifparser_getkey (curr_device, "broadcast");
	if (buf)
		nm_ip4_config_set_broadcast (sys_data->config, inet_addr (buf));
	else
	{
		guint32 broadcast = ((nm_ip4_config_get_address (sys_data->config) & nm_ip4_config_get_netmask (sys_data->config))
								| ~nm_ip4_config_get_netmask (sys_data->config));
		nm_ip4_config_set_broadcast (sys_data->config, broadcast);
	}

        if (!sys_data->use_dhcp)
            set_ip4_config_from_resolv_conf (SYSCONFDIR"/resolv.conf", sys_data->config);

#if 0
	nm_debug ("------ Config (%s)", nm_device_get_iface (dev));
	nm_debug ("    DHCP=%d\n", use_dhcp);
	nm_debug ("    ADDR=%d\n", ip4_address);
	nm_debug ("    GW=%d\n", ip4_gateway);
	nm_debug ("    NM=%d\n", ip4_netmask);
	nm_debug ("---------------------\n");
#endif

out:
	ifparser_destroy();
	if (error)
	{
		sys_data->use_dhcp = TRUE;
		/* Clear out the config */
		nm_ip4_config_unref (sys_data->config);
		sys_data->config = NULL;
	}

	return (void *)sys_data;
}

/*
 * nm_system_device_free_system_config
 *
 * Free stored system config data
 *
 */
void nm_system_device_free_system_config (NMDevice *dev, void *system_config_data)
{
	DebSystemConfigData *sys_data = (DebSystemConfigData *)system_config_data;

	g_return_if_fail (dev != NULL);

	if (!sys_data)
		return;

	if (sys_data->config)
		nm_ip4_config_unref (sys_data->config);
}

NMIP4Config *nm_system_device_new_ip4_system_config (NMDevice *dev)
{
	DebSystemConfigData	*sys_data;
	NMIP4Config		*new_config = NULL;

	g_return_val_if_fail (dev != NULL, NULL);

	if ((sys_data = nm_device_get_system_config_data (dev)))
		new_config = nm_ip4_config_copy (sys_data->config);

	return new_config;
}
