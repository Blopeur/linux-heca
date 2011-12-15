/*
 1 * rdma.c
 *
 *  Created on: 22 Jun 2011
 *      Author: john
 */

#include <dsm/dsm_module.h>



static char *ip = 0;
static int port = 0;

static int open(struct inode *inode, struct file *f) {
    struct private_data *data;
    struct rcm * rcm = get_rcm();
    data = kmalloc(sizeof(*data), GFP_KERNEL);

    if (!data)
        return -EFAULT;

    spin_lock(&rcm->rcm_lock);
    data->root_swap = RB_ROOT;
    data->mm = current->mm;
    INIT_LIST_HEAD(&data->head);
    f->private_data = (void *) data;
    spin_unlock(&rcm->rcm_lock);

    return 0;

}

static void free_svm(struct rcu_head *head) {
    kfree(container_of( head, struct subvirtual_machine, rcu_head));

}

static void free_mem_region(struct rcu_head *head) {
    kfree(container_of( head, struct mem_region, rcu));

}

/*
 * 		for DSM in RCM.dsm_ls:
 * 			for SVM in DSM.svm_ls:
 * 				if SVM is local:
 * 					for MR in SVM.mr_ls:
 * 				 		free(MR)
 * 					free(SVM)
 *
 */
static int release(struct inode *inode, struct file *f) {
    struct private_data *data = (struct private_data *) f->private_data;
    struct subvirtual_machine *svm = NULL;
    struct mem_region *mr = NULL;
    struct dsm *_dsm = NULL;
    u16 dsm_id;
    struct rcm * rcm = get_rcm();

    if (!data->svm)
        return 1;

    dsm_id = data->svm->id.dsm_id;
    spin_lock(&rcm->rcm_lock);
    if (data->svm->ele)
        release_page_work(&data->svm->ele->page_pool.page_release_work);
    list_for_each_entry_rcu(_dsm, &rcm->dsm_ls, ls)
    {
        if (_dsm->dsm_id == dsm_id) {
            list_for_each_entry_rcu(svm, &_dsm->svm_ls, ls)
            {
                if (svm == data->svm) {
                    printk("[release] SVM: dsm_id=%u ... vm_id=%u\n",
                            svm->id.dsm_id, svm->id.svm_id);

                    list_for_each_entry_rcu(mr, &svm->mr_ls, ls)
                    {
                        list_del_rcu(&mr->ls);
                        call_rcu(&mr->rcu, free_mem_region);

                    }

                    data->svm->priv = NULL;
                    list_del_rcu(&svm->ls);
                    call_rcu(&svm->rcu_head, free_svm);

                }

            }

        }

    }
    spin_unlock(&rcm->rcm_lock);
    synchronize_rcu();
    kfree(data);

    return 0;
}

