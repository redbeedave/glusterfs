/*
  Copyright (c) 2011 Gluster, Inc. <http://www.gluster.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "common-utils.h"
#include "cli1-xdr.h"
#include "xdr-generic.h"
#include "glusterd.h"
#include "glusterd-op-sm.h"
#include "glusterd-store.h"
#include "glusterd-utils.h"
#include "glusterd-volgen.h"

#include <signal.h>

int
glusterd_handle_log_rotate (rpcsvc_request_t *req)
{
        int32_t                 ret     = -1;
        gf_cli_req              cli_req = {{0,}};
        dict_t                 *dict    = NULL;
        glusterd_op_t           cli_op = GD_OP_LOG_ROTATE;
        char                   *volname = NULL;

        GF_ASSERT (req);

        if (!xdr_to_generic (req->msg[0], &cli_req,
                             (xdrproc_t)xdr_gf_cli_req)) {
                //failed to decode msg;
                req->rpc_err = GARBAGE_ARGS;
                goto out;
        }

        if (cli_req.dict.dict_len) {
                /* Unserialize the dictionary */
                dict  = dict_new ();

                ret = dict_unserialize (cli_req.dict.dict_val,
                                        cli_req.dict.dict_len,
                                        &dict);
                if (ret < 0) {
                        gf_log ("glusterd", GF_LOG_ERROR,
                                "failed to "
                                "unserialize req-buffer to dictionary");
                        goto out;
                }
        }

        ret = dict_get_str (dict, "volname", &volname);
        if (ret) {
                gf_log (THIS->name, GF_LOG_ERROR, "failed to get volname");
                goto out;
        }

        gf_log ("glusterd", GF_LOG_INFO, "Received log rotate req "
                "for volume %s", volname);

        ret = dict_set_uint64 (dict, "rotate-key", (uint64_t)time (NULL));
        if (ret)
                goto out;

        ret = glusterd_op_begin (req, GD_OP_LOG_ROTATE, dict);

out:
        if (ret && dict)
                dict_unref (dict);

        glusterd_friend_sm ();
        glusterd_op_sm ();

        if (ret)
                ret = glusterd_op_send_cli_response (cli_op, ret, 0, req,
                                                     NULL, "operation failed");

        if (cli_req.dict.dict_val)
                free (cli_req.dict.dict_val);
        return ret;
}

/* op-sm */
int
glusterd_op_stage_log_rotate (dict_t *dict, char **op_errstr)
{
        int                                     ret = -1;
        char                                    *volname = NULL;
        glusterd_volinfo_t                      *volinfo = NULL;
        gf_boolean_t                            exists = _gf_false;
        char                                    msg[2048] = {0};
        char                                    *brick = NULL;

        ret = dict_get_str (dict, "volname", &volname);
        if (ret) {
                gf_log ("", GF_LOG_ERROR, "Unable to get volume name");
                goto out;
        }

        exists = glusterd_check_volume_exists (volname);
        ret = glusterd_volinfo_find (volname, &volinfo);
        if (!exists) {
                snprintf (msg, sizeof (msg), "Volume %s does not exist",
                          volname);
                gf_log ("", GF_LOG_ERROR, "%s", msg);
                *op_errstr = gf_strdup (msg);
                ret = -1;
                goto out;
        }

        if (_gf_false == glusterd_is_volume_started (volinfo)) {
                snprintf (msg, sizeof (msg), "Volume %s needs to be started before"
                          " log rotate.", volname);
                gf_log ("", GF_LOG_ERROR, "%s", msg);
                *op_errstr = gf_strdup (msg);
                ret = -1;
                goto out;
        }

        ret = dict_get_str (dict, "brick", &brick);
        if (ret)
                goto out;

        if (strchr (brick, ':')) {
                ret = glusterd_volume_brickinfo_get_by_brick (brick, volinfo, NULL,
                                                              GF_PATH_COMPLETE);
                if (ret) {
                        snprintf (msg, sizeof (msg), "Incorrect brick %s "
                                  "for volume %s", brick, volname);
                        gf_log ("", GF_LOG_ERROR, "%s", msg);
                        *op_errstr = gf_strdup (msg);
                        goto out;
                }
        }
out:
        gf_log ("", GF_LOG_DEBUG, "Returning %d", ret);

        return ret;
}


