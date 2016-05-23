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
    virCommandPtr cmd;
    char log[] = "dump.log";
    char hugetlbfs[] = "hugetlbfs";
    char tracefs[] = "tracefs";

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

    criu_init_opts();
    criu_set_images_dir_fd(fd);

    criu_set_log_file(log);
    criu_set_log_level(4);

    priv = vm->privateData;
    criu_set_pid(priv->initpid);
    criu_set_tcp_established(true);
    criu_set_file_locks(true);
    criu_set_link_remap(true);
    criu_set_force_irmap(true);
    criu_set_manage_cgroups(true);
    criu_set_auto_ext_mnt(true);
    criu_set_ext_sharing(true);
    criu_set_ext_masters(true);
    criu_add_enable_fs(hugetlbfs);
    criu_add_enable_fs(tracefs);
    criu_set_leave_running(false);

    VIR_DEBUG("About to checkpoint vm: %s pid=%d", vm->def->name, priv->initpid);
    if (criu_dump() < 0)
        return -1;

    cmd = virCommandNewArgList("tar", "-zcvf", "checkpoint.tar.gz",
                               "checkpointdir", NULL);
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;
    ret = 0;

 cleanup:
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

    root = virDomainGetFilesystemForTarget(vm->def, "/");

    cmd = virCommandNewArgList("tar", "-zxvf", "checkpoint.tar.gz", NULL);
    if (virCommandRun(cmd, NULL) < 0) {
       virReportError(VIR_ERR_INTERNAL_ERROR,
                      "%s", _("Can't untar checkpoint data"));
        virCommandFree(cmd);
        return -1;
    }

    criu_init_opts();
    fd = open("./checkpointdir", O_DIRECTORY);
    if (fd < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Can't open images dir"));
        goto cleanup;
    }

    criu_set_images_dir_fd(fd);
    criu_set_log_file(log);
    criu_set_log_level(4);

    /* Change the root filesystem (when run in mount namespace) */
    criu_set_root(root->src);
    ret = criu_restore();

 cleanup:
    virCommandFree(cmd);
    return ret;
}
