/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mgs/mgs_handler.c
 *  Lustre Management Server (mgs) request handler
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
 *   Author: Nathan Rutman <nathan@clusterfs.com>
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

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MGS
#define D_MGS D_CONFIG/*|D_WARNING*/

#ifdef __KERNEL__
# include <linux/module.h>
# include <linux/pagemap.h>
# include <linux/miscdevice.h>
# include <linux/init.h>
#else
# include <liblustre.h>
#endif

#include <obd_class.h>
#include <lustre_dlm.h>
#include <lprocfs_status.h>
#include <lustre_fsfilt.h>
#include <lustre_commit_confd.h>
#include <lustre_disk.h>
#include "mgs_internal.h"


/* Establish a connection to the MGS.*/
static int mgs_connect(const struct lu_env *env,
                       struct lustre_handle *conn, struct obd_device *obd,
                       struct obd_uuid *cluuid, struct obd_connect_data *data,
                       void *localdata)
{
        struct obd_export *exp;
        int rc;
        ENTRY;

        if (!conn || !obd || !cluuid)
                RETURN(-EINVAL);

        rc = class_connect(conn, obd, cluuid);
        if (rc)
                RETURN(rc);
        exp = class_conn2export(conn);
        LASSERT(exp);

        exp->exp_flvr.sf_rpc = SPTLRPC_FLVR_NULL;

        mgs_counter_incr(exp, LPROC_MGS_CONNECT);

        if (data != NULL) {
                data->ocd_connect_flags &= MGS_CONNECT_SUPPORTED;
                exp->exp_connect_flags = data->ocd_connect_flags;
                data->ocd_version = LUSTRE_VERSION_CODE;
        }

        if ((exp->exp_connect_flags & OBD_CONNECT_FID) == 0) {
                CWARN("MGS requires FID support, but client not\n");
                rc = -EBADE;
        }

        if (rc) {
                class_disconnect(exp);
        } else {
                class_export_put(exp);
        }

        RETURN(rc);
}

static int mgs_disconnect(struct obd_export *exp)
{
        int rc;
        ENTRY;

        LASSERT(exp);

        class_export_get(exp);
        mgs_counter_incr(exp, LPROC_MGS_DISCONNECT);

        /* Disconnect early so that clients can't keep using export */
        rc = class_disconnect(exp);
        ldlm_cancel_locks_for_export(exp);

        /* complete all outstanding replies */
        spin_lock(&exp->exp_lock);
        while (!list_empty(&exp->exp_outstanding_replies)) {
                struct ptlrpc_reply_state *rs =
                        list_entry(exp->exp_outstanding_replies.next,
                                   struct ptlrpc_reply_state, rs_exp_list);
                struct ptlrpc_service *svc = rs->rs_service;

                spin_lock(&svc->srv_lock);
                list_del_init(&rs->rs_exp_list);
                ptlrpc_schedule_difficult_reply(rs);
                spin_unlock(&svc->srv_lock);
        }
        spin_unlock(&exp->exp_lock);

        class_export_put(exp);
        RETURN(rc);
}

static int mgs_cleanup(struct obd_device *obd);
static int mgs_handle(struct ptlrpc_request *req);

static int mgs_llog_init(struct obd_device *obd, int group,
                         struct obd_device *tgt, int count,
                         struct llog_catid *logid, struct obd_uuid *uuid)
{
        struct obd_llog_group *olg = &obd->obd_olg;
        int rc;
        ENTRY;

        LASSERT(group == OBD_LLOG_GROUP);
        LASSERT(olg->olg_group == group);

        rc = llog_setup(obd, olg, LLOG_CONFIG_ORIG_CTXT, obd, 0, NULL,
                        &llog_lvfs_ops);
        RETURN(rc);
}

static int mgs_llog_finish(struct obd_device *obd, int count)
{
        struct llog_ctxt *ctxt;
        int rc = 0;
        ENTRY;

        ctxt = llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT);
        if (ctxt)
                rc = llog_cleanup(ctxt);

        RETURN(rc);
}

