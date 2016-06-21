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

#include "virobject.h"
#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "lxc_domain.h"
#include "lxc_driver.h"
#include "lxc_criu.h"
#if WITH_CRIU
# include "criu/criu.h"
#endif

#define VIR_FROM_THIS VIR_FROM_LXC

VIR_LOG_INIT("lxc.lxc_criu");

#if WITH_CRIU
int lxcCriuDump(virLXCDriverPtr driver ATTRIBUTE_UNUSED,
                virDomainObjPtr vm,
                const char *checkpointdir)
{
    int fd;
    int ret = -1;
    virLXCDomainObjPrivatePtr priv;
    criu_opts *opts = NULL;

    if (virFileMakePath(checkpointdir) < 0) {
        virReportSystemError(errno,
                             _("Failed to mkdir %s"), checkpointdir);
        return -1;
    }

    fd = open(checkpointdir, O_DIRECTORY);
    if (fd < 0) {
        virReportSystemError(errno,
                             _("Failed to open directory %s"), checkpointdir);
        return -1;
    }

    if (criu_local_init_opts(&opts) == -1)
        goto cleanup;

    criu_local_set_images_dir_fd(opts, fd);
    criu_local_set_log_file(opts, (char*)"dump.log");
    criu_local_set_log_level(opts, 4);

    priv = vm->privateData;
    criu_local_set_pid(opts, priv->initpid);
    criu_local_set_tcp_established(opts, true);
    criu_local_set_file_locks(opts, true);
    criu_local_set_link_remap(opts, true);
    criu_local_set_force_irmap(opts, true);
    /* Ignore cgroups, they are going to be be managed
     * by libvirt */
    criu_local_set_manage_cgroups(opts, false);
    criu_local_set_auto_ext_mnt(opts, true);
    criu_local_add_enable_fs(opts, (char*)"hugetlbfs");
    criu_local_add_enable_fs(opts, (char*)"tracefs");
    /* criu_local_set_shell_job(opts, true);*/
    criu_local_set_leave_running(opts, false);

    /* hax for ignoring ttys*/
    criu_local_add_skip_mnt(opts, (char*)"/dev/console");
    criu_local_add_skip_mnt(opts, (char*)"/dev/tty1");
    criu_local_add_skip_mnt(opts, (char*)"/dev/ptmx");
    criu_local_add_skip_mnt(opts, (char*)"/dev/pts");

    VIR_DEBUG("About to checkpoint vm: %s pid=%d", vm->def->name, priv->initpid);
    if (criu_local_dump(opts) < 0)
        return -1;

    ret = 0;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    return ret;
}

int lxcCriuRestore(virDomainDefPtr def, int fd)
{
    int ret = -1;
    virDomainFSDefPtr root;
    criu_opts *opts = NULL;

    root = virDomainGetFilesystemForTarget(def, "/");

    criu_local_init_opts(&opts);

    criu_local_set_images_dir_fd(opts, fd);
    criu_local_set_log_file(opts, (char*)"restore.log");
    criu_local_set_log_level(opts, 4);

    criu_local_set_auto_ext_mnt(opts, true);
    criu_local_set_ext_sharing(opts, true);
    criu_local_set_ext_masters(opts, true);
    criu_local_set_file_locks(opts, true);
    /* criu_local_set_shell_job(opts, true);*/
    /* Do not restore cgroup properties but require cgroup to
     * pre-exist at the moment of restore procedure*/
    criu_local_set_manage_cgroups(opts, true);
    criu_local_set_manage_cgroups_mode(opts, CRIU_CG_MODE_NONE);
    /* Change the root filesystem (when run in mount namespace) */
    criu_local_set_root(opts, root->src);
    ret = criu_local_restore(opts);

    return ret;
}
#else
int lxcCriuDump(virLXCDriverPtr driver ATTRIBUTE_UNUSED,
                virDomainObjPtr vm ATTRIBUTE_UNUSED,
                const char *checkpointdir ATTRIBUTE_UNUSED)
{
    virReportUnsupportedError();
    return -1;
}

int lxcCriuRestore(virDomainDefPtr def ATTRIBUTE_UNUSED,
                   int fd ATTRIBUTE_UNUSED)
{
    virReportUnsupportedError();
    return -1;
}
#endif
