/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 * Steve Walsh <steve.walsh@sap.com> 2012 (c)
 */

#include <linux/heca.h>


#include "ioctl.h"
#include "hutils.h"
#include "hspace.h"
#include "hproc.h"
#include "mr.h"

#include "task.h"
#include "base.h"




static int ioctl_hproc(int ioctl, void __user *argp)
{
        struct hecaioc_hproc hproc_info;

        if (copy_from_user((void *) &hproc_info, argp, sizeof hproc_info)) {
                heca_printk(KERN_ERR "copy_from_user failed");
                return -EFAULT;
        }

        if (!hproc_info.pid) {
                hproc_info.pid = get_current_pid();
                heca_printk(KERN_INFO "no pid defined assuming %d",
                                hproc_info.pid);
        }

        switch (ioctl) {
        case HECAIOC_HPROC_ADD:
                return create_hproc(&hproc_info);
        case HECAIOC_HPROC_RM:
                teardown_hproc_by_id(hproc_info.hspace_id, hproc_info.hproc_id);
                return 0;
        }
        return -EINVAL;
}

static int ioctl_mr(int ioctl, void __user *argp)
{
        struct hecaioc_hmr udata;

        if (copy_from_user((void *) &udata, argp, sizeof udata)) {
                heca_printk(KERN_ERR "copy_from_user failed");
                return -EFAULT;
        }

        /*FIXME add more check*/
        if(udata.hmr_id ==0)
                return -EINVAL;
        if(udata.sz == 0)
                return -EINVAL;

        switch (ioctl) {
        case HECAIOC_HMR_ADD:
                return create_heca_mr(&udata);
        }

        return -EINVAL;
}

static int ioctl_ps(int ioctl, void __user *argp)
{
        struct hecaioc_ps udata;

        if (copy_from_user((void *) &udata, argp, sizeof udata)) {
                heca_printk(KERN_ERR "copy_from_user failed");
                return -EFAULT;
        }

        if (!udata.pid) {
                udata.pid = get_current_pid();
                heca_printk(KERN_INFO "no pid defined assuming %d", udata.pid);
        }

        switch (ioctl) {
        case HECAIOC_PS_PUSHBACK:
                return pushback_ps(&udata);
        case HECAIOC_PS_UNMAP:
                return unmap_ps(&udata);
        }

        return -EINVAL;
}

static long ioctl_hspace(unsigned int ioctl, void __user *argp)
{
        struct hecaioc_hspace hspace_info;
        int rc = -EFAULT;

        if ((rc = copy_from_user((void *) &hspace_info, argp,
                                        sizeof hspace_info))) {
                heca_printk(KERN_ERR "copy_from_user %d", rc);
                goto failed;
        }

        switch (ioctl) {
        case HECAIOC_HSPACE_ADD:
                heca_printk(KERN_INFO "registering hspace");
                return register_hspace(&hspace_info);
        case HECAIOC_HSPACE_RM:
                return deregister_hspace(hspace_info.hspace_id);
        default:
                goto failed;
        }

failed:
        return rc;
}

long heca_ioctl(struct file *f, unsigned int ioctl, unsigned long arg)
{
        void __user *argp = (void __user *) arg;
        int r = -EINVAL;

        heca_printk(KERN_DEBUG "<enter> ioctl=0x%X", ioctl);

        /* special case: no need for prior hspace in process */
        switch (ioctl) {
        case HECAIOC_HSPACE_ADD:
        case HECAIOC_HSPACE_RM:
                r = ioctl_hspace(ioctl, argp);
                goto out;
        case HECAIOC_HPROC_ADD:
        case HECAIOC_HPROC_RM:
                r = ioctl_hproc(ioctl, argp);
                goto out;
        case HECAIOC_HMR_ADD:
                r = ioctl_mr(ioctl, argp);
                goto out;
        case HECAIOC_PS_PUSHBACK:
        case HECAIOC_PS_UNMAP:
                r = ioctl_ps(ioctl, argp);
                goto out;
        default:
                heca_printk(KERN_ERR "ioctl 0x%X not supported", ioctl);
        }

out:
        heca_printk(KERN_DEBUG "<exit> ioctl=0x%X: %d", ioctl, r);
        return r;
}