/* Start the MGS obd */
static int mgs_setup(struct obd_device *obd, struct lustre_cfg *lcfg)
{
        struct lprocfs_static_vars lvars;
        struct mgs_obd *mgs = &obd->u.mgs;
        struct lustre_mount_info *lmi;
        struct lustre_sb_info *lsi;
        struct vfsmount *mnt;
        int rc = 0;
        ENTRY;

        CDEBUG(D_CONFIG, "Starting MGS\n");

        /* Find our disk */
        lmi = server_get_mount(obd->obd_name);
        if (!lmi)
                RETURN(rc = -EINVAL);

        mnt = lmi->lmi_mnt;
        lsi = s2lsi(lmi->lmi_sb);
        obd->obd_fsops = fsfilt_get_ops(MT_STR(lsi->lsi_ldd));
        if (IS_ERR(obd->obd_fsops))
                GOTO(err_put, rc = PTR_ERR(obd->obd_fsops));

        /* namespace for mgs llog */
        obd->obd_namespace = ldlm_namespace_new("MGS", LDLM_NAMESPACE_SERVER,
                                                LDLM_NAMESPACE_MODEST);
        if (obd->obd_namespace == NULL)
                GOTO(err_ops, rc = -ENOMEM);

        /* ldlm setup */
        ptlrpc_init_client(LDLM_CB_REQUEST_PORTAL, LDLM_CB_REPLY_PORTAL,
                           "mgs_ldlm_client", &obd->obd_ldlm_client);

        LASSERT(!lvfs_check_rdonly(lvfs_sbdev(mnt->mnt_sb)));

        rc = mgs_fs_setup(obd, mnt);
        if (rc) {
                CERROR("%s: MGS filesystem method init failed: rc = %d\n",
                       obd->obd_name, rc);
                GOTO(err_ns, rc);
        }

        rc = obd_llog_init(obd, OBD_LLOG_GROUP, obd, 0, NULL, NULL);
        if (rc)
                GOTO(err_fs, rc);

        /* No recovery for MGC's */
        obd->obd_replayable = 0;

        /* Internal mgs setup */
        mgs_init_fsdb_list(obd);
        sema_init(&mgs->mgs_sem, 1);

        /* Start the service threads */
        mgs->mgs_service =
                ptlrpc_init_svc(MGS_NBUFS, MGS_BUFSIZE, MGS_MAXREQSIZE,
                                MGS_MAXREPSIZE, MGS_REQUEST_PORTAL,
                                MGC_REPLY_PORTAL, MGS_SERVICE_WATCHDOG_TIMEOUT,
                                mgs_handle, LUSTRE_MGS_NAME,
                                obd->obd_proc_entry, NULL,
                                MGS_THREADS_AUTO_MIN, MGS_THREADS_AUTO_MAX,
                                "ll_mgs", LCT_MD_THREAD);

        if (!mgs->mgs_service) {
                CERROR("failed to start service\n");
                GOTO(err_llog, rc = -ENOMEM);
        }

        rc = ptlrpc_start_threads(obd, mgs->mgs_service);
        if (rc)
                GOTO(err_thread, rc);

        /* Setup proc */
        lprocfs_mgs_init_vars(&lvars);
        if (lprocfs_obd_setup(obd, lvars.obd_vars) == 0) {
                lproc_mgs_setup(obd);
        }

        ping_evictor_start();

        LCONSOLE_INFO("MGS %s started\n", obd->obd_name);

        RETURN(0);

err_thread:
        ptlrpc_unregister_service(mgs->mgs_service);
err_llog:
        obd_llog_finish(obd, 0);
err_fs:
        /* No extra cleanup needed for llog_init_commit_thread() */
        mgs_fs_cleanup(obd);
err_ns:
        ldlm_namespace_free(obd->obd_namespace, NULL, 0);
        obd->obd_namespace = NULL;
err_ops:
        fsfilt_put_ops(obd->obd_fsops);
err_put:
        server_put_mount(obd->obd_name, mnt);
        mgs->mgs_sb = 0;
        return rc;
}

