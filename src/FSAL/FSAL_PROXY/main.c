/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Max Matveev, 2012
 *
 * Copyright CEA/DAM/DIF  (2008)
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <ctype.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "fsal.h"
#include "FSAL/fsal_init.h"
#include "pxy_fsal_methods.h"

#define PROXY_SUPPORTED_ATTRS ((const attrmask_t) (ATTRS_POSIX))

/* filesystem info for PROXY */
static struct fsal_staticfsinfo_t proxy_info = {
	.maxfilesize = INT64_MAX,
	.maxlink = _POSIX_LINK_MAX,
	.maxnamelen = 1024,
	.maxpathlen = 1024,
	.no_trunc = true,
	.chown_restricted = true,
	.case_preserving = true,
	.lock_support = false,
	.named_attr = true,
	.unique_handles = true,
	.lease_time = {10, 0},
	.acl_support = FSAL_ACLSUPPORT_ALLOW,
	.homogenous = true,
	.supported_attrs = PROXY_SUPPORTED_ATTRS,
	.link_supports_permission_checks = true,
};

#ifdef _USE_GSSRPC
static struct config_item_list sec_types[] = {
	CONFIG_LIST_TOK("krb5", RPCSEC_GSS_SVC_NONE),
	CONFIG_LIST_TOK("krb5i", RPCSEC_GSS_SVC_INTEGRITY),
	CONFIG_LIST_TOK("krb5p", RPCSEC_GSS_SVC_PRIVACY),
	CONFIG_LIST_EOL
};
#endif

/*512 bytes to store header*/
#define SEND_RECV_HEADER_SPACE 512
/*1MB of default maxsize*/
#define DEFAULT_MAX_WRITE_READ 1048576

static struct config_item proxy_remote_params[] = {
	CONF_ITEM_UI32("Retry_SleepTime", 0, 60, 10,
		       pxy_client_params, retry_sleeptime),
	CONF_MAND_IP_ADDR("Srv_Addr", "127.0.0.1",
			  pxy_client_params, srv_addr),
	CONF_ITEM_UI32("NFS_Service", 0, UINT32_MAX, 100003,
		       pxy_client_params, srv_prognum),
	CONF_ITEM_UI32("NFS_SendSize", 512 + SEND_RECV_HEADER_SPACE,
		       FSAL_MAXIOSIZE,
		       DEFAULT_MAX_WRITE_READ + SEND_RECV_HEADER_SPACE,
		       pxy_client_params, srv_sendsize),
	CONF_ITEM_UI32("NFS_RecvSize", 512 + SEND_RECV_HEADER_SPACE,
		       FSAL_MAXIOSIZE,
		       DEFAULT_MAX_WRITE_READ + SEND_RECV_HEADER_SPACE,
		       pxy_client_params, srv_recvsize),
	CONF_ITEM_UI16("NFS_Port", 0, UINT16_MAX, 2049,
		       pxy_client_params, srv_port),
	CONF_ITEM_BOOL("Use_Privileged_Client_Port", true,
		       pxy_client_params, use_privileged_client_port),
	CONF_ITEM_UI32("RPC_Client_Timeout", 1, 60*4, 60,
		       pxy_client_params, srv_timeout),
#ifdef _USE_GSSRPC
	CONF_ITEM_STR("Remote_PrincipalName", 0, MAXNAMLEN, NULL,
		      pxy_client_params, remote_principal),
	CONF_ITEM_STR("KeytabPath", 0, MAXPATHLEN, "/etc/krb5.keytab"
		      pxy_client_params, keytab),
	CONF_ITEM_UI32("Credential_LifeTime", 0, 86400*2, 86400,
		       pxy_client_params, cred_lifetime),
	CONF_ITEM_TOKEN("Sec_Type", RPCSEC_GSS_SVC_NONE, sec_types,
			pxy_client_params, sec_type),
	CONF_ITEM_BOOL("Active_krb5", false,
		       pxy_client_params, active_krb5),
#endif
#ifdef PROXY_HANDLE_MAPPING
	CONF_ITEM_BOOL("Enable_Handle_Mapping", false,
		       pxy_client_params, enable_handle_mapping),
	CONF_ITEM_STR("HandleMap_DB_Dir", 0, MAXPATHLEN,
		      "/var/ganesha/handlemap",
		      pxy_client_params, hdlmap.databases_directory),
	CONF_ITEM_STR("HandleMap_Tmp_Dir", 0, MAXPATHLEN,
		      "/var/ganesha/tmp",
		      pxy_client_params, hdlmap.temp_directory),
	CONF_ITEM_UI32("HandleMap_DB_Count", 1, 16, 8,
		       pxy_client_params, hdlmap.database_count),
	CONF_ITEM_UI32("HandleMap_HashTable_Size", 1, 127, 103,
		       pxy_client_params, hdlmap.hashtable_size),
#endif
	CONFIG_EOL
};

