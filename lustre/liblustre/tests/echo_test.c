/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Light user test program
 *
 *  Copyright (c) 2002-2004 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <liblustre.h>
#include <linux/obd.h>
#include <linux/obd_class.h>

#define LIBLUSTRE_TEST 1
#include "../utils/lctl.c"

#include "../lutil.h"

extern int class_handle_ioctl(unsigned int cmd, unsigned long arg);

struct pingcli_args {
        ptl_nid_t mynid;
        ptl_nid_t nid;
	ptl_pid_t port;
        int count;
        int size;
};
/*      bug #4615       */
#if 0
char *portals_id2str(int nal, ptl_process_id_t id, char *str)
{
        switch(nal){
        case TCPNAL:
                /* userspace NAL */
        case IIBNAL:
        case VIBNAL:
        case OPENIBNAL:
        case RANAL:
        case SOCKNAL:
                snprintf(str, PTL_NALFMT_SIZE - 1, "%u:%u.%u.%u.%u,%u",
                         (__u32)(id.nid >> 32), HIPQUAD((id.nid)) , id.pid);
                break;
        case QSWNAL:
        case GMNAL:
        case LONAL:
                snprintf(str, PTL_NALFMT_SIZE - 1, "%u:%u,%u",
                         (__u32)(id.nid >> 32), (__u32)id.nid, id.pid);
                break;
        default:
                snprintf(str, PTL_NALFMT_SIZE - 1, "?%d? %llx,%lx",
                         nal, (long long)id.nid, (long)id.pid );
                break;
        }
        return str;
}
#endif

static int liblustre_ioctl(int dev_id, unsigned int opc, void *ptr)
{
	int   rc = -EINVAL;
	
	switch (dev_id) {
	default:
		fprintf(stderr, "Unexpected device id %d\n", dev_id);
		abort();
		break;
		
	case OBD_DEV_ID:
		rc = class_handle_ioctl(opc, (unsigned long)ptr);
		break;
	}

	return rc;
}

static char *echo_server_nid = NULL;
static char *echo_server_ostname = "obd1";
static char *osc_dev_name = "OSC_DEV_NAME";
static char *echo_dev_name = "ECHO_CLIENT_DEV_NAME";

static int connect_echo_client(void)
{
	struct lustre_cfg lcfg;
	ptl_nid_t nid;
	char *peer = "ECHO_PEER_NID";
	class_uuid_t osc_uuid, echo_uuid;
	struct obd_uuid osc_uuid_str, echo_uuid_str;
	int nal, err;
	ENTRY;

        generate_random_uuid(osc_uuid);
        class_uuid_unparse(osc_uuid, &osc_uuid_str);
        generate_random_uuid(echo_uuid);
        class_uuid_unparse(echo_uuid, &echo_uuid_str);

        if (ptl_parse_nid(&nid, echo_server_nid)) {
                CERROR("Can't parse NID %s\n", echo_server_nid);
                RETURN(-EINVAL);
        }
        nal = ptl_name2nal("tcp");
        if (nal <= 0) {
                CERROR("Can't parse NAL tcp\n");
                RETURN(-EINVAL);
        }

	/* add uuid */
        LCFG_INIT(lcfg, LCFG_ADD_UUID, NULL);
        lcfg.lcfg_nid = nid;
        lcfg.lcfg_inllen1 = strlen(peer) + 1;
        lcfg.lcfg_inlbuf1 = peer;
        lcfg.lcfg_nal = nal;
        err = class_process_config(&lcfg);
        if (err < 0) {
		CERROR("failed add_uuid\n");
                RETURN(-EINVAL);
	}

	/* attach osc */
        LCFG_INIT(lcfg, LCFG_ATTACH, osc_dev_name);
        lcfg.lcfg_inlbuf1 = "osc";
        lcfg.lcfg_inllen1 = strlen(lcfg.lcfg_inlbuf1) + 1;
        lcfg.lcfg_inlbuf2 = osc_uuid_str.uuid;
        lcfg.lcfg_inllen2 = strlen(lcfg.lcfg_inlbuf2) + 1;
        err = class_process_config(&lcfg);
        if (err < 0) {
		CERROR("failed attach osc\n");
                RETURN(-EINVAL);
	}

	/* setup osc */
        LCFG_INIT(lcfg, LCFG_SETUP, osc_dev_name);
        lcfg.lcfg_inlbuf1 = echo_server_ostname;
        lcfg.lcfg_inllen1 = strlen(lcfg.lcfg_inlbuf1) + 1;
        lcfg.lcfg_inlbuf2 = peer;
        lcfg.lcfg_inllen2 = strlen(lcfg.lcfg_inlbuf2) + 1;
        err = class_process_config(&lcfg);
        if (err < 0) {
		CERROR("failed setup osc\n");
                RETURN(-EINVAL);
	}

	/* attach echo_client */
        LCFG_INIT(lcfg, LCFG_ATTACH, echo_dev_name);
        lcfg.lcfg_inlbuf1 = "echo_client";
        lcfg.lcfg_inllen1 = strlen(lcfg.lcfg_inlbuf1) + 1;
        lcfg.lcfg_inlbuf2 = echo_uuid_str.uuid;
        lcfg.lcfg_inllen2 = strlen(lcfg.lcfg_inlbuf2) + 1;
        err = class_process_config(&lcfg);
        if (err < 0) {
		CERROR("failed attach echo_client\n");
                RETURN(-EINVAL);
	}

	/* setup echo_client */
        LCFG_INIT(lcfg, LCFG_SETUP, echo_dev_name);
        lcfg.lcfg_inlbuf1 = osc_dev_name;
        lcfg.lcfg_inllen1 = strlen(lcfg.lcfg_inlbuf1) + 1;
        lcfg.lcfg_inlbuf2 = NULL;
        lcfg.lcfg_inllen2 = 0;
        err = class_process_config(&lcfg);
        if (err < 0) {
		CERROR("failed setup echo_client\n");
                RETURN(-EINVAL);
	}

	RETURN(0);
}

