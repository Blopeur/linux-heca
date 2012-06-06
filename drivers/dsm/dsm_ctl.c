/*
 * Benoit Hudzia <benoit.hudzia@sap.com>
 * Aidan Shribman <aidan.shribman@sap.com>
 */

#include <linux/list.h>
#include <linux/delay.h>
#include <dsm/dsm_module.h>

static char *ip = 0;
static int port = 0;

static inline void reset_page_stats(struct dsm_page_stats *stats)
{
    dsm_stats_set(&stats->nb_page_pull, 0);
    dsm_stats_set(&stats->nb_page_pull_fail, 0);
    dsm_stats_set(&stats->nb_page_push_request, 0);
    dsm_stats_set(&stats->nb_page_requested, 0);
    dsm_stats_set(&stats->nb_page_sent, 0);
    dsm_stats_set(&stats->nb_page_redirect, 0);
    dsm_stats_set(&stats->nb_err, 0);
    dsm_stats_set(&stats->nb_page_request_success, 0);
    dsm_stats_set(&stats->nb_page_requested_prefetch, 0);
}

static inline void reset_msg_stats(struct msg_stats *stats)
{
    dsm_stats_set(&stats->err, 0);
    dsm_stats_set(&stats->page_info_update, 0);
    dsm_stats_set(&stats->page_request_reply, 0);
    dsm_stats_set(&stats->request_page, 0);
    dsm_stats_set(&stats->request_page_pull, 0);
    dsm_stats_set(&stats->try_request_page, 0);
    dsm_stats_set(&stats->try_request_page_fail, 0);
    dsm_stats_set(&stats->page_request_redirect, 0);
}

void reset_dsm_connection_stats(struct con_element_sysfs *sysfs) {
    reset_msg_stats(&sysfs->rx_stats);
    reset_msg_stats(&sysfs->tx_stats);
}

void remove_svm(u32 dsm_id, u32 svm_id)
{
    struct dsm_module_state *dsm_state = get_dsm_module_state();
    struct dsm *dsm;
    struct subvirtual_machine *svm = NULL;

    mutex_lock(&dsm_state->dsm_state_mutex);
    dsm = find_dsm(dsm_id);
    if (dsm) {
        mutex_lock(&dsm->dsm_mutex);
        svm = find_svm(dsm, svm_id);
        if (!svm) {
            mutex_unlock(&dsm_state->dsm_state_mutex);
            goto out;
        }
    }
    if (svm->priv) {
        radix_tree_delete(&get_dsm_module_state()->mm_tree_root,
                (unsigned long) svm->priv->mm);
    }
    mutex_unlock(&dsm_state->dsm_state_mutex);
    if (!dsm)
	return;

    atomic_set(&svm->status, DSM_SVM_OFFLINE);

    list_del(&svm->svm_ptr);
    radix_tree_delete(&dsm->svm_tree_root, (unsigned long) svm->svm_id);
    if (svm->priv) {
        dsm->nb_local_svm--;
        radix_tree_delete(&dsm->svm_mm_tree_root,
                (unsigned long) svm->priv->mm);
    }

    release_svm_from_mr_descriptors(svm);
    if (svm->priv) {
	struct rb_root *root;
	struct rb_node *node;

	BUG_ON(!dsm_state->rcm);
        root = &dsm_state->rcm->root_conn;
        for (node = rb_first(root); node; node = rb_next(node)) {
	    struct conn_element *ele;

            ele = rb_entry(node, struct conn_element, rb_node);
	    BUG_ON(!ele);
            release_svm_tx_requests(svm, &ele->tx_buffer);
            release_svm_tx_elements(svm, ele);
        }
        release_push_elements(svm, NULL);
    } else if (svm->ele) {
	struct list_head *pos;

        release_svm_tx_requests(svm, &svm->ele->tx_buffer);
        release_svm_tx_elements(svm, svm->ele);
        list_for_each (pos, &svm->dsm->svm_list) {
	    struct subvirtual_machine *local_svm;

            local_svm = list_entry(pos, struct subvirtual_machine, svm_ptr);
	    BUG_ON(!local_svm);
            if (!local_svm->priv)
                continue;
            release_push_elements(local_svm, svm);
        }
    }

    synchronize_rcu();
    delete_svm_sysfs_entry(&svm->svm_sysfs.svm_kobject);

    kfree(svm);

out:
    mutex_unlock(&dsm->dsm_mutex);
}