static int mgs_precleanup(struct obd_device *obd, enum obd_cleanup_stage stage)
{
        int rc = 0;
        ENTRY;

        switch (stage) {
        case OBD_CLEANUP_EARLY:
        case OBD_CLEANUP_EXPORTS:
                break;
        case OBD_CLEANUP_SELF_EXP:
                rc = obd_llog_finish(obd, 0);
                break;
        case OBD_CLEANUP_OBD:
                break;
        }
        RETURN(rc);
}

static int mgs_ldlm_nsfree(void *data)
{
        struct ldlm_namespace *ns = (struct ldlm_namespace *)data;
        ENTRY;

        ptlrpc_daemonize("ll_mgs_nsfree");
        ldlm_namespace_free(ns, NULL, 1 /* obd_force should always be on */);
        RETURN(0);
}

static int mgs_cleanup(struct obd_device *obd)
{
        struct mgs_obd *mgs = &obd->u.mgs;
        ENTRY;

        if (mgs->mgs_sb == NULL)
                RETURN(0);

        ping_evictor_stop();

        ptlrpc_unregister_service(mgs->mgs_service);

        mgs_cleanup_fsdb_list(obd);
        lproc_mgs_cleanup(obd);
        mgs_fs_cleanup(obd);

        server_put_mount(obd->obd_name, mgs->mgs_vfsmnt);
        mgs->mgs_sb = NULL;

        /* Free the namespace in it's own thread, so that if the
           ldlm_cancel_handler put the last mgs obd ref, we won't
           deadlock here. */
        cfs_kernel_thread(mgs_ldlm_nsfree, obd->obd_namespace,
                          CLONE_VM | CLONE_FILES);


        fsfilt_put_ops(obd->obd_fsops);

        LCONSOLE_INFO("%s has stopped.\n", obd->obd_name);
        RETURN(0);
}

/* similar to filter_prepare_destroy */
static int mgs_get_cfg_lock(struct obd_device *obd, char *fsname,
                            struct lustre_handle *lockh)
{
        struct ldlm_res_id res_id;
        int rc, flags = 0;
        ENTRY;

        rc = mgc_fsname2resid(fsname, &res_id);
        if (!rc)
                rc = ldlm_cli_enqueue_local(obd->obd_namespace, &res_id,
                                            LDLM_PLAIN, NULL, LCK_EX,
                                            &flags, ldlm_blocking_ast,
                                            ldlm_completion_ast, NULL,
                                            fsname, 0, NULL, lockh);
        if (rc)
                CERROR("can't take cfg lock for %s (%d)\n", fsname, rc);

        RETURN(rc);
}

static int mgs_put_cfg_lock(struct lustre_handle *lockh)
{
        ENTRY;
        ldlm_lock_decref(lockh, LCK_EX);
        RETURN(0);
}

/* rc=0 means ok
      1 means update
     <0 means error */
static int mgs_check_target(struct obd_device *obd, struct mgs_target_info *mti)
{
        int rc;
        ENTRY;

        rc = mgs_check_index(obd, mti);
        if (rc == 0) {
                LCONSOLE_ERROR_MSG(0x13b, "%s claims to have registered, but "
                                   "this MGS does not know about it. Assuming"
                                   " writeconf.\n", mti->mti_svname);
                mti->mti_flags |= LDD_F_WRITECONF;
                rc = 1;
        } else if (rc == -1) {
                LCONSOLE_ERROR_MSG(0x13c, "Client log %s-client has "
                                   "disappeared! Regenerating all logs.\n",
                                   mti->mti_fsname);
                mti->mti_flags |= LDD_F_WRITECONF;
                rc = 1;
        } else {
                /* Index is correctly marked as used */

                /* If the logs don't contain the mti_nids then add
                   them as failover nids */
                rc = mgs_check_failnid(obd, mti);
        }

        RETURN(rc);
}

