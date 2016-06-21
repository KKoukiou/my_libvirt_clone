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
#include <sys/mount.h>

#include "virobject.h"
#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "vircommand.h"
#include "virstring.h"
#include "viralloc.h"

#include "lxc_domain.h"
#include "lxc_driver.h"
#include "lxc_criu.h"

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
    virCommandPtr cmd;
    struct stat sb;
    char *path = NULL;
    char *tty_info_path = NULL;
    char *ttyinfo = NULL;
    int status;

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

    cmd = virCommandNew("criu");
    virCommandAddArg(cmd, "dump");

    virCommandAddArgList(cmd, "--images-dir", checkpointdir, NULL);

    virCommandAddArgList(cmd, "--log-file", "dump.log", NULL);

    virCommandAddArgList(cmd, "-vvvv", NULL);

    priv = vm->privateData;
    virCommandAddArg(cmd, "--tree");
    virCommandAddArgFormat(cmd, "%d", priv->initpid);

    virCommandAddArgList(cmd, "--tcp-established", "--file-locks",
                              "--link-remap", "--force-irmap", NULL);

    virCommandAddArgList(cmd, "--manage-cgroup", NULL);

    virCommandAddArgList(cmd, "--enable-external-sharing",
                              "--enable-external-masters", NULL);

    virCommandAddArgList(cmd, "--enable-fs", "hugetlbfs",
                              "--enable-fs", "tracefs", NULL);

    /* The ttys have their  one end in the checkpointed process set
     * and the other end in a separate process.
     * For this reason we should enumerate the external files on dump.
     */
    virCommandAddArgList(cmd, "--ext-mount-map", "/dev/console:console", NULL);
    virCommandAddArgList(cmd, "--ext-mount-map", "/dev/tty1:tty1", NULL);
    virCommandAddArgList(cmd, "--ext-mount-map", "auto", NULL);

    /* The master pair of the /dev/pts device lives outside from what is dumped
     * inside the libvirt-lxc process. Add the slave pair as an external tty
     * otherwise criu will fail.
     */
    if (virAsprintf(&path, "/proc/%d/root/dev/pts/0", priv->initpid) < 0)
        goto cleanup;

    if (stat(path, &sb) < 0) {
        virReportSystemError(errno,
                             _("Unable to stat %s"), path);
        goto cleanup;
    }

    if (virAsprintf(&tty_info_path, "%s/tty.info", checkpointdir) < 0)
        goto cleanup;

    if (virAsprintf(&ttyinfo, "tty[%x:%x]",
                   (unsigned int)sb.st_rdev, (unsigned int)sb.st_dev) < 0)
        goto cleanup;

    if (virFileWriteStr(tty_info_path, ttyinfo, 0666) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to write tty info to %s"), tty_info_path);
        goto cleanup;
    }

    VIR_DEBUG("tty.info: tty[%x:%x]",
             (unsigned int)sb.st_dev, (unsigned int)sb.st_rdev);
    virCommandAddArg(cmd, "--external");
    virCommandAddArgFormat(cmd, "tty[%x:%x]",
                          (unsigned int)sb.st_rdev, (unsigned int)sb.st_dev);

    virCommandAddArgList(cmd, "--ext-unix-sk", NULL);

    /*temporary hacks that should be FIXED*/
    virCommandAddArgList(cmd, "--skip-mnt", "/sys/kernel/security",
                              "--skip-mnt", "/run/user/1000", NULL);

    VIR_DEBUG("About to checkpoint domain %s (pid = %d)",
              vm->def->name, priv->initpid);
    virCommandRawStatus(cmd);
    if (virCommandRun(cmd, &status) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    VIR_FREE(path);
    VIR_FREE(tty_info_path);
    VIR_FREE(ttyinfo);

    return ret;
}