void remove_dsm(struct dsm *dsm) {
    struct subvirtual_machine *svm;
    struct dsm_module_state *dsm_state = get_dsm_module_state();
    struct list_head *pos, *n;

    dsm_printk("removing dsm %d", dsm->dsm_id);

    list_for_each_safe (pos, n, &dsm->svm_list) {
        svm = list_entry(pos, struct subvirtual_machine, svm_ptr);
        remove_svm(dsm->dsm_id, svm->svm_id);
    }

    mutex_lock(&dsm_state->dsm_state_mutex);
    list_del(&dsm->dsm_ptr);
    radix_tree_delete(&dsm_state->dsm_tree_root, (unsigned long) dsm->dsm_id);
    mutex_unlock(&dsm_state->dsm_state_mutex);
    synchronize_rcu();

    destroy_mrs(dsm, 1);
    delete_dsm_sysfs_entry(&dsm->dsm_kobject);

    mutex_lock(&dsm_state->dsm_state_mutex);
    kfree(dsm);
    mutex_unlock(&dsm_state->dsm_state_mutex);
}

/* FIXME: just a dummy lock so that radix_tree functions work */
DEFINE_SPINLOCK(dsm_lock); 

static int register_dsm(struct private_data *priv_data, void __user *argp)
{
    int r = 0;
    struct svm_data svm_info;
    struct dsm *found_dsm, *new_dsm = NULL;
    struct dsm_module_state *dsm_state = get_dsm_module_state();

    if (copy_from_user((void *) &svm_info, argp, sizeof svm_info)) {
        dsm_printk("reading data from userspace failed");
        return -EFAULT;
    }

    /* if not exists allocate a new dsm */
    new_dsm = kzalloc(sizeof(*new_dsm), GFP_KERNEL);
    if (!new_dsm) {
        dsm_printk("can't allocate");
        return -ENOMEM;
    }
    new_dsm->dsm_id = svm_info.dsm_id;
    mutex_init(&new_dsm->dsm_mutex);
    seqlock_init(&new_dsm->mr_seq_lock);
    INIT_RADIX_TREE(&new_dsm->svm_tree_root, GFP_KERNEL & ~__GFP_WAIT);
    INIT_RADIX_TREE(&new_dsm->svm_mm_tree_root, GFP_KERNEL & ~__GFP_WAIT);
    INIT_LIST_HEAD(&new_dsm->svm_list);
    new_dsm->mr_tree_root = RB_ROOT;
    new_dsm->nb_local_svm = 0;
 
    /* check if dsm already exists */
    mutex_lock(&dsm_state->dsm_state_mutex);
    found_dsm = find_dsm(svm_info.dsm_id);
    if (!found_dsm) {
#if 0
        mutex_lock(&new_dsm->dsm_mutex);
#endif
    }
    mutex_unlock(&dsm_state->dsm_state_mutex);

    if (found_dsm) {
        dsm_printk("we already have the dsm in place");
        r = -EEXIST;
        goto failed;
    }

    while (1) {
        r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
        if (r == -ENOMEM) {
            dsm_printk("radix_tree_preload: ENOMEM retrying ...");
            mdelay(2);
            continue;
        }

        if (r) {
            dsm_printk("radix_tree_preload: failed %d", r);
            goto failed;
        }
        break;
    }

    spin_lock(&dsm_lock);
    r = radix_tree_insert(&dsm_state->dsm_tree_root,
		    (unsigned long) new_dsm->dsm_id, new_dsm);
    spin_unlock(&dsm_lock);

    radix_tree_preload_end();

    if (r) {
        dsm_printk("radix_tree_insert: failed %d", r);
        goto err_delete;
    }

    if (create_dsm_sysfs_entry(new_dsm, dsm_state)) {
        dsm_printk("create_dsm_sysfs_entry: failed %d", r);
        goto err_delete;
    }

    priv_data->dsm = new_dsm;
    list_add(&new_dsm->dsm_ptr, &dsm_state->dsm_list);
    dsm_printk("registered dsm %p,  dsm_id : %u, res: %d \n",
            new_dsm, svm_info.dsm_id, r);
    new_dsm = NULL;
    goto done;

err_delete:
    radix_tree_delete(&dsm_state->dsm_tree_root,
            (unsigned long) svm_info.dsm_id);
done:
#if 0
    mutex_unlock(&new_dsm->dsm_mutex);
#endif
failed:
    if (new_dsm)
        kfree(new_dsm);
    return r;
}