/* Called whenever a target starts up.  Flags indicate first connect, etc. */
static int mgs_handle_target_reg(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct lustre_handle lockh;
        struct mgs_target_info *mti, *rep_mti;
        int rc = 0, lockrc;
        ENTRY;

        mgs_counter_incr(req->rq_export, LPROC_MGS_TARGET_REG);

        mti = req_capsule_client_get(&req->rq_pill, &RMF_MGS_TARGET_INFO);
        if (!(mti->mti_flags & (LDD_F_WRITECONF | LDD_F_UPGRADE14 |
                                LDD_F_UPDATE))) {
                /* We're just here as a startup ping. */
                CDEBUG(D_MGS, "Server %s is running on %s\n",
                       mti->mti_svname, obd_export_nid2str(req->rq_export));
                rc = mgs_check_target(obd, mti);
                /* above will set appropriate mti flags */
                if (rc <= 0)
                        /* Nothing wrong, or fatal error */
                        GOTO(out_nolock, rc);
        }

        /* Revoke the config lock to make sure nobody is reading. */
        /* Although actually I think it should be alright if
           someone was reading while we were updating the logs - if we
           revoke at the end they will just update from where they left off. */
        lockrc = mgs_get_cfg_lock(obd, mti->mti_fsname, &lockh);
        if (lockrc != ELDLM_OK) {
                LCONSOLE_ERROR_MSG(0x13d, "%s: Can't signal other nodes to "
                                   "update their configuration (%d). Updating "
                                   "local logs anyhow; you might have to "
                                   "manually restart other nodes to get the "
                                   "latest configuration.\n",
                                   obd->obd_name, lockrc);
        }

        OBD_FAIL_TIMEOUT(OBD_FAIL_MGS_SLOW_TARGET_REG, 10);

        /* Log writing contention is handled by the fsdb_sem */

        if (mti->mti_flags & LDD_F_WRITECONF) {
                if (mti->mti_flags & LDD_F_SV_TYPE_MDT &&
                    mti->mti_stripe_index == 0) {
                        rc = mgs_erase_logs(obd, mti->mti_fsname);
                        LCONSOLE_WARN("%s: Logs for fs %s were removed by user "
                                      "request.  All servers must be restarted "
                                      "in order to regenerate the logs."
                                      "\n", obd->obd_name, mti->mti_fsname);
                } else if (mti->mti_flags &
                           (LDD_F_SV_TYPE_OST | LDD_F_SV_TYPE_MDT)) {
                        rc = mgs_erase_log(obd, mti->mti_svname);
                        LCONSOLE_WARN("%s: Regenerating %s log by user "
                                      "request.\n",
                                      obd->obd_name, mti->mti_svname);
                }
                mti->mti_flags |= LDD_F_UPDATE;
                /* Erased logs means start from scratch. */
                mti->mti_flags &= ~LDD_F_UPGRADE14; 
        }

        /* COMPAT_146 */
        if (mti->mti_flags & LDD_F_UPGRADE14) {
                rc = mgs_upgrade_sv_14(obd, mti);
                if (rc) {
                        CERROR("Can't upgrade from 1.4 (%d)\n", rc);
                        GOTO(out, rc);
                }
                
                /* We're good to go */
                mti->mti_flags |= LDD_F_UPDATE;
        }
        /* end COMPAT_146 */

        if (mti->mti_flags & LDD_F_UPDATE) {
                CDEBUG(D_MGS, "updating %s, index=%d\n", mti->mti_svname,
                       mti->mti_stripe_index);

                /* create or update the target log
                   and update the client/mdt logs */
                rc = mgs_write_log_target(obd, mti);
                if (rc) {
                        CERROR("Failed to write %s log (%d)\n",
                               mti->mti_svname, rc);
                        GOTO(out, rc);
                }

                mti->mti_flags &= ~(LDD_F_VIRGIN | LDD_F_UPDATE |
                                    LDD_F_NEED_INDEX | LDD_F_WRITECONF |
                                    LDD_F_UPGRADE14);
                mti->mti_flags |= LDD_F_REWRITE_LDD;
        }

out:
        /* done with log update */
        if (lockrc == ELDLM_OK)
                mgs_put_cfg_lock(&lockh);
out_nolock:
        CDEBUG(D_MGS, "replying with %s, index=%d, rc=%d\n", mti->mti_svname,
               mti->mti_stripe_index, rc);
        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                RETURN(rc);

        /* send back the whole mti in the reply */
        rep_mti = req_capsule_server_get(&req->rq_pill, &RMF_MGS_TARGET_INFO);
        *rep_mti = *mti;

        /* Flush logs to disk */
        fsfilt_sync(obd, obd->u.mgs.mgs_sb);
        RETURN(rc);
}