int lxcCriuRestore(virDomainDefPtr def, int restorefd,
                   int ttyfd)
{
    int ret = -1;
    virDomainFSDefPtr root;
    int status;
    virCommandPtr cmd;
    char *ttyinfo = NULL;
    char *inheritfd = NULL;
    char *tty_info_path = NULL;
    char *checkpointfd = NULL;
    char *checkpointdir = NULL;
    char *rootfs_mount = NULL;

    root = virDomainGetFilesystemForTarget(def, "/");

    cmd = virCommandNew("criu");
    virCommandAddArg(cmd, "restore");

    if (virAsprintf(&checkpointfd, "/proc/self/fd/%d", restorefd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to write checkpoint dir path"));
        goto cleanup;
    }

    if (virFileResolveLink(checkpointfd, &checkpointdir) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to readlink checkpoint dir path"));
        goto cleanup;
    }

    /* CRIU needs the container's root bind mounted so that it is the root of
     * some mount.
     */
    if (virAsprintf(&rootfs_mount, "/mnt/%s", def->name) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to write rootfs dir mount path"));
        goto cleanup;
    }

    if (virFileMakePath(rootfs_mount) < 0) {
         virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to mkdir rootfs mount path"));
        goto cleanup;
    }

    if (mount(root->src, rootfs_mount, NULL, MS_BIND, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to create rootfs mountpoint"));
        goto cleanup;
    }

    virCommandAddArgList(cmd, "--images-dir", checkpointdir, NULL);

    virCommandAddArgList(cmd, "--log-file", "restore.log", NULL);

    virCommandAddArgList(cmd, "-vvvv", NULL);
    virCommandAddArgList(cmd, "--tcp-established", "--file-locks",
                              "--link-remap", "--force-irmap", NULL);

    virCommandAddArgList(cmd, "--enable-external-sharing",
                              "--enable-external-masters", NULL);

    virCommandAddArgList(cmd, "--ext-mount-map", "auto", NULL);

    virCommandAddArgList(cmd, "--enable-fs", "hugetlbfs",
                              "--enable-fs", "tracefs", NULL);

    virCommandAddArgList(cmd, "--ext-mount-map", "console:/dev/console", NULL);
    virCommandAddArgList(cmd, "--ext-mount-map", "tty1:/dev/tty1", NULL);

    virCommandAddArgList(cmd, "--ext-unix-sk", NULL);

    /* Restore cgroup properties if only cgroup has been created by criu,
     * otherwise do not restore properies
     */
    virCommandAddArgList(cmd, "--manage-cgroup", "soft", NULL);

    virCommandAddArgList(cmd, "--restore-detached", "--restore-sibling", NULL);

    /* Restore external tty that was saved in tty.info file
     */
    if (virAsprintf(&tty_info_path, "%s/tty.info", checkpointdir) < 0)
        goto cleanup;

    if (virFileReadAll(tty_info_path, 1024, &ttyinfo) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to read tty info from %s"), tty_info_path);
        goto cleanup;
    }
    if (virAsprintf(&inheritfd, "fd[%d]:%s", ttyfd, ttyinfo) < 0)
        goto cleanup;

    virCommandAddArgList(cmd, "--inherit-fd", inheritfd, NULL);

    /* Change the root filesystem because we run  in mount namespace.
     */
    virCommandAddArgList(cmd, "--root", rootfs_mount, NULL);

    virCommandRawStatus(cmd);
    if (virCommandRun(cmd, &status) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_DEBUG("Restore process finished %s",
             (ret == 0) ? "successfully" : "with error");
    VIR_FORCE_CLOSE(restorefd);
    VIR_FREE(tty_info_path);
    VIR_FREE(ttyinfo);
    VIR_FREE(inheritfd);
    VIR_FREE(checkpointdir);
    VIR_FREE(checkpointfd);
    VIR_FREE(rootfs_mount);
    VIR_FREE(cmd);

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
                   int fd ATTRIBUTE_UNUSED,
                   int ttyfd ATTRIBUTE_UNUSED)
{
    virReportUnsupportedError();
    return -1;
}
#endif