int
glusterd_op_log_rotate (dict_t *dict)
{
        int                   ret                = -1;
        glusterd_conf_t      *priv               = NULL;
        glusterd_volinfo_t   *volinfo            = NULL;
        glusterd_brickinfo_t *brickinfo          = NULL;
        xlator_t             *this               = NULL;
        char                 *volname            = NULL;
        char                 *brick              = NULL;
        char                  path[PATH_MAX]     = {0,};
        char                  logfile[PATH_MAX]  = {0,};
        char                  pidfile[PATH_MAX]  = {0,};
        FILE                 *file               = NULL;
        pid_t                 pid                = 0;
        uint64_t              key                = 0;
        int                   valid_brick        = 0;
        glusterd_brickinfo_t *tmpbrkinfo         = NULL;

        this = THIS;
        GF_ASSERT (this);
        priv = this->private;
        GF_ASSERT (priv);

        ret = dict_get_str (dict, "volname", &volname);
        if (ret) {
                gf_log ("", GF_LOG_ERROR, "volname not found");
                goto out;
        }

        ret = dict_get_uint64 (dict, "rotate-key", &key);
        if (ret) {
                gf_log ("", GF_LOG_ERROR, "rotate key not found");
                goto out;
        }

        ret = dict_get_str (dict, "brick", &brick);
        if (ret)
                goto out;

        if (!strchr (brick, ':'))
                brick = NULL;
        else {
                ret = glusterd_brickinfo_from_brick (brick, &tmpbrkinfo);
                if (ret) {
                        gf_log ("glusterd", GF_LOG_ERROR,
                                "cannot get brickinfo from brick");
                        goto out;
                }
        }

        ret = glusterd_volinfo_find (volname, &volinfo);
        if (ret)
                goto out;

        ret = -1;
        list_for_each_entry (brickinfo, &volinfo->bricks, brick_list) {
                if (uuid_compare (brickinfo->uuid, priv->uuid))
                        continue;

                if (brick &&
                    (strcmp (tmpbrkinfo->hostname, brickinfo->hostname) ||
                     strcmp (tmpbrkinfo->path,brickinfo->path)))
                        continue;

                valid_brick = 1;

                GLUSTERD_GET_VOLUME_DIR (path, volinfo, priv);
                GLUSTERD_GET_BRICK_PIDFILE (pidfile, path, brickinfo->hostname,
                                            brickinfo->path);

                file = fopen (pidfile, "r+");
                if (!file) {
                        gf_log ("", GF_LOG_ERROR, "Unable to open pidfile: %s",
                                pidfile);
                        ret = -1;
                        goto out;
                }

                ret = fscanf (file, "%d", &pid);
                if (ret <= 0) {
                        gf_log ("", GF_LOG_ERROR, "Unable to read pidfile: %s",
                                pidfile);
                        ret = -1;
                        goto out;
                }
                fclose (file);
                file = NULL;

                snprintf (logfile, PATH_MAX, "%s.%"PRIu64,
                          brickinfo->logfile, key);

                ret = rename (brickinfo->logfile, logfile);
                if (ret)
                        gf_log ("", GF_LOG_WARNING, "rename failed");

                ret = kill (pid, SIGHUP);
                if (ret) {
                        gf_log ("", GF_LOG_ERROR, "Unable to SIGHUP to %d", pid);
                        goto out;
                }
                ret = 0;

                /* If request was for brick, only one iteration is enough */
                if (brick)
                        break;
        }

        if (ret && !valid_brick)
                ret = 0;

out:
        if (tmpbrkinfo)
                glusterd_brickinfo_delete (tmpbrkinfo);

        return ret;
}