static int mgs_set_info_rpc(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct mgs_send_param *msp, *rep_msp;
        struct lustre_handle lockh;
        int lockrc, rc;
        struct lustre_cfg_bufs bufs;
        struct lustre_cfg *lcfg;
        char fsname[MTI_NAME_MAXLEN];
        ENTRY;

        msp = req_capsule_client_get(&req->rq_pill, &RMF_MGS_SEND_PARAM);
        LASSERT(msp);

        /* Construct lustre_cfg structure to pass to function mgs_setparam */
        lustre_cfg_bufs_reset(&bufs, NULL);
        lustre_cfg_bufs_set_string(&bufs, 1, msp->mgs_param);
        lcfg = lustre_cfg_new(LCFG_PARAM, &bufs);
        rc = mgs_setparam(obd, lcfg, fsname);
        if (rc) {
                CERROR("Error %d in setting the parameter %s for fs %s\n",
                       rc, msp->mgs_param, fsname);
                RETURN(rc);
        }

        /* Revoke lock so everyone updates.  Should be alright if
         * someone was already reading while we were updating the logs,
         * so we don't really need to hold the lock while we're
         * writing.
         */
        if (fsname[0]) {
                lockrc = mgs_get_cfg_lock(obd, fsname, &lockh);
                if (lockrc != ELDLM_OK)
                        CERROR("lock error %d for fs %s\n", lockrc,
                               fsname);
                else
                        mgs_put_cfg_lock(&lockh);
        }
        lustre_cfg_free(lcfg);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc == 0) {
                rep_msp = req_capsule_server_get(&req->rq_pill, &RMF_MGS_SEND_PARAM);
                rep_msp = msp;
        }
        RETURN(rc);
}

/* Called whenever a target cleans up. */
/* XXX - Currently unused */
static int mgs_handle_target_del(struct ptlrpc_request *req)
{
        ENTRY;
        mgs_counter_incr(req->rq_export, LPROC_MGS_TARGET_DEL);
        RETURN(0);
}

/* XXX - Currently unused */
static int mgs_handle_exception(struct ptlrpc_request *req)
{
        ENTRY;
        mgs_counter_incr(req->rq_export, LPROC_MGS_EXCEPTION);
        RETURN(0);
}