static int is_local(struct svm_data *svm_info)
{
    return !svm_info->offset;
}

static int register_svm(struct private_data *priv_data, void __user *argp)
{
    struct dsm_module_state *dsm_state = get_dsm_module_state();
    int r = 0;
    struct dsm *dsm;
    struct subvirtual_machine *found_svm, *new_svm = NULL;
    struct svm_data svm_info;

    if (copy_from_user((void *) &svm_info, argp, sizeof svm_info)) {
        dsm_printk("failed to copy from user");
        return -EFAULT;
    }

    mutex_lock(&dsm_state->dsm_state_mutex);
    dsm = find_dsm(svm_info.dsm_id);
    if (dsm) {
#if 0
        mutex_lock(&dsm->dsm_mutex);
#endif
    }
    mutex_unlock(&dsm_state->dsm_state_mutex);
    if (!dsm) {
        dsm_printk(KERN_ERR "could not find dsm: %d", svm_info.dsm_id);
        r = -EFAULT;
        goto do_unlock;
    }

    found_svm = find_svm(dsm, svm_info.svm_id);
    if (found_svm) {
        dsm_printk(KERN_ERR "svm %d (dsm %d) already exists",
            svm_info.svm_id, svm_info.dsm_id);
        r = -EEXIST;
        goto do_unlock;
    }

    /* allocate a new svm */
    new_svm = kzalloc(sizeof(*new_svm), GFP_KERNEL);
    if (!new_svm) {
        dsm_printk(KERN_ERR "failed kzalloc");
        r = -ENOMEM;
        goto do_unlock;
    }

    new_svm->svm_id = svm_info.svm_id;
    new_svm->dsm = dsm;
    if (is_local(&svm_info)) {
        new_svm->priv = priv_data;
        new_svm->priv->svm = new_svm;
        new_svm->priv->offset = svm_info.offset;
        new_svm->dsm->nb_local_svm++;
    } else {
        u32 svm_id[] = {new_svm->svm_id, 0};
        new_svm->descriptor = dsm_get_descriptor(dsm, svm_id);
    }
    spin_lock_init(&new_svm->page_cache_spinlock);
    INIT_RADIX_TREE(&new_svm->page_cache, GFP_ATOMIC);
    new_svm->push_cache = RB_ROOT;
    seqlock_init(&new_svm->push_cache_lock);
    INIT_LIST_HEAD(&new_svm->mr_list);

    /* register new svm to radix tree */
    while (1) {
        r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
        if (r == -ENOMEM) {
            dsm_printk(KERN_ERR "radix_tree_preload: ENOMEM retrying ...");
            mdelay(2);
            continue;
        }

        if (r) {
            dsm_printk(KERN_ERR "radix_tree_preload: failed %d", r);
            goto do_unlock;
        }
        break;
    }

    spin_lock(&dsm_lock);
    r = radix_tree_insert(&dsm->svm_tree_root,
            (unsigned long) new_svm->svm_id, new_svm);
    spin_unlock(&dsm_lock);

    if (r) {
        dsm_printk(KERN_ERR "failed radix_tree_insert (svm) %d", r);
        goto err_preload_end;
    }

    if (!is_local(&svm_info))
        goto err_preload_end;

    spin_lock(&dsm_lock);
    r = radix_tree_insert(&dsm->svm_mm_tree_root,
            (unsigned long) new_svm->priv->mm, new_svm);
    spin_unlock(&dsm_lock);

    if (r) {
        dsm_printk(KERN_ERR "failed radix_tree_insert (svm_mm) %d", r);
        goto err_preload_end;
    }

    spin_lock(&dsm_lock);
    r = radix_tree_insert(&dsm_state->mm_tree_root,
            (unsigned long) new_svm->priv->mm, new_svm);
    spin_unlock(&dsm_lock);

    if (r) {
        dsm_printk(KERN_ERR "failed radix_tree_insert (mm) %d", r);
        goto err_preload_end;
    }

err_preload_end:
    radix_tree_preload_end();
    if (r)
        goto err_delete;

    /* update state */
    atomic_set(&new_svm->status, DSM_SVM_ONLINE);
    reset_page_stats(&new_svm->svm_sysfs.stats);
    r = create_svm_sysfs_entry(new_svm);
    if (r) {
        dsm_printk(KERN_ERR "failed create_svm_sysfs_entry %d", r);
        goto err_sysfs;
    }
    list_add(&new_svm->svm_ptr, &dsm->svm_list);
    dsm_printk(KERN_INFO "svm %p, res %d, dsm_id %u, svm_id: %u\n",
            new_svm, r, svm_info.dsm_id, svm_info.svm_id);
    new_svm = NULL;
    goto do_unlock;

err_sysfs:
    delete_svm_sysfs_entry(&new_svm->svm_sysfs.svm_kobject);

err_delete:
    radix_tree_delete(&dsm->svm_tree_root, (unsigned long) new_svm->svm_id);
    if (!is_local(&svm_info))
        goto do_unlock;
    radix_tree_delete(&dsm->svm_mm_tree_root,
            (unsigned long) new_svm->priv->mm);
    radix_tree_delete(&dsm_state->mm_tree_root,
            (unsigned long) new_svm->priv->mm);

do_unlock:
#if 0
    mutex_unlock(&dsm->dsm_mutex);
#endif
    if (new_svm)
        kfree(new_svm);
    return r;
}

