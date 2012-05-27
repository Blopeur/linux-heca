/*
 1 * rdma.c
 *
 *  Created on: 22 Jun 2011
 *      Author: Benoit
 */

#include <linux/list.h>
#include <dsm/dsm_module.h>

static char *ip = 0;
static int port = 0;

module_param(ip, charp, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(ip, "The ip of the machine running this module - will be used"
       " as node_id.");
module_param(port, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(port, "The port on the machine running this module - used for"
       " DSM_RDMA communication.");

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

void remove_dsm(struct dsm *dsm)
{
    struct subvirtual_machine *svm;
    struct dsm_module_state *dsm_state = get_dsm_module_state();
    struct list_head *pos, *n;

    printk("[remove_dsm] removing dsm %d  \n", dsm->dsm_id);

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

static int register_dsm(struct private_data *priv_data, void __user *argp)
{
    int r = -EFAULT;
    struct svm_data svm_info;
    struct dsm *found_dsm, *new_dsm = NULL;
    struct dsm_module_state *dsm_state = get_dsm_module_state();

    if (copy_from_user((void *) &svm_info, argp, sizeof svm_info)) {
        printk("[register_dsm] reading data from userspace failed \n");
        return r;
    }

    mutex_lock(&dsm_state->dsm_state_mutex);

    do {
        found_dsm = find_dsm(svm_info.dsm_id);
        if (found_dsm) {
            printk("[register_dsm] we already have the dsm in place \n");
            break;
        }

        new_dsm = kzalloc(sizeof(*new_dsm), GFP_KERNEL);
        if (!new_dsm)
            break;

        new_dsm->dsm_id = svm_info.dsm_id;
        mutex_init(&new_dsm->dsm_mutex);
        seqlock_init(&new_dsm->mr_seq_lock);
        INIT_RADIX_TREE(&new_dsm->svm_tree_root, GFP_KERNEL);
        INIT_RADIX_TREE(&new_dsm->svm_mm_tree_root, GFP_KERNEL);
        INIT_LIST_HEAD(&new_dsm->svm_list);
        new_dsm->mr_tree_root = RB_ROOT;
        new_dsm->nb_local_svm = 0;

        r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
        if (r)
            break;
        r = radix_tree_insert(&dsm_state->dsm_tree_root,
                (unsigned long) svm_info.dsm_id, new_dsm);
        radix_tree_preload_end();

        if (likely(!r)) {
            if (create_dsm_sysfs_entry(new_dsm, dsm_state)) {
                radix_tree_delete(&dsm_state->dsm_tree_root,
                        (unsigned long) svm_info.dsm_id);
                continue;
            }

            priv_data->dsm = new_dsm;
            list_add(&new_dsm->dsm_ptr, &dsm_state->dsm_list);
            printk("[DSM_DSM]\n registered dsm %p,  dsm_id : %u, res: %d \n",
                    new_dsm, svm_info.dsm_id, r);
            goto exit;
        }

    } while (r != -ENOMEM);

    if (new_dsm)
        kfree(new_dsm);

exit: 
    mutex_unlock(&dsm_state->dsm_state_mutex);
    return r;
}

static int register_svm(struct private_data *priv_data, void __user *argp)
{
    int r = -EFAULT;
    struct dsm *dsm;
    struct subvirtual_machine *found_svm, *new_svm = NULL;
    struct svm_data svm_info;

    if (copy_from_user((void *) &svm_info, argp, sizeof svm_info))
        return r;

    dsm = find_dsm(svm_info.dsm_id);
    BUG_ON(!dsm);

    mutex_lock(&dsm->dsm_mutex);
    do {
        found_svm = find_svm(dsm, svm_info.svm_id);
        if (found_svm)
            break;

        new_svm = kzalloc(sizeof(*new_svm), GFP_KERNEL);
        if (!new_svm)
            break;

        r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
        if (r)
            break;

        r = radix_tree_insert(&dsm->svm_tree_root,
                (unsigned long) svm_info.svm_id, new_svm);
        if (likely(!r)) {
            //the following should never fail as we locked the dsm and we made sure that we add the ID first
            r = radix_tree_insert(&dsm->svm_mm_tree_root,
                    (unsigned long) priv_data->mm, new_svm);
            r = radix_tree_insert(&get_dsm_module_state()->mm_tree_root,
                    (unsigned long) priv_data->mm, new_svm);
            radix_tree_preload_end();

            new_svm->priv = priv_data;
            priv_data->svm = new_svm;
            priv_data->offset = svm_info.offset;
            new_svm->svm_id = svm_info.svm_id;
            new_svm->ele = NULL;
            new_svm->dsm = dsm;
            new_svm->dsm->nb_local_svm++;
            atomic_set(&new_svm->status, DSM_SVM_ONLINE);

            if (create_svm_sysfs_entry(new_svm)) {
                radix_tree_delete(&dsm->svm_tree_root,
                        (unsigned long) svm_info.svm_id);
                radix_tree_delete(&dsm->svm_mm_tree_root,
                        (unsigned long) priv_data->mm);
                continue;
            }

            spin_lock_init(&new_svm->page_cache_spinlock);
            INIT_RADIX_TREE(&new_svm->page_cache, GFP_ATOMIC);
            new_svm->push_cache = RB_ROOT;
            seqlock_init(&new_svm->push_cache_lock);
            INIT_LIST_HEAD(&new_svm->mr_list);
            list_add(&new_svm->svm_ptr, &dsm->svm_list);
            printk("[DSM_SVM] reg svm %p, res %d, dsm_id %u, svm_id: %u\n",
                    new_svm, r, svm_info.dsm_id, svm_info.svm_id);
            goto out;

        }
        radix_tree_preload_end();

    } while (r != -ENOMEM);

    if (new_svm)
        kfree(new_svm);

out: 
    mutex_unlock(&dsm->dsm_mutex);
    return r;
}

static int connect_svm(struct private_data *priv_data, void __user *argp)
{
    int r = -EFAULT, ip_addr;
    struct dsm *dsm;
    struct subvirtual_machine *found_svm, *new_svm = NULL;
    struct svm_data svm_info;
    struct conn_element *cele;
    struct dsm_module_state *dsm_state = get_dsm_module_state();

    if (copy_from_user((void *) &svm_info, argp, sizeof svm_info))
        return r;

    dsm = find_dsm(svm_info.dsm_id);
    BUG_ON(!dsm);

    printk("[DSM_CONNECT] dsm_id: %u [%p], svm_id: %u\n", svm_info.dsm_id, dsm,
            svm_info.svm_id);

    mutex_lock(&dsm->dsm_mutex);
    do {
        found_svm = find_svm(dsm, svm_info.svm_id);
        if (found_svm)
            break;

        new_svm = kzalloc(sizeof(*new_svm), GFP_KERNEL);
        if (!new_svm)
            break;

        r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
        if (r)
            break;
        r = radix_tree_insert(&dsm->svm_tree_root,
                (unsigned long) svm_info.svm_id, new_svm);
        radix_tree_preload_end();

        if (likely(!r)) {
            u32 svm_id[2] = { svm_info.svm_id, 0 };

            new_svm->svm_id = svm_info.svm_id;
            new_svm->priv = NULL;
            new_svm->dsm = dsm;
            new_svm->descriptor = dsm_get_descriptor(dsm, svm_id);
            atomic_set(&new_svm->status, DSM_SVM_ONLINE);

            if (create_svm_sysfs_entry(new_svm)) {
                radix_tree_delete(&dsm->svm_tree_root,
                        (unsigned long) svm_info.svm_id);
                continue;
            }

            INIT_LIST_HEAD(&new_svm->mr_list);
            list_add(&new_svm->svm_ptr, &dsm->svm_list);
            ip_addr = inet_addr(svm_info.ip);

            // Check for connection
            cele = search_rb_conn(ip_addr);
            if (!cele) {
                r = create_connection(dsm_state->rcm, &svm_info);
                if (r)
                    goto connect_fail;

                might_sleep();
                cele = search_rb_conn(ip_addr);
                if (!cele) {
                    r = -ENOLINK;
                    goto connect_fail;
                }

                wait_for_completion(&cele->completion);
                if (!atomic_read(&cele->alive)) {
                    r = -ENOLINK;
                    goto connect_fail;
                }
            }
            new_svm->ele = cele;
            break;
        }

    } while (r != -ENOMEM);

    mutex_unlock(&dsm->dsm_mutex);
    return r;

connect_fail: 
    list_del(&new_svm->svm_ptr);
    radix_tree_delete(&dsm->svm_tree_root, (unsigned long) svm_info.svm_id);
    delete_svm_sysfs_entry(&new_svm->svm_sysfs.svm_kobject);

    kfree(new_svm);
    mutex_unlock(&dsm->dsm_mutex);
    return r;
}

static int do_unmap_range(struct dsm *dsm, int dsc, unsigned long start,
        unsigned long end)
{
    int r = 0;
    unsigned long it;

    for (it = start; it < end; it += PAGE_SIZE) {
        r = dsm_flag_page_remote(current->mm, dsm, dsc, it);
        if (r)
            break;
    }

    return r;
}

static int unmap_range(struct private_data *priv_data, void __user *argp)
{
    int r = -EFAULT;
    struct unmap_data udata;
    struct dsm *dsm;

    if (copy_from_user((void *) &udata, argp, sizeof udata))
        goto out;

    dsm = find_dsm(udata.dsm_id);
    if (!dsm)
        goto out;

    r = do_unmap_range(dsm, dsm_get_descriptor(dsm, udata.svm_ids), udata.addr,
            udata.addr + udata.sz - 1);

out:
    return r;
}

static int register_mr(struct private_data *priv_data, void __user *argp)
{
    int r = -EFAULT, i;
    struct dsm *dsm;
    struct memory_region *mr;
    struct unmap_data udata;

    if (copy_from_user((void *) &udata, argp, sizeof udata))
        goto out;

    printk("[DSM_MR] addr [%lu] sz [%zu]\n", udata.addr, udata.sz);

    dsm = find_dsm(udata.dsm_id);
    BUG_ON(!dsm);

//     Make sure specific MR not already created.
    if (search_mr(dsm, udata.addr))
        goto out;

    mr = kzalloc(sizeof(struct memory_region), GFP_KERNEL);
    if (!mr)
        goto out;

    mr->addr = udata.addr;
    mr->sz = udata.sz;
    mr->descriptor = dsm_get_descriptor(dsm, udata.svm_ids);

    insert_mr(dsm, mr);
    for (i = 0; udata.svm_ids[i]; i++) {
        struct subvirtual_machine *svm = find_svm(dsm, udata.svm_ids[i]);
        if (!svm)
            goto out;

        if (svm->priv && svm->priv->mm == current->mm) {
            if (i == 0)
                r = 0;
            mr->local = LOCAL;
            goto out;
        }
    }

    r = 0;
    if (udata.unmap) {
        r = do_unmap_range(dsm, mr->descriptor, udata.addr,
                udata.addr + udata.sz - 1);
    }

out: 
    return r;
}

static int pushback_page(struct private_data *priv_data, void __user *argp)
{
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

out: 
    return r;
}

static int open(struct inode *inode, struct file *f)
{
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
    struct private_data *data = (struct private_data *) f->private_data;

    if (!data->svm)
        return 1;
    remove_svm(data->dsm->dsm_id, data->svm->svm_id);
    if (data->dsm->nb_local_svm == 0) {
        remove_dsm(data->dsm);
        printk("[Release ] last local svm , freeing the dsm\n");
    }
    kfree(data);

    return 0;
}

static long ioctl(struct file *f, unsigned int ioctl, unsigned long arg)
{
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
        case DSM_DSM:
            r = register_dsm(priv_data, argp);
            break;
        case DSM_SVM:
            if (priv_data->dsm)
                r = register_svm(priv_data, argp);
            break;
        case DSM_CONNECT:
            if (priv_data->dsm)
                r = connect_svm(priv_data, argp);
            break;
        case DSM_MR:
            if (priv_data->dsm)
                r = register_mr(priv_data, argp);
            break;
        case DSM_UNMAP_RANGE:
            if (priv_data->dsm)
                r = unmap_range(priv_data, argp);
            break;

        /*
         * Statistics and devel/debug
         */
        case DSM_GEN_STAT: {
            if (copy_from_user((void *) &svm_info, argp, sizeof svm_info))
                goto out;

            ip_addr = inet_addr(svm_info.ip);
            cele = search_rb_conn(ip_addr);

            if (likely(cele)) {
                reset_dsm_connection_stats(&cele->sysfs);
            }
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

out: 
    return r;
}

static struct file_operations rdma_fops = { .owner = THIS_MODULE,
    .release = release, .unlocked_ioctl = ioctl, .open = open,
    .llseek = noop_llseek, };
static struct miscdevice rdma_misc = { MISC_DYNAMIC_MINOR, "rdma",
    &rdma_fops, };

static int dsm_init(void)
{
    struct dsm_module_state *dsm_state = create_dsm_module_state();

    reg_dsm_functions(&request_dsm_page, &dsm_request_page_pull,
            &reclaim_dsm_page);

    printk("[dsm_init] ip : %s\n", ip);
    printk("[dsm_init] port : %d\n", port);

    if (create_rcm(dsm_state, ip, port))
        goto err;

    if (dsm_sysfs_setup(dsm_state)) {
        dereg_dsm_functions();
        destroy_rcm(dsm_state);
    }

    rdma_listen(dsm_state->rcm->cm_id, 2);
err: 
    return misc_register(&rdma_misc);
}
module_init(dsm_init);

static void dsm_exit(void)
{
    struct dsm *dsm = NULL;
    struct dsm_module_state *dsm_state = get_dsm_module_state();
    while (!list_empty(&dsm_state->dsm_list)) {
        dsm = list_first_entry(&dsm_state->dsm_list, struct dsm, dsm_ptr);
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