static int disconnect_echo_client(void)
{
	struct lustre_cfg lcfg;
	int err;
	ENTRY;

	/* cleanup echo_client */
        LCFG_INIT(lcfg, LCFG_CLEANUP, echo_dev_name);
        err = class_process_config(&lcfg);
        if (err < 0) {
		CERROR("failed cleanup echo_client\n");
                RETURN(-EINVAL);
	}

	/* detach echo_client */
        LCFG_INIT(lcfg, LCFG_DETACH, echo_dev_name);
        err = class_process_config(&lcfg);
        if (err < 0) {
		CERROR("failed detach echo_client\n");
                RETURN(-EINVAL);
	}

	/* cleanup osc */
        LCFG_INIT(lcfg, LCFG_CLEANUP, osc_dev_name);
        err = class_process_config(&lcfg);
        if (err < 0) {
		CERROR("failed cleanup osc device\n");
                RETURN(-EINVAL);
	}

	/* detach osc */
        LCFG_INIT(lcfg, LCFG_DETACH, osc_dev_name);
        err = class_process_config(&lcfg);
        if (err < 0) {
		CERROR("failed detach osc device\n");
                RETURN(-EINVAL);
	}

	RETURN(0);
}

static void usage(const char *s)
{
	printf("Usage: %s -s ost_host_name [-n ost_name]\n", s);
	printf("    ost_host_name: the host name of echo server\n");
	printf("    ost_name: ost name, default is \"obd1\"\n");
}

extern int time_ptlwait1;
extern int time_ptlwait2;
extern int time_ptlselect;

int main(int argc, char **argv) 
{
	int c, rc;

	while ((c = getopt(argc, argv, "s:n:")) != -1) {
		switch (c) {
		case 's':
			echo_server_nid = optarg;
			break;
		case 'n':
			echo_server_ostname = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

        if (optind != argc)
                usage(argv[0]);

	if (!echo_server_nid) {
		usage(argv[0]);
		return 1;
	}

	portal_debug = 0;
	portal_subsystem_debug = 0;

        liblustre_init_random();
        liblustre_set_nal_nid();

        if (liblustre_init_current(argv[0]) ||
	    init_obdclass() || init_lib_portals() ||
	    ptlrpc_init() ||
	    mdc_init() ||
	    lov_init() ||
	    osc_init() ||
	    echo_client_init()) {
		printf("error\n");
		return 1;
	}

	rc = connect_echo_client();
	if (rc)
		return rc;

	set_ioc_handler(liblustre_ioctl);

	rc = lctl_main(1, &argv[0]);

	rc |= disconnect_echo_client();

	return rc;
}