static int connect_svm(struct private_data *priv_data, void __user *argp)
{
    int r = 0;
    struct dsm *dsm;
    struct subvirtual_machine *svm;
    struct svm_data svm_info;
    struct conn_element *cele;
    int ip_addr;
    struct dsm_module_state *dsm_state = get_dsm_module_state();

    r = copy_from_user((void *) &svm_info, argp, sizeof svm_info);
    if (r) {
        dsm_printk(KERN_ERR "copy_from_user failed: %d", r);
        return -EFAULT;
    }

    dsm = find_dsm(svm_info.dsm_id);
    if (!dsm) {
        dsm_printk(KERN_ERR "can't find dsm %d", svm_info.dsm_id);
        return -EFAULT;
    }

    dsm_printk(KERN_ERR "connecting to dsm_id: %u [%p], svm_id: %u\n",
        svm_info.dsm_id, dsm, svm_info.svm_id);

#if 0
    mutex_lock(&dsm->dsm_mutex);
#endif
    svm = find_svm(dsm, svm_info.svm_id);
    if (!svm) {
        dsm_printk(KERN_ERR "Can't find svm %d", svm_info.svm_id);
        return -EFAULT;
    }

    ip_addr = inet_addr(svm_info.ip);

    cele = search_rb_conn(ip_addr);
    if (cele) {
        dsm_printk(KERN_ERR "has existing connection to %d", ip_addr);
        goto done;
    }

    r = create_connection(dsm_state->rcm, &svm_info);
    if (r) {
        dsm_printk(KERN_ERR "create_connection failed %d", r);
        goto failed;
    }

    might_sleep();
    cele = search_rb_conn(ip_addr);
    if (!cele) {
        dsm_printk(KERN_ERR "conneciton does not exist", r);
        r = -ENOLINK;
        goto failed;
    }

    wait_for_completion(&cele->completion);
    if (!atomic_read(&cele->alive)) {
        dsm_printk(KERN_ERR "conneciton is not alive ... abouting");
        r = -ENOLINK;
        goto failed;
    }
    goto done;

done:
    svm->ele = cele;
failed:
#if 0
    mutex_unlock(&dsm->dsm_mutex);
#endif
    dsm_printk(KERN_INFO "dsm %d svm %d svm_connect ip %d: %d",
        svm_info.dsm_id, svm_info.svm_id, ip_addr, r);
    return r;
}

