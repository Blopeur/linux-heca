/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/errno.h>

#include "hspace.h"
#include "hecatonchire.h"
#include "hutils.h"
#include "hproc.h"

#include "base.h"


#define HSPACE_KOBJECT          "%u"

#define to_hspace(s)            container_of(s, struct heca_space, kobj)
#define to_hspace_attr(sa)      container_of(sa, struct hspace_attr, attr)

/*
 * Heca Space  Kobject
 */

struct hspace_attr {
        struct attribute attr;
        ssize_t(*show)(struct heca_space *, char *);
        ssize_t(*store)(struct heca_space *, char *, size_t);
};

static void kobj_hspace_release(struct kobject *k)
{
        heca_printk(KERN_DEBUG, "Releasing kobject %p", k);
}

static ssize_t heca_space_show(struct kobject *k, struct attribute *a,
                char *buffer)
{
        struct heca_space *hspace = to_hspace(k);
        struct hspace_attr *hspace_attr = to_hspace_attr(a);
        if (hspace_attr->show)
                return hspace_attr->show(hspace,buffer);
        return 0;
}

static struct hspace_attr *hspace_attr[] = {
        NULL
};

static struct sysfs_ops heca_space_ops = {
        .show = heca_space_show,
};

static struct kobj_type ktype_hspace = {
        .release = kobj_hspace_release,
        .sysfs_ops = &heca_space_ops,
        .default_attrs = (struct attribute **) hspace_attr,
};


/* 
 * Main Hspace function 
 */

int deregister_hspace(__u32 hspace_id)
{
        struct heca_module_state *heca_state = get_heca_module_state();
        int ret = 0;
        struct heca_space *hspace;
        struct list_head *curr, *next;

        heca_printk(KERN_DEBUG "<enter> hspace_id=%d", hspace_id);
        list_for_each_safe (curr, next, &heca_state->hspaces_list) {
                hspace = list_entry(curr, struct heca_space, hspace_ptr);
                if (hspace->hspace_id == hspace_id)
                        remove_hspace(hspace);
        }

        destroy_hcm_listener(heca_state);
        heca_printk(KERN_DEBUG "<exit> %d", ret);
        return ret;
}

int register_hspace(struct hecaioc_hspace *hspace_info)
{
        struct heca_module_state *heca_state = get_heca_module_state();
        int rc;

        heca_printk(KERN_DEBUG "<enter>");

        if ((rc = create_hcm_listener(heca_state,
                                        hspace_info->local.sin_addr.s_addr,
                                        hspace_info->local.sin_port))) {
                heca_printk(KERN_ERR "create_hcm %d", rc);
                goto done;
        }

        if ((rc = create_hspace(hspace_info->hspace_id))) {
                heca_printk(KERN_ERR "create_hspace %d", rc);
                goto done;
        }

done:
        if (rc)
                deregister_hspace(hspace_info->hspace_id);
        heca_printk(KERN_DEBUG "<exit> %d", rc);
        return rc;
}

struct heca_space *find_hspace(u32 id)
{
        struct heca_module_state *heca_state = get_heca_module_state();
        struct heca_space *hspace;
        struct heca_space **hspacep;
        struct radix_tree_root *root;

        rcu_read_lock();
        root = &heca_state->hspaces_tree_root;
repeat:
        hspace = NULL;
        hspacep = (struct heca_space **) radix_tree_lookup_slot(root,
                        (unsigned long) id);
        if (hspacep) {
                hspace = radix_tree_deref_slot((void **) hspacep);
                if (unlikely(!hspace))
                        goto out;
                if (radix_tree_exception(hspace)) {
                        if (radix_tree_deref_retry(hspace))
                                goto repeat;
                }
        }
out:
        rcu_read_unlock();
        return hspace;
}

void remove_hspace(struct heca_space *hspace)
{
        struct heca_process *hproc;
        struct heca_module_state *heca_state = get_heca_module_state();
        struct list_head *pos, *n;

        BUG_ON(!hspace);

        heca_printk(KERN_DEBUG "<enter> hspace=%d", hspace->hspace_id);

        list_for_each_safe (pos, n, &hspace->hprocs_list) {
                hproc = list_entry(pos, struct heca_process, hproc_ptr);
                remove_hproc(hspace->hspace_id, hproc->hproc_id);
        }

        mutex_lock(&heca_state->heca_state_mutex);
        list_del(&hspace->hspace_ptr);
        radix_tree_delete(&heca_state->hspaces_tree_root,
                        (unsigned long) hspace->hspace_id);
        mutex_unlock(&heca_state->heca_state_mutex);
        synchronize_rcu();

        mutex_lock(&heca_state->heca_state_mutex);
        kfree(hspace);
        mutex_unlock(&heca_state->heca_state_mutex);

        heca_printk(KERN_DEBUG "<exit>");
}


int create_hspace(__u32 hspace_id)
{
        int r = 0;
        struct heca_space *found_hspace, *new_hspace = NULL;
        struct heca_module_state *heca_state = get_heca_module_state();

        /* already exists? (first check; the next one is under lock */
        found_hspace = find_hspace(hspace_id);
        if (found_hspace) {
                heca_printk("we already have the hspace in place");
                return -EEXIST;
        }

        /* allocate a new hspace */
        new_hspace = kzalloc(sizeof(*new_hspace), GFP_KERNEL);
        if (!new_hspace) {
                heca_printk("can't allocate");
                return -ENOMEM;
        }
        new_hspace->hspace_id = hspace_id;
        mutex_init(&new_hspace->hspace_mutex);
        INIT_RADIX_TREE(&new_hspace->hprocs_tree_root,
                        GFP_KERNEL & ~__GFP_WAIT);
        INIT_RADIX_TREE(&new_hspace->hprocs_mm_tree_root,
                        GFP_KERNEL & ~__GFP_WAIT);
        INIT_LIST_HEAD(&new_hspace->hprocs_list);
        new_hspace->nb_local_hprocs = 0;

        while (1) {
                r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
                if (!r)
                        break;

                if (r == -ENOMEM) {
                        heca_printk("radix_tree_preload: ENOMEM retrying ...");
                        mdelay(2);
                        continue;
                }

                heca_printk("radix_tree_preload: failed %d", r);
                goto failed;
        }

        spin_lock(&heca_state->radix_lock);
        r = radix_tree_insert(&heca_state->hspaces_tree_root,
                        (unsigned long) new_hspace->hspace_id, new_hspace);
        spin_unlock(&heca_state->radix_lock);
        radix_tree_preload_end();

        if (r) {
                heca_printk("radix_tree_insert: failed %d", r);
                goto failed;
        }

        list_add(&new_hspace->hspace_ptr, &heca_state->hspaces_list);
        heca_printk("registered hspace %p, hspace_id : %u, res: %d",
                        new_hspace, hspace_id, r);
        return r;

failed:
        kfree(new_hspace);
        return r;
}
