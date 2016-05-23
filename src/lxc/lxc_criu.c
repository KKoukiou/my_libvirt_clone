/*
 * lxc_criu.c: wrapper functions for CRIU C API to be used for lxc migration
 *
 * Copyright (C) 2016 Katerina Koukiou
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Katerina Koukiou <k.koukiou@gmail.com>
 */

#include <config.h>

#include <fcntl.h>
#include <sys/stat.h>

#include "vircommand.h"
#include "virobject.h"
#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "lxc_domain.h"
#include "lxc_driver.h"
#include "lxc_criu.h"
#include "criu/criu.h"

#define VIR_FROM_THIS VIR_FROM_LXC

VIR_LOG_INIT("lxc.lxc_criu");

int lxcCriuCheck(void)
{
    return criu_check();
}


int lxcCriuDump(virLXCDriverPtr driver ATTRIBUTE_UNUSED,
                virDomainObjPtr vm)
{
    int fd;
    int ret = -1;
    virLXCDomainObjPrivatePtr priv;
    virCommandPtr cmd = NULL;
    char log[] = "dump.log";
    char hugetlbfs[] = "hugetlbfs";
    char tracefs[] = "tracefs";
    criu_opts *opts;

    if (mkdir("checkpointdir", 0777) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Can't create checkpoint directory"));
        return -1;
    }

    fd = open("checkpointdir", O_DIRECTORY);
    if (fd < 0) {
        virReportSystemError(errno, "%s",
                                   _("Failed to open directory"));
        return -1;
    }

    if (criu_local_init_opts(&opts) == -1)
        goto cleanup;

    criu_local_set_images_dir_fd(opts, fd);

    criu_local_set_log_file(opts, log);
    criu_local_set_log_level(opts, 4);

    priv = vm->privateData;
    criu_local_set_pid(opts, priv->initpid);
    criu_local_set_tcp_established(opts, true);
    criu_local_set_file_locks(opts, true);
    criu_local_set_link_remap(opts, true);
    criu_local_set_force_irmap(opts, true);
    criu_local_set_manage_cgroups(opts, true);
    criu_local_set_auto_ext_mnt(opts, true);
    criu_local_add_enable_fs(opts, hugetlbfs);
    criu_local_add_enable_fs(opts, tracefs);
    criu_local_set_leave_running(opts, false);

    VIR_DEBUG("About to checkpoint vm: %s pid=%d", vm->def->name, priv->initpid);
    if (criu_local_dump(opts) < 0)
        return -1;

    cmd = virCommandNewArgList("tar", "-zcvf", "checkpoint.tar.gz",
                               "checkpointdir", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;
    ret = 0;

 cleanup:
    /*Will do files cleanup later when we know that data have been tranfered to dst*/
    VIR_FORCE_CLOSE(fd);
    virCommandFree(cmd);
    return ret;
}

int lxcCriuRestore(virLXCDriverPtr driver ATTRIBUTE_UNUSED,
                   virDomainObjPtr vm)
{
    int fd;
    int ret = -1;
    virCommandPtr cmd;
    char log[] = "restore.log";
    virDomainFSDefPtr root;
    criu_opts *opts;

    root = virDomainGetFilesystemForTarget(vm->def, "/");

    cmd = virCommandNewArgList("tar", "-zxvf", "checkpoint.tar.gz", NULL);
    if (virCommandRun(cmd, NULL) < 0) {
       virReportError(VIR_ERR_INTERNAL_ERROR,
                      "%s", _("Can't untar checkpoint data"));
        virCommandFree(cmd);
        return -1;
    }

    cmd = virCommandNewArgList("rm", "-f", "checkpoint.tar.gz", NULL);
    if (virCommandRun(cmd, NULL) < 0) {
       virReportError(VIR_ERR_INTERNAL_ERROR,
                      "%s", _("Can't delete checkpoint tar file"));
        virCommandFree(cmd);
        return -1;
    }

    criu_local_init_opts(&opts);

    fd = open("./checkpointdir", O_DIRECTORY);
    if (fd < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Can't open images dir"));
        goto cleanup;
    }

    criu_local_set_images_dir_fd(opts, fd);
    criu_local_set_log_file(opts, log);
    criu_local_set_log_level(opts, 4);
    criu_local_set_auto_ext_mnt(opts, true);
    criu_local_set_ext_sharing(opts, true);
    criu_local_set_ext_masters(opts, true);


    /* Change the root filesystem (when run in mount namespace) */
    criu_local_set_root(opts, root->src);
    ret = criu_local_restore(opts);

 cleanup:
    virCommandFree(cmd);
    return ret;
}