/* TODO: handle requests in a similar way as MDT: see mdt_handle_common() */
int mgs_handle(struct ptlrpc_request *req)
{
        int fail = OBD_FAIL_MGS_ALL_REPLY_NET;
        int opc, rc = 0;
        ENTRY;

        req_capsule_init(&req->rq_pill, req, RCL_SERVER);
        OBD_FAIL_TIMEOUT(OBD_FAIL_MGS_SLOW_REQUEST_NET, 2);

        LASSERT(current->journal_info == NULL);
        opc = lustre_msg_get_opc(req->rq_reqmsg);
        if (opc != MGS_CONNECT) {
                if (req->rq_export == NULL) {
                        CERROR("lustre_mgs: operation %d on unconnected MGS\n",
                               opc);
                        req->rq_status = -ENOTCONN;
                        GOTO(out, rc = -ENOTCONN);
                }
        }

        switch (opc) {
        case MGS_CONNECT:
                DEBUG_REQ(D_MGS, req, "connect");
                /* MGS and MDS have same request format for connect */
                req_capsule_set(&req->rq_pill, &RQF_MDS_CONNECT);
                rc = target_handle_connect(req);
                if (!rc && (lustre_msg_get_conn_cnt(req->rq_reqmsg) > 1))
                        /* Make clients trying to reconnect after a MGS restart
                           happy; also requires obd_replayable */
                        lustre_msg_add_op_flags(req->rq_repmsg,
                                                MSG_CONNECT_RECONNECT);
                break;
        case MGS_DISCONNECT:
                DEBUG_REQ(D_MGS, req, "disconnect");
                /* MGS and MDS have same request format for disconnect */
                req_capsule_set(&req->rq_pill, &RQF_MDS_DISCONNECT);
                rc = target_handle_disconnect(req);
                req->rq_status = rc;            /* superfluous? */
                break;
        case MGS_EXCEPTION:
                DEBUG_REQ(D_MGS, req, "exception");
                rc = mgs_handle_exception(req);
                break;
        case MGS_TARGET_REG:
                DEBUG_REQ(D_MGS, req, "target add");
                req_capsule_set(&req->rq_pill, &RQF_MGS_TARGET_REG);
                rc = mgs_handle_target_reg(req);
                break;
        case MGS_TARGET_DEL:
                DEBUG_REQ(D_MGS, req, "target del");
                rc = mgs_handle_target_del(req);
                break;
        case MGS_SET_INFO:
                DEBUG_REQ(D_MGS, req, "set_info");
                req_capsule_set(&req->rq_pill, &RQF_MGS_SET_INFO);
                rc = mgs_set_info_rpc(req);
                break;

        case LDLM_ENQUEUE:
                DEBUG_REQ(D_MGS, req, "enqueue");
                req_capsule_set(&req->rq_pill, &RQF_LDLM_ENQUEUE);
                rc = ldlm_handle_enqueue(req, ldlm_server_completion_ast,
                                         ldlm_server_blocking_ast, NULL);
                break;
        case LDLM_BL_CALLBACK:
        case LDLM_CP_CALLBACK:
                DEBUG_REQ(D_MGS, req, "callback");
                CERROR("callbacks should not happen on MGS\n");
                LBUG();
                break;

        case OBD_PING:
                DEBUG_REQ(D_INFO, req, "ping");
                req_capsule_set(&req->rq_pill, &RQF_OBD_PING);
                rc = target_handle_ping(req);
                break;
        case OBD_LOG_CANCEL:
                DEBUG_REQ(D_MGS, req, "log cancel");
                rc = -ENOTSUPP; /* la la la */
                break;

        case LLOG_ORIGIN_HANDLE_CREATE:
                DEBUG_REQ(D_MGS, req, "llog_init");
                req_capsule_set(&req->rq_pill, &RQF_LLOG_ORIGIN_HANDLE_CREATE);
                rc = llog_origin_handle_create(req);
                break;
        case LLOG_ORIGIN_HANDLE_NEXT_BLOCK:
                DEBUG_REQ(D_MGS, req, "llog next block");
                req_capsule_set(&req->rq_pill,
                                &RQF_LLOG_ORIGIN_HANDLE_NEXT_BLOCK);
                rc = llog_origin_handle_next_block(req);
                break;
        case LLOG_ORIGIN_HANDLE_READ_HEADER:
                DEBUG_REQ(D_MGS, req, "llog read header");
                req_capsule_set(&req->rq_pill,
                                &RQF_LLOG_ORIGIN_HANDLE_READ_HEADER);
                rc = llog_origin_handle_read_header(req);
                break;
        case LLOG_ORIGIN_HANDLE_CLOSE:
                DEBUG_REQ(D_MGS, req, "llog close");
                rc = llog_origin_handle_close(req);
                break;
        case LLOG_CATINFO:
                DEBUG_REQ(D_MGS, req, "llog catinfo");
                req_capsule_set(&req->rq_pill, &RQF_LLOG_CATINFO);
                rc = llog_catinfo(req);
                break;
        default:
                req->rq_status = -ENOTSUPP;
                rc = ptlrpc_error(req);
                RETURN(rc);
        }

        LASSERT(current->journal_info == NULL);

        if (rc)
                CERROR("MGS handle cmd=%d rc=%d\n", opc, rc);

out:
        target_send_reply(req, rc, fail);
        RETURN(0);
}

static inline int mgs_destroy_export(struct obd_export *exp)
{
        ENTRY;

        target_destroy_export(exp);

        RETURN(0);
}