static int remote_commit(void *node, void *link_mem, void *self_struct,
		       struct config_error_type *err_type)
{
	/* struct pxy_client_params *pcpi = self_struct; */
	struct pxy_fsal_module *pxy;

	pxy = container_of(link_mem, struct pxy_fsal_module, special);

	if (pxy->fsinfo.maxwrite + SEND_RECV_HEADER_SPACE >
			pxy->special.srv_sendsize ||
	    pxy->fsinfo.maxread + SEND_RECV_HEADER_SPACE >
			pxy->special.srv_recvsize) {
		LogCrit(COMPONENT_CONFIG,
"FSAL_PROXY CONF : maxwrite/maxread + header > Max_SendSize/Max_RecvSize");
		err_type->invalid = true;
		return 1;
	}

	/* Other verifications/parameter checking to be added here */

	return 0;
}

/**
 * @brief Validate and commit the proxy params
 *
 * This is also pretty simple.  Just a NOP in both cases.
 *
 * @param link_mem - pointer to the link_mem struct memory.
 * @param self_struct - NULL for init parent, not NULL for attaching
 */

static struct config_item proxy_params[] = {
	CONF_ITEM_BOOL("link_support", true,
		       pxy_fsal_module, fsinfo.link_support),
	CONF_ITEM_BOOL("symlink_support", true,
		       pxy_fsal_module, fsinfo.symlink_support),
	CONF_ITEM_BOOL("cansettime", true,
		       pxy_fsal_module, fsinfo.cansettime),
	CONF_ITEM_UI64("maxread", 512,
		       FSAL_MAXIOSIZE - SEND_RECV_HEADER_SPACE,
		       DEFAULT_MAX_WRITE_READ,
		       pxy_fsal_module, fsinfo.maxread),
	CONF_ITEM_UI64("maxwrite", 512,
		       FSAL_MAXIOSIZE - SEND_RECV_HEADER_SPACE,
		       DEFAULT_MAX_WRITE_READ,
		       pxy_fsal_module, fsinfo.maxwrite),
	CONF_ITEM_MODE("umask", 0,
		       pxy_fsal_module, fsinfo.umask),
	CONF_ITEM_BOOL("auth_xdev_export", false,
		       pxy_fsal_module, fsinfo.auth_exportpath_xdev),
	CONF_ITEM_MODE("xattr_access_rights", 0400,
		       pxy_fsal_module, fsinfo.xattr_access_rights),
	CONF_ITEM_BLOCK("Remote_Server", proxy_remote_params,
		       noop_conf_init, remote_commit,
		       pxy_fsal_module, special),

	CONFIG_EOL
};

struct config_block proxy_param = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.proxy",
	.blk_desc.name = "PROXY",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = proxy_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

static fsal_status_t pxy_init_config(struct fsal_module *fsal_hdl,
				     config_file_t config_struct,
				     struct config_error_type *err_type)
{
	int rc;
	struct pxy_fsal_module *pxy =
	    container_of(fsal_hdl, struct pxy_fsal_module, module);

	pxy->fsinfo = proxy_info;
	(void) load_config_from_parse(config_struct,
				      &proxy_param,
				      pxy,
				      true,
				      err_type);
	if (!config_error_is_harmless(err_type))
		return fsalstat(ERR_FSAL_INVAL, 0);

#ifdef PROXY_HANDLE_MAPPING
	rc = HandleMap_Init(&pxy->special.hdlmap);
	if (rc < 0)
		return fsalstat(ERR_FSAL_INVAL, -rc);
#endif

	rc = pxy_init_rpc(pxy);
	if (rc)
		return fsalstat(ERR_FSAL_FAULT, rc);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static bool pxy_support_ex(struct fsal_obj_handle *obj)
{
	return true;
}

static struct pxy_fsal_module PROXY;

MODULE_INIT void pxy_init(void)
{
	if (register_fsal(&PROXY.module, "PROXY", FSAL_MAJOR_VERSION,
			  FSAL_MINOR_VERSION, FSAL_ID_NO_PNFS) != 0)
		return;
	PROXY.module.m_ops.init_config = pxy_init_config;
	PROXY.module.m_ops.create_export = pxy_create_export;
	PROXY.module.m_ops.support_ex = pxy_support_ex;
}

MODULE_FINI void pxy_unload(void)
{
	int retval;

	retval = unregister_fsal(&PROXY.module);

	if (retval != 0) {
		fprintf(stderr, "PROXY module failed to unregister : %d",
			retval);
		return;
	}

	retval = pxy_close_thread();
	if (retval != 0) {
		fprintf(stderr, "PROXY module failed to wait threads : %d",
			retval);
		return;
	}
}