static int register_mr(struct private_data *priv_data, void __user *argp)
{
    int ret = 0, j;
    struct dsm *dsm;
    struct memory_region *mr;
    struct unmap_data udata;
    u64 addr, end;

    if (copy_from_user((void *) &udata, argp, sizeof udata))
        goto out;

    dsm_printk(KERN_INFO "addr [0x%lx] sz [0x%lx] svm[0] [0x%x]\n",
        udata.addr, udata.sz, *udata.svm_ids);

    dsm = find_dsm(udata.dsm_id);
    if (!dsm) {
        dsm_printk(KERN_ERR "can't find dsm %d", udata.dsm_id);
        ret = -EFAULT;
        goto out;
    }

    if (search_mr(dsm, udata.addr)) {
        dsm_printk(KERN_ERR "can't find MR of addr 0x%lx", udata.addr);
        ret = -EFAULT;
        goto out;
    }

    mr = kzalloc(sizeof(struct memory_region), GFP_KERNEL);
    if (!mr) {
        dsm_printk(KERN_ERR "can't allocate memory for MR");
        ret = -ENOMEM;
        goto out;
    }

    mr->addr = udata.addr;
    mr->sz = udata.sz;
    mr->descriptor = dsm_get_descriptor(dsm, udata.svm_ids);

    if (!mr->descriptor) {
        dsm_printk(KERN_ERR "can't find MR descriptor for svm_ids");
        ret = -EFAULT;
        goto out;
    }

    insert_mr(dsm, mr);
    for (j = 0; udata.svm_ids[j]; j++) {
        struct subvirtual_machine *svm;
        u32 svm_id = udata.svm_ids[j];

        svm = find_svm(dsm, svm_id);
        if (!svm) {
            dsm_printk(KERN_ERR "can't find svm %d", svm_id);
            ret = -EFAULT;
            goto out;
        }

#if 0
        if (svm->priv && svm->priv->mm == current->mm) { /* is dsm svm */
            if (j) {
                dsm_printk(KERN_ERR "dsm svm id=%d must be in index 0", svm_id);
                ret = -EFAULT;
            } else {
                dsm_printk(KERN_INFO "dsm svm id=%d found in index 0", svm_id);
            }
            mr->local = LOCAL;
            goto out;
        }
#endif
    }

    end = udata.addr + udata.sz - 1;
    for (addr = udata.addr; addr < end; addr += PAGE_SIZE) {
        ret = dsm_flag_page_remote(current->mm, dsm, mr->descriptor, addr);
        if (ret) {
            dsm_printk(KERN_ERR "can't flag remote page 0x%lx", addr);
            goto out;
        }
    }

out:
    return ret;
}

static int pushback_page(struct private_data *priv_data, void __user *argp) {
    int r = -EFAULT;
    unsigned long addr;
    struct dsm *dsm;
    struct unmap_data udata;
    struct memory_region *mr;

    if (copy_from_user((void *) &udata, argp, sizeof udata))
        goto out;

    dsm = find_dsm(udata.dsm_id);
    BUG_ON(!dsm);

    addr = udata.addr & PAGE_MASK;
    mr = search_mr(dsm, addr);
    r = dsm_request_page_pull(dsm, current->mm, priv_data->svm, udata.addr, mr);

    out: return r;
}

static int open(struct inode *inode, struct file *f) {
    struct private_data *data;
    struct dsm_module_state *dsm_state = get_dsm_module_state();

    data = kmalloc(sizeof(*data), GFP_KERNEL);
    data->svm = NULL;
    data->offset = 0;

    if (!data)
        return -EFAULT;

    mutex_lock(&dsm_state->dsm_state_mutex);
    data->mm = current->mm;
    f->private_data = (void *) data;
    mutex_unlock(&dsm_state->dsm_state_mutex);

    return 0;
}