/* from mdt_iocontrol */
int mgs_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                  void *karg, void *uarg)
{
        struct obd_device *obd = exp->exp_obd;
        struct obd_ioctl_data *data = karg;
        struct lvfs_run_ctxt saved;
        int rc = 0;

        ENTRY;
        CDEBUG(D_IOCTL, "handling ioctl cmd %#x\n", cmd);

        switch (cmd) {

        case OBD_IOC_PARAM: {
                struct lustre_handle lockh;
                struct lustre_cfg *lcfg;
                struct llog_rec_hdr rec;
                char fsname[MTI_NAME_MAXLEN];
                int lockrc;

                rec.lrh_len = llog_data_len(data->ioc_plen1);

                if (data->ioc_type == LUSTRE_CFG_TYPE) {
                        rec.lrh_type = OBD_CFG_REC;
                } else {
                        CERROR("unknown cfg record type:%d \n", data->ioc_type);
                        RETURN(-EINVAL);
                }

                OBD_ALLOC(lcfg, data->ioc_plen1);
                if (lcfg == NULL)
                        RETURN(-ENOMEM);
                rc = copy_from_user(lcfg, data->ioc_pbuf1, data->ioc_plen1);
                if (rc)
                        GOTO(out_free, rc);

                if (lcfg->lcfg_bufcount < 1)
                        GOTO(out_free, rc = -EINVAL);

                rc = mgs_setparam(obd, lcfg, fsname);
                if (rc) {
                        CERROR("setparam err %d\n", rc);
                        GOTO(out_free, rc);
                }

                /* Revoke lock so everyone updates.  Should be alright if
                   someone was already reading while we were updating the logs,
                   so we don't really need to hold the lock while we're
                   writing (above). */
                if (fsname[0]) {
                        lockrc = mgs_get_cfg_lock(obd, fsname, &lockh);
                        if (lockrc != ELDLM_OK)
                                CERROR("lock error %d for fs %s\n", lockrc,
                                       fsname);
                        else
                                mgs_put_cfg_lock(&lockh);
                }

out_free:
                OBD_FREE(lcfg, data->ioc_plen1);
                RETURN(rc);
        }

        case OBD_IOC_DUMP_LOG: {
                struct llog_ctxt *ctxt;
                ctxt = llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT);
                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = class_config_dump_llog(ctxt, data->ioc_inlbuf1, NULL);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                llog_ctxt_put(ctxt);

                RETURN(rc);
        }

        case OBD_IOC_LLOG_CHECK:
        case OBD_IOC_LLOG_INFO:
        case OBD_IOC_LLOG_PRINT: {
                struct llog_ctxt *ctxt;
                ctxt = llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT);

                push_ctxt(&saved, &ctxt->loc_exp->exp_obd->obd_lvfs_ctxt, NULL);
                rc = llog_ioctl(ctxt, cmd, data);
                pop_ctxt(&saved, &ctxt->loc_exp->exp_obd->obd_lvfs_ctxt, NULL);
                llog_ctxt_put(ctxt);

                RETURN(rc);
        }

        default:
                CDEBUG(D_INFO, "unknown command %x\n", cmd);
                RETURN(-EINVAL);
        }
        RETURN(0);
}

/* use obd ops to offer management infrastructure */
static struct obd_ops mgs_obd_ops = {
        .o_owner           = THIS_MODULE,
        .o_connect         = mgs_connect,
        .o_disconnect      = mgs_disconnect,
        .o_setup           = mgs_setup,
        .o_precleanup      = mgs_precleanup,
        .o_cleanup         = mgs_cleanup,
        .o_destroy_export  = mgs_destroy_export,
        .o_iocontrol       = mgs_iocontrol,
        .o_llog_init       = mgs_llog_init,
        .o_llog_finish     = mgs_llog_finish
};

static int __init mgs_init(void)
{
        struct lprocfs_static_vars lvars;

        lprocfs_mgs_init_vars(&lvars);
        class_register_type(&mgs_obd_ops, NULL,
                            lvars.module_vars, LUSTRE_MGS_NAME, NULL);

        return 0;
}

static void /*__exit*/ mgs_exit(void)
{
        class_unregister_type(LUSTRE_MGS_NAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre  Management Server (MGS)");
MODULE_LICENSE("GPL");

module_init(mgs_init);
module_exit(mgs_exit);