static long ioctl(struct file *f, unsigned int ioctl, unsigned long arg) {
    int r = -1;
    unsigned long i = 0;
    unsigned long end = 0;
    int counter = 0;
    int ret = 0;
    unsigned long addr;
    struct conn_element *cele;
    int ip_addr;
    struct dsm_message msg;
    struct page *page;

    struct private_data *priv_data = (struct private_data *) f->private_data;
    void __user *argp = (void __user *) arg;
    struct svm_data svm_info;
    struct subvirtual_machine *svm = NULL;
    struct mem_region *mr = NULL;
    struct dsm *_dsm = NULL;
    struct dsm_vm_id id;
    struct unmap_data udata;
    struct mr_data mr_info;

    struct rcm * rcm = get_rcm();

    switch (ioctl) {
        case DSM_SVM: {

            r = -EFAULT;

            if (copy_from_user((void *) &svm_info, argp, sizeof svm_info))
                goto out;

            id.dsm_id = svm_info.dsm_id;
            id.svm_id = svm_info.svm_id;

            svm = find_svm(&id);

            printk(
                    "[DSM_SVM]\n\tfound svm : %p\n\tdsm_id : %u\n\tsvm_id : %u\n",
                    svm, svm_info.dsm_id, svm_info.svm_id);

            if (!svm) {
                svm = kmalloc(sizeof(*svm), GFP_KERNEL);
                if (!svm)
                    goto fail1;

                priv_data->svm = svm;
                priv_data->offset = svm_info.offset;

                svm->id.dsm_id = svm_info.dsm_id;
                svm->id.svm_id = svm_info.svm_id;
                svm->priv = priv_data;
                svm->ele = NULL;
                spin_lock(&rcm->route_lock);
                _dsm = list_first_entry(&rcm->dsm_ls, struct dsm, ls);

                list_add_rcu(&svm->ls, &_dsm->svm_ls);

                INIT_LIST_HEAD(&svm->mr_ls);
                spin_unlock(&rcm->route_lock);
            } else {
                priv_data->svm = svm;
                priv_data->offset = svm_info.offset;

                svm->priv = priv_data;
                //svm->ele = NULL;
                spin_lock(&rcm->route_lock);
                // Free all MR and add new one
                list_for_each_entry_rcu(mr, &svm->mr_ls, ls)
                {
                    list_del_rcu(&mr->ls);
                    call_rcu(&mr->rcu, free_mem_region);

                }

                synchronize_rcu();
                spin_unlock(&rcm->route_lock);
            }

            r = 0;
            fail1:

            break;

        }
        case DSM_MR: {

            printk("[DSM_MR]\n");

            r = -EFAULT;

            if (copy_from_user((void *) &mr_info, argp, sizeof mr_info))
                goto out;

            id.dsm_id = mr_info.dsm_id;
            id.svm_id = mr_info.svm_id;

            // Make sure specific MR not already created.
            mr = find_mr(mr_info.start_addr, &id);
            if (mr)
                goto out;

            mr = kmalloc(sizeof(*mr), GFP_KERNEL);
            if (!mr)
                goto out;

            mr->addr = mr_info.start_addr;
            mr->sz = mr_info.size;
            mr->svm = find_svm(&id);
            spin_lock(&rcm->route_lock);
            list_add_rcu(&mr->ls, &mr->svm->mr_ls);
            spin_unlock(&rcm->route_lock);
            r = 0;

            break;

        }
        case DSM_CONNECT: {
            //printk("[DSM_CONNECT]\n");

            r = -EFAULT;

            if (copy_from_user((void *) &svm_info, argp, sizeof svm_info))
                goto out;

            id.dsm_id = svm_info.dsm_id;
            id.svm_id = svm_info.svm_id;

            svm = find_svm(&id);

            printk(
                    "[DSM_CONNECT]\n\tfound svm : %p\n\tdsm_id : %u\n\tsvm_id : %u\n",
                    svm, svm_info.dsm_id, svm_info.svm_id);

            if (!svm) {
                svm = kmalloc(sizeof(*svm), GFP_KERNEL);
                if (!svm)
                    goto fail2;

                svm->id.dsm_id = svm_info.dsm_id;
                svm->id.svm_id = svm_info.svm_id;
                svm->priv = NULL;

                ip_addr = inet_addr(svm_info.ip);

                // Check for connection

                cele = search_rb_conn(ip_addr);

                if (!cele) {
                    ret = create_connection(rcm, &svm_info);
                    if (ret)
                        goto fail2;

                }
                cele = search_rb_conn(ip_addr);
                BUG_ON(!cele);
                svm->ele = cele;

                _dsm = list_first_entry(&rcm->dsm_ls, struct dsm, ls);
                spin_lock(&rcm->route_lock);
                list_add_rcu(&svm->ls, &_dsm->svm_ls);

                INIT_LIST_HEAD(&svm->mr_ls);
                spin_unlock(&rcm->route_lock);

            } else if (!svm->ele) {

                ip_addr = inet_addr(svm_info.ip);

                // Check for connection

                cele = search_rb_conn(ip_addr);

                if (!cele) {
                    ret = create_connection(rcm, &svm_info);
                    if (ret)
                        goto fail2;

                }
                svm->ele = cele;

            }

            r = 0;

            fail2:

            break;

        }
        case DSM_UNMAP_RANGE: {
            printk("[DSM_UNMAP_RANGE]\n");

            r = -EFAULT;

            if (copy_from_user((void *) &udata, argp, sizeof udata))
                goto out;

            // DSM2: why are locks outside of function?

            svm = find_svm(&udata.id);

            r = -1;

            if (!svm)
                goto out;

            if (priv_data->svm->id.dsm_id != svm->id.dsm_id)
                goto out;

            i = udata.addr;
            end = i + udata.sz;
            counter = 0;
            while (i < end) {
                r = dsm_flag_page_remote(current->mm, udata.id, i);
                if (r)
                    break;

                i += PAGE_SIZE;
                counter++;

            }
            printk("[?] unmapped #pages : %d\n", counter);
            r = 0;

            break;

        }
        case PAGE_SWAP: {
            r = -EFAULT;

            printk("[PAGE_SWAP] swapping of one page \n");
            if (copy_from_user((void *) &msg, argp, sizeof msg))
                goto out;

            page = dsm_extract_page_from_remote(&msg);

            if (page == (void *) -EFAULT
            )
                r = -EFAULT;
            else
                r = !!page;

            break;

        }
        case UNMAP_PAGE: {

            r = -EFAULT;

            if (copy_from_user((void *) &udata, argp, sizeof udata)) {
                goto out;
            }

            svm = find_svm(&udata.id);

            if (!svm) {
                printk("[UNMAP_PAGE] could not find the route element \n");
                r = -1;
                printk("[unmap page 1] dsm_id : %d - vm_id : %d\n",
                        udata.id.dsm_id, udata.id.svm_id);
                goto out;

            }

            printk("[unmap page 2] dsm_id : %d - vm_id : %d\n", udata.id.dsm_id,
                    udata.id.svm_id);

            if (priv_data->svm->id.dsm_id != svm->id.dsm_id) {
                printk("[UNMAP_PAGE] DSM id not same, bad id  \n");
                r = -1;
            }

            r = dsm_flag_page_remote(current->mm, udata.id, udata.addr);

            break;

        }
        case DSM_TRY_PUSH_BACK_PAGE: {
            printk("[DSM_TRY_PUSH_BACK_PAGE]\n");

            r = -EFAULT;

            if (copy_from_user((void *) &udata, argp, sizeof udata))
                goto out;

            // DSM2: why are locks outside of function?

            svm = find_svm(&udata.id);

            r = -1;

            if (!svm)
                goto out;
            if (svm == priv_data->svm)
                goto out;
            addr = udata.addr & PAGE_MASK;
            if (!page_is_in_dsm_cache(addr))
                r = dsm_request_page_pull(current->mm, svm, priv_data->svm,
                        udata.addr);
            else
                r = 0;
            break;

        }

        case DSM_GEN_STAT: {
            if (copy_from_user((void *) &svm_info, argp, sizeof svm_info))
                goto out;

            ip_addr = inet_addr(svm_info.ip);
            cele = search_rb_conn(ip_addr);

            if (likely(cele)) {

                reset_dsm_connection_stats(&cele->stats);

            }

            break;
        }
        case DSM_GET_STAT: {
            if (copy_from_user((void *) &svm_info, argp, sizeof svm_info))
                goto out;

            ip_addr = inet_addr(svm_info.ip);
            cele = search_rb_conn(ip_addr);

            if (likely(cele)) {

                //  print_dsm_stats(&cele->stats);

            }

            break;
        }
        default: {
            r = -EFAULT;

            break;

        }

    }

    out: return r;

}