static int release(struct inode *inode, struct file *f)
{
#if 0
    struct private_data *data = (struct private_data *) f->private_data;
    if (!data->svm)
        return 1;
    remove_svm(data->dsm->dsm_id, data->svm->svm_id);
    if (data->dsm->nb_local_svm == 0) {
        remove_dsm(data->dsm);
        printk("[Release ] last local svm , freeing the dsm\n");
    }
    kfree(data);
#endif
    return 0;
}

static long ioctl(struct file *f, unsigned int ioctl, unsigned long arg) {

    struct private_data *priv_data = (struct private_data *) f->private_data;
    void __user *argp = (void __user *) arg;
    struct dsm_module_state *dsm_state = get_dsm_module_state();

    int r = -1;
    struct conn_element *cele;
    int ip_addr;
    struct svm_data svm_info;

    if (!dsm_state->rcm)
        goto out;

    switch (ioctl) {
        case DSM_DSM: {
            r = register_dsm(priv_data, argp);
            break;
        }
        case DSM_SVM: {
            if (priv_data->dsm)
                r = register_svm(priv_data, argp);
            break;
        }
        case DSM_CONNECT: {
            if (priv_data->dsm)
                r = connect_svm(priv_data, argp);
            break;
        }
        case DSM_MR: {
            if (priv_data->dsm)
                r = register_mr(priv_data, argp);
            break;
        }

        /*
         * Statistics and devel/debug
         */
        case DSM_GEN_STAT: {
            if (copy_from_user((void *) &svm_info, argp, sizeof svm_info))
                goto out;

            ip_addr = inet_addr(svm_info.ip);
            cele = search_rb_conn(ip_addr);

            if (cele)
                reset_dsm_connection_stats(&cele->sysfs);
            break;
        }
        case DSM_GET_STAT: {
            if (copy_from_user((void *) &svm_info, argp, sizeof svm_info))
                goto out;

            ip_addr = inet_addr(svm_info.ip);
            cele = search_rb_conn(ip_addr);
            break;
        }
        case DSM_TRY_PUSH_BACK_PAGE: {
            r = pushback_page(priv_data, argp);
            break;
        }

        default: {
            r = -EFAULT;
            break;
        }
    }

    out: return r;
}

static struct file_operations rdma_fops = { .owner = THIS_MODULE, .release = release, .unlocked_ioctl = ioctl, .open = open, .llseek = noop_llseek, };

static struct miscdevice rdma_misc = { MISC_DYNAMIC_MINOR, "rdma", &rdma_fops,

};

module_param(ip, charp, S_IRUGO|S_IWUSR);
module_param(port, int, S_IRUGO|S_IWUSR);

MODULE_PARM_DESC( ip,
        "The ip of the machine running this module - will be used as node_id.");
MODULE_PARM_DESC( port,
        "The port on the machine running this module - used for DSM_RDMA communication.");

static int dsm_init(void) {
    struct dsm_module_state *dsm_state = create_dsm_module_state();

    reg_dsm_functions(&request_dsm_page, &dsm_request_page_pull);

    printk("[dsm_init] ip : %s\n", ip);
    printk("[dsm_init] port : %d\n", port);

    if (create_rcm(dsm_state, ip, port))
        goto err;

    if (dsm_sysfs_setup(dsm_state)) {
        dereg_dsm_functions();
        destroy_rcm(dsm_state);
    }

    rdma_listen(dsm_state->rcm->cm_id, 2);
    err: return misc_register(&rdma_misc);
}
module_init(dsm_init);

static void dsm_exit(void) {
    struct dsm * dsm = NULL;
    struct dsm_module_state *dsm_state = get_dsm_module_state();
    while (!list_empty(&dsm_state->dsm_list)) {
        dsm = list_first_entry(&dsm_state->dsm_list, struct dsm, dsm_ptr );
        remove_dsm(dsm);
    }

    dereg_dsm_functions();
    dsm_sysfs_cleanup(dsm_state);
    destroy_rcm(dsm_state);

    misc_deregister(&rdma_misc);
    destroy_dsm_module_state();
}
module_exit(dsm_exit);

MODULE_VERSION("0.0.1");
MODULE_AUTHOR("virtex");
MODULE_DESCRIPTION("");
MODULE_LICENSE("GPL");