static struct file_operations rdma_fops = { .owner = THIS_MODULE, .release =
        release, .unlocked_ioctl = ioctl, .open = open, .llseek = noop_llseek,

};

static struct miscdevice rdma_misc = { MISC_DYNAMIC_MINOR, "rdma", &rdma_fops,

};

module_param(ip, charp, S_IRUGO|S_IWUSR);
module_param(port, int, S_IRUGO|S_IWUSR);

MODULE_PARM_DESC( ip,
        "The ip of the machine running this module - will be used as node_id.");
MODULE_PARM_DESC(
        port,
        "The port on the machine running this module - used for DSM_RDMA communication.");

static int dsm_init(void) {
    struct dsm *_dsm;
    struct rcm * rcm;
    reg_dsm_functions(&find_svm, &find_local_svm, &request_dsm_page);

    printk("[dsm_init] ip : %s\n", ip);
    printk("[dsm_init] port : %d\n", port);

    if (create_rcm(ip, port))
        goto err;
    rcm = get_rcm();
    if (dsm_sysf_setup(rcm)) {
        dereg_dsm_functions();

        destroy_rcm();
    }

    INIT_LIST_HEAD(&rcm->dsm_ls);

    _dsm = kmalloc(sizeof(*_dsm), GFP_KERNEL);

    _dsm->dsm_id = 1;

    INIT_LIST_HEAD(&_dsm->svm_ls);
    spin_lock(&rcm->rcm_lock);
    list_add_rcu(&_dsm->ls, &rcm->dsm_ls);
    spin_unlock(&rcm->rcm_lock);
    rdma_listen(rcm->cm_id, 2);
//DSM2: really need better cleanup here - incase of failure
    err: return misc_register(&rdma_misc);

}
module_init(dsm_init);

static void dsm_exit(void) {
    dereg_dsm_functions();
    dsm_sysf_cleanup(get_rcm());
    destroy_rcm();

    misc_deregister(&rdma_misc);

}
module_exit(dsm_exit);

MODULE_VERSION("0.0.1");
MODULE_AUTHOR("virtex");
MODULE_DESCRIPTION("");
MODULE_LICENSE("GPL");
