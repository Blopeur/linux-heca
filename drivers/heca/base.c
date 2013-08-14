/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 * Steve Walsh <steve.walsh@sap.com> 2012 (c)
 */
#include <linux/pagemap.h>
#include "ioctl.h"
#include "trace.h"
#include "struct.h"
#include "base.h"
#include "conn.h"
#include "pull.h"
#include "push.h"
#include "sysfs.h"
#include "ops.h"
#include "task.h"

/*
 * conn_element funcs
 */
struct heca_connection_element *search_rb_conn(int node_ip)
{
        struct heca_connections_manager *rcm = get_dsm_module_state()->hcm;
        struct rb_root *root;
        struct rb_node *node;
        struct heca_connection_element *this = 0;
        unsigned long seq;

        do {
                seq = read_seqbegin(&rcm->connections_lock);
                root = &rcm->connections_rb_tree_root;
                for (node = root->rb_node; node; this = 0) {
                        this = rb_entry(node, struct heca_connection_element, rb_node);

                        if (node_ip < this->remote_node_ip)
                                node = node->rb_left;
                        else if (node_ip > this->remote_node_ip)
                                node = node->rb_right;
                        else
                                break;
                }
        } while (read_seqretry(&rcm->connections_lock, seq));

        return this;
}

void insert_rb_conn(struct heca_connection_element *ele)
{
        struct heca_connections_manager *rcm = get_dsm_module_state()->hcm;
        struct rb_root *root;
        struct rb_node **new, *parent = NULL;
        struct heca_connection_element *this;

        write_seqlock(&rcm->connections_lock);
        root = &rcm->connections_rb_tree_root;
        new = &root->rb_node;
        while (*new) {
                this = rb_entry(*new, struct heca_connection_element, rb_node);
                parent = *new;
                if (ele->remote_node_ip < this->remote_node_ip)
                        new = &((*new)->rb_left);
                else if (ele->remote_node_ip > this->remote_node_ip)
                        new = &((*new)->rb_right);
        }
        rb_link_node(&ele->rb_node, parent, new);
        rb_insert_color(&ele->rb_node, root);
        write_sequnlock(&rcm->connections_lock);
}

void erase_rb_conn(struct heca_connection_element *ele)
{
        struct heca_connections_manager *rcm = get_dsm_module_state()->hcm;

        write_seqlock(&rcm->connections_lock);
        rb_erase(&ele->rb_node, &rcm->connections_rb_tree_root);
        write_sequnlock(&rcm->connections_lock);
}

/*
 * dsm funcs
 */
struct heca_space *find_dsm(u32 id)
{
        struct heca_module_state *dsm_state = get_dsm_module_state();
        struct heca_space *dsm;
        struct heca_space **dsmp;
        struct radix_tree_root *root;

        rcu_read_lock();
        root = &dsm_state->hspaces_tree_root;
repeat:
        dsm = NULL;
        dsmp = (struct heca_space **) radix_tree_lookup_slot(root, (unsigned long) id);
        if (dsmp) {
                dsm = radix_tree_deref_slot((void **) dsmp);
                if (unlikely(!dsm))
                        goto out;
                if (radix_tree_exception(dsm)) {
                        if (radix_tree_deref_retry(dsm))
                                goto repeat;
                }
        }
out:
        rcu_read_unlock();
        return dsm;
}

void remove_dsm(struct heca_space *dsm)
{
        struct heca_process *svm;
        struct heca_module_state *dsm_state = get_dsm_module_state();
        struct list_head *pos, *n;

        BUG_ON(!dsm);

        heca_printk(KERN_DEBUG "<enter> dsm=%d", dsm->hspace_id);

        list_for_each_safe (pos, n, &dsm->hprocs_list) {
                svm = list_entry(pos, struct heca_process, hproc_ptr);
                remove_svm(dsm->hspace_id, svm->hproc_id);
        }

        mutex_lock(&dsm_state->heca_state_mutex);
        list_del(&dsm->hspace_ptr);
        radix_tree_delete(&dsm_state->hspaces_tree_root,
                        (unsigned long) dsm->hspace_id);
        mutex_unlock(&dsm_state->heca_state_mutex);
        synchronize_rcu();

        delete_dsm_sysfs_entry(&dsm->hspace_kobject);

        mutex_lock(&dsm_state->heca_state_mutex);
        kfree(dsm);
        mutex_unlock(&dsm_state->heca_state_mutex);

        heca_printk(KERN_DEBUG "<exit>");
}


int create_dsm(__u32 dsm_id)
{
        int r = 0;
        struct heca_space *found_dsm, *new_dsm = NULL;
        struct heca_module_state *dsm_state = get_dsm_module_state();

        /* already exists? (first check; the next one is under lock */
        found_dsm = find_dsm(dsm_id);
        if (found_dsm) {
                heca_printk("we already have the dsm in place");
                return -EEXIST;
        }

        /* allocate a new dsm */
        new_dsm = kzalloc(sizeof(*new_dsm), GFP_KERNEL);
        if (!new_dsm) {
                heca_printk("can't allocate");
                return -ENOMEM;
        }
        new_dsm->hspace_id = dsm_id;
        mutex_init(&new_dsm->hspace_mutex);
        INIT_RADIX_TREE(&new_dsm->hprocs_tree_root, GFP_KERNEL & ~__GFP_WAIT);
        INIT_RADIX_TREE(&new_dsm->hprocs_mm_tree_root, GFP_KERNEL & ~__GFP_WAIT);
        INIT_LIST_HEAD(&new_dsm->hprocs_list);
        new_dsm->nb_local_hprocs = 0;

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

        spin_lock(&dsm_state->radix_lock);
        r = radix_tree_insert(&dsm_state->hspaces_tree_root,
                        (unsigned long) new_dsm->hspace_id, new_dsm);
        spin_unlock(&dsm_state->radix_lock);
        radix_tree_preload_end();

        if (r) {
                heca_printk("radix_tree_insert: failed %d", r);
                goto failed;
        }

        r = create_dsm_sysfs_entry(new_dsm, dsm_state);
        if (r) {
                heca_printk("create_dsm_sysfs_entry: failed %d", r);
                goto err_delete;
        }

        list_add(&new_dsm->hspace_ptr, &dsm_state->hspaces_list);
        heca_printk("registered dsm %p, dsm_id : %u, res: %d",
                        new_dsm, dsm_id, r);
        return r;

err_delete:
        radix_tree_delete(&dsm_state->hspaces_tree_root,
                        (unsigned long) dsm_id);
failed:
        kfree(new_dsm);
        return r;
}

/*
 * svm funcs
 */
static void destroy_svm_mrs(struct heca_process *svm);

static inline int is_svm_local(struct heca_process *svm)
{
        return !!svm->mm;
}

static inline int grab_svm(struct heca_process *svm)
{
#if !defined(CONFIG_SMP) && defined(CONFIG_TREE_RCU)
# ifdef CONFIG_PREEMPT_COUNT
        BUG_ON(!in_atomic());
# endif
        BUG_ON(atomic_read(&hproc->refs) == 0);
        atomic_inc(&hproc->refs);
#else
        if (!atomic_inc_not_zero(&svm->refs))
                return -1;
#endif
        return 0;
}

static struct heca_process *_find_svm_in_tree(
                struct radix_tree_root *root, unsigned long svm_id)
{
        struct heca_process *svm;
        struct heca_process **svmp;

        rcu_read_lock();
repeat:
        svm = NULL;
        svmp = (struct heca_process **) radix_tree_lookup_slot(root,
                        (unsigned long) svm_id);
        if (svmp) {
                svm = radix_tree_deref_slot((void**) svmp);
                if (unlikely(!svm))
                        goto out;
                if (radix_tree_exception(svm)) {
                        if (radix_tree_deref_retry(svm))
                                goto repeat;
                }

                if (grab_svm(svm))
                        goto repeat;

        }

out:
        rcu_read_unlock();
        return svm;
}

inline struct heca_process *find_svm(struct heca_space *dsm, u32 svm_id)
{
        return _find_svm_in_tree(&dsm->hprocs_tree_root, (unsigned long) svm_id);
}

inline struct heca_process *find_local_svm_in_dsm(struct heca_space *dsm,
                struct mm_struct *mm)
{
        return _find_svm_in_tree(&dsm->hprocs_mm_tree_root, (unsigned long) mm);
}

inline struct heca_process *find_local_svm_from_mm(struct mm_struct *mm)
{
        struct heca_module_state *mod = get_dsm_module_state();

        return (likely(mod)) ?
                _find_svm_in_tree(&mod->mm_tree_root, (unsigned long) mm) :
                NULL;
}

static int insert_svm_to_radix_trees(struct heca_module_state *dsm_state,
                struct heca_space *dsm, struct heca_process *new_svm)
{
        int r;

preload:
        r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
        if (r) {
                if (r == -ENOMEM) {
                        heca_printk(KERN_ERR "radix_tree_preload: ENOMEM retrying ...");
                        mdelay(2);
                        goto preload;
                }
                heca_printk(KERN_ERR "radix_tree_preload: failed %d", r);
                goto out;
        }


        spin_lock(&dsm_state->radix_lock);
        r = radix_tree_insert(&dsm->hprocs_tree_root,
                        (unsigned long) new_svm->hproc_id, new_svm);
        if (r)
                goto unlock;

        if (is_svm_local(new_svm)) {
                r = radix_tree_insert(&dsm->hprocs_mm_tree_root,
                                (unsigned long) new_svm->mm, new_svm);
                if (r)
                        goto unlock;

                r = radix_tree_insert(&dsm_state->mm_tree_root,
                                (unsigned long) new_svm->mm, new_svm);
        }

unlock:
        spin_unlock(&dsm_state->radix_lock);

        radix_tree_preload_end();
        if (r) {
                heca_printk(KERN_ERR "failed radix_tree_insert %d", r);
                radix_tree_delete(&dsm->hprocs_tree_root,
                                (unsigned long) new_svm->hproc_id);
                if (is_svm_local(new_svm)) {
                        radix_tree_delete(&dsm->hprocs_mm_tree_root,
                                        (unsigned long) new_svm->mm);
                        radix_tree_delete(&dsm_state->mm_tree_root,
                                        (unsigned long) new_svm->mm);
                }
        }

out:
        return r;
}

int create_svm(struct hecaioc_hproc *svm_info)
{
        struct heca_module_state *dsm_state = get_dsm_module_state();
        int r = 0;
        struct heca_space *dsm;
        struct heca_process *found_svm, *new_svm = NULL;

        /* allocate a new svm */
        new_svm = kzalloc(sizeof(*new_svm), GFP_KERNEL);
        if (!new_svm) {
                heca_printk(KERN_ERR "failed kzalloc");
                return -ENOMEM;
        }

        /* grab dsm lock */
        mutex_lock(&dsm_state->heca_state_mutex);
        dsm = find_dsm(svm_info->hspace_id);
        if (dsm)
                mutex_lock(&dsm->hspace_mutex);
        mutex_unlock(&dsm_state->heca_state_mutex);
        if (!dsm) {
                heca_printk(KERN_ERR "could not find dsm: %d",
                                svm_info->hspace_id);
                r = -EFAULT;
                goto no_dsm;
        }

        /* already exists? */
        found_svm = find_svm(dsm, svm_info->hproc_id);
        if (found_svm) {
                heca_printk(KERN_ERR "svm %d (dsm %d) already exists",
                                svm_info->hproc_id, svm_info->hspace_id);
                r = -EEXIST;
                goto out;
        }

        /* initial svm data */
        new_svm->hproc_id = svm_info->hproc_id;
        new_svm->is_local = svm_info->is_local;
        new_svm->pid = svm_info->pid;
        new_svm->hspace = dsm;
        atomic_set(&new_svm->refs, 2);

        /* register local svm */
        if (svm_info->is_local) {
                struct mm_struct *mm;

                mm = find_mm_by_pid(new_svm->pid);
                if (!mm) {
                        heca_printk(KERN_ERR "can't find pid %d", new_svm->pid);
                        r = -ESRCH;
                        goto out;
                }

                found_svm = find_local_svm_from_mm(mm);
                if (found_svm) {
                        heca_printk(KERN_ERR "svm already exists for current process");
                        r = -EEXIST;
                        goto out;
                }

                new_svm->mm = mm;
                new_svm->hspace->nb_local_hprocs++;
                new_svm->hmr_tree_root = RB_ROOT;
                seqlock_init(&new_svm->hmr_seq_lock);
                new_svm->hmr_cache = NULL;

                init_llist_head(&new_svm->delayed_gup);
                INIT_DELAYED_WORK(&new_svm->delayed_gup_work,
                                delayed_gup_work_fn);
                init_llist_head(&new_svm->deferred_gups);
                INIT_WORK(&new_svm->deferred_gup_work, deferred_gup_work_fn);

                spin_lock_init(&new_svm->page_cache_spinlock);
                spin_lock_init(&new_svm->page_readers_spinlock);
                spin_lock_init(&new_svm->page_maintainers_spinlock);
                INIT_RADIX_TREE(&new_svm->page_cache, GFP_ATOMIC);
                INIT_RADIX_TREE(&new_svm->page_readers, GFP_ATOMIC);
                INIT_RADIX_TREE(&new_svm->page_maintainers, GFP_ATOMIC);
                new_svm->push_cache = RB_ROOT;
                seqlock_init(&new_svm->push_cache_lock);
        }

        r = create_svm_sysfs_entry(new_svm);
        if (r) {
                heca_printk(KERN_ERR "failed create_svm_sysfs_entry %d", r);
                goto out;
        }

        /* register svm by id and mm_struct (must come before dsm_get_descriptor) */
        if (insert_svm_to_radix_trees(dsm_state, dsm, new_svm))
                goto out;
        list_add(&new_svm->hproc_ptr, &dsm->hprocs_list);

        /* assign descriptor for remote svm */
        if (!is_svm_local(new_svm)) {
                u32 svm_ids[] = {new_svm->hproc_id, 0};
                new_svm->descriptor = dsm_get_descriptor(dsm->hspace_id, svm_ids);
        }

out:
        mutex_unlock(&dsm->hspace_mutex);
        if (found_svm)
                release_svm(found_svm);

        if (r) {
                kfree(new_svm);
                new_svm = NULL;
                goto no_dsm;
        }

        if (!svm_info->is_local) {
                r = connect_svm(svm_info->hspace_id, svm_info->hproc_id,
                                svm_info->remote.sin_addr.s_addr,
                                svm_info->remote.sin_port);

                if (r) {
                        heca_printk(KERN_ERR "connect_svm failed %d", r);
                        kfree(new_svm);
                        new_svm = NULL;
                }
        }
no_dsm:
        heca_printk(KERN_INFO "svm %p, res %d, dsm_id %u, svm_id: %u --> ret %d",
                        new_svm, r, svm_info->hspace_id, svm_info->hproc_id, r);
        return r;
}

inline void release_svm(struct heca_process *svm)
{
        atomic_dec(&svm->refs);
        if (atomic_cmpxchg(&svm->refs, 1, 0) == 1) {
                trace_free_svm(svm->hproc_id);
                delete_svm_sysfs_entry(&svm->hproc_kobject);
                synchronize_rcu();
                kfree(svm);
        }
}

/*
 * We dec page's refcount for every missing remote response (it would have
 * happened in dsm_ppe_clear_release after sending an answer to remote svm)
 */
static void surrogate_push_remote_svm(struct heca_process *svm,
                struct heca_process *remote_svm)
{
        struct rb_node *node;

        write_seqlock(&svm->push_cache_lock);
        for (node = rb_first(&svm->push_cache); node;) {
                struct heca_page_cache *dpc;
                int i;
                dpc = rb_entry(node, struct heca_page_cache, rb_node);
                node = rb_next(node);
                for (i = 0; i < dpc->hprocs.num; i++) {
                        if (dpc->hprocs.ids[i] == remote_svm->hproc_id)
                                goto surrogate;
                }
                continue;

surrogate:
                if (likely(test_and_clear_bit(i, &dpc->bitmap))) {
                        page_cache_release(dpc->pages[0]);
                        atomic_dec(&dpc->nproc);
                        if (atomic_cmpxchg(&dpc->nproc, 1, 0) ==
                                        1 && find_first_bit(&dpc->bitmap,
                                                dpc->hprocs.num) >= dpc->hprocs.num)
                                dsm_push_cache_release(dpc->hproc, &dpc, 0);
                }
        }
        write_sequnlock(&svm->push_cache_lock);
}

static void release_svm_push_elements(struct heca_process *svm)
{
        struct rb_node *node;

        write_seqlock(&svm->push_cache_lock);
        for (node = rb_first(&svm->push_cache); node;) {
                struct heca_page_cache *dpc;
                int i;

                dpc = rb_entry(node, struct heca_page_cache, rb_node);
                node = rb_next(node);
                /*
                 * dpc->svms has a pointer to the descriptor ids array, which already
                 * changed. we need to rely on the bitmap right now.
                 */
                for (i = 0; i < dpc->hprocs.num; i++) {
                        if (test_and_clear_bit(i, &dpc->bitmap))
                                page_cache_release(dpc->pages[0]);
                }
                dsm_push_cache_release(dpc->hproc, &dpc, 0);
        }
        write_sequnlock(&svm->push_cache_lock);
}

/*
 * pull ops tx_elements are only released after a response has returned.
 * therefore we can catch them and surrogate for them by iterating the tx
 * buffer.
 */
static void release_svm_tx_elements(struct heca_process *svm,
                struct heca_connection_element *ele)
{
        struct tx_buffer_element *tx_buf;
        int i;

        /* killed before it was first connected */
        if (!ele || !ele->tx_buffer.tx_buf)
                return;

        tx_buf = ele->tx_buffer.tx_buf;

        for (i = 0; i < ele->tx_buffer.len; i++) {
                struct tx_buffer_element *tx_e = &tx_buf[i];
                struct heca_message *msg = tx_e->hmsg_buffer;
                int types = MSG_REQ_PAGE | MSG_REQ_PAGE_TRY |
                        MSG_RES_PAGE_FAIL | MSG_REQ_READ;

                if (msg->type & types && msg->dsm_id == svm->hspace->hspace_id
                                && (msg->src_id == svm->hproc_id
                                || msg->dest_id == svm->hproc_id)
                                && atomic_cmpxchg(&tx_e->used, 1, 2) == 1) {
                        struct heca_page_cache *dpc = tx_e->wrk_req->hpc;

                        dsm_pull_req_failure(dpc);
                        tx_e->wrk_req->dst_addr->mem_page = NULL;
                        dsm_release_pull_dpc(&dpc);
                        dsm_ppe_clear_release(ele, &tx_e->wrk_req->dst_addr);

                        /* rdma processing already finished, we have to release ourselves */
                        smp_mb();
                        if (atomic_read(&tx_e->used) > 2)
                                try_release_tx_element(ele, tx_e);
                }
        }
}

static void release_svm_queued_requests(struct heca_process *svm,
                struct tx_buffer *tx)
{
        struct heca_request *req, *n;
        u32 svm_id = svm->hproc_id;

        mutex_lock(&tx->flush_mutex);
        dsm_request_queue_merge(tx);
        list_for_each_entry_safe (req, n,
                        &tx->ordered_request_queue, ordered_list){
                if (req->remote_hproc_id == svm_id ||
                                req->local_hproc_id == svm_id) {
                        list_del(&req->ordered_list);
                        if (req->hpc && req->hpc->tag == PULL_TAG)
                                dsm_release_pull_dpc(&req->hpc);
                        release_dsm_request(req);
                }
        }
        mutex_unlock(&tx->flush_mutex);
}

void remove_svm(u32 dsm_id, u32 svm_id)
{
        struct heca_module_state *dsm_state = get_dsm_module_state();
        struct heca_space *dsm;
        struct heca_process *svm = NULL;

        mutex_lock(&dsm_state->heca_state_mutex);
        dsm = find_dsm(dsm_id);
        if (!dsm) {
                mutex_unlock(&dsm_state->heca_state_mutex);
                return;
        }

        mutex_lock(&dsm->hspace_mutex);
        svm = find_svm(dsm, svm_id);
        if (!svm) {
                mutex_unlock(&dsm_state->heca_state_mutex);
                goto out;
        }
        if (is_svm_local(svm)) {
                radix_tree_delete(&get_dsm_module_state()->mm_tree_root,
                                (unsigned long) svm->mm);
        }
        mutex_unlock(&dsm_state->heca_state_mutex);

        list_del(&svm->hproc_ptr);
        radix_tree_delete(&dsm->hprocs_tree_root, (unsigned long) svm->hproc_id);
        if (is_svm_local(svm)) {
                cancel_delayed_work_sync(&svm->delayed_gup_work);
                // to make sure everything is clean
                dequeue_and_gup_cleanup(svm);
                dsm->nb_local_hprocs--;
                radix_tree_delete(&dsm->hprocs_mm_tree_root,
                                (unsigned long) svm->mm);
        }

        remove_svm_from_descriptors(svm);

        /*
         * we removed the svm from all descriptors and trees, so we won't make any
         * new operations concerning it. now we only have to make sure to cancel
         * all pending operations involving this svm, and it will be safe to remove
         * it.
         *
         * we cannot actually hold until every operation is complete, so we rely on
         * refcounting. and yet we try to catch every operation, and be a surrogate
         * for it, if possible; otherwise we just trust it to drop the refcount when
         * it finishes. the main point is catching all operations, not leaving
         * anything unattended (thus creating a resource leak).
         *
         * we catch all pending operations using (by order) the queued requests
         * lists, the tx elements buffers, and the push caches of svms.
         *
         * FIXME: what about pull operations, in which we remove_svm() after
         * find_svm(), but before tx_dsm_send()??? We can't disable preemption
         * there, but we might lookup_svm() after we send, and handle the case in
         * which it isn't!
         * FIXME: the same problem is valid for push operations!
         */
        if (is_svm_local(svm)) {
                struct rb_root *root;
                struct rb_node *node;

                if (dsm_state->hcm) {
                        root = &dsm_state->hcm->connections_rb_tree_root;
                        for (node = rb_first(root);
                                        node; node = rb_next(node)) {
                                struct heca_connection_element *ele;

                                ele = rb_entry(node,
                                                struct heca_connection_element, rb_node);
                                BUG_ON(!ele);
                                release_svm_queued_requests(svm,
                                                &ele->tx_buffer);
                                release_svm_tx_elements(svm, ele);
                        }
                }
                release_svm_push_elements(svm);
                destroy_svm_mrs(svm);
        } else if (svm->connection) {
                struct heca_process *local_svm;

                release_svm_queued_requests(svm, &svm->connection->tx_buffer);
                release_svm_tx_elements(svm, svm->connection);

                /* potentially very expensive way to do this */
                list_for_each_entry (local_svm, &svm->hspace->hprocs_list, hproc_ptr) {
                        if (is_svm_local(local_svm))
                                surrogate_push_remote_svm(local_svm, svm);
                }
        }

        atomic_dec(&svm->refs);
        release_svm(svm);

out:
        mutex_unlock(&dsm->hspace_mutex);
}

struct heca_process *find_any_svm(struct heca_space *dsm, struct heca_process_list svms)
{
        int i;
        struct heca_process *svm;

        for_each_valid_hproc(svms, i) {
                svm = find_svm(dsm, svms.ids[i]);
                if (likely(svm))
                        return svm;
        }

        return NULL;
}


/*
 * memory_region funcs
 */
struct heca_memory_region *find_mr(struct heca_process *svm,
                u32 id)
{
        struct heca_memory_region *mr, **mrp;
        struct radix_tree_root *root;

        rcu_read_lock();
        root = &svm->hmr_id_tree_root;
repeat:
        mr = NULL;
        mrp = (struct heca_memory_region **) radix_tree_lookup_slot(root,
                        (unsigned long) id);
        if (mrp) {
                mr = radix_tree_deref_slot((void **) mrp);
                if (unlikely(!mr))
                        goto out;
                if (radix_tree_exception(mr)) {
                        if (radix_tree_deref_retry(mr))
                                goto repeat;
                }
        }
out:
        rcu_read_unlock();
        return mr;
}

struct heca_memory_region *search_mr_by_addr(struct heca_process *svm,
                unsigned long addr)
{
        struct rb_root *root = &svm->hmr_tree_root;
        struct rb_node *node;
        struct heca_memory_region *this = svm->hmr_cache;
        unsigned long seq;

        /* try to follow cache hint */
        if (likely(this)) {
                if (addr >= this->addr && addr < this->addr + this->sz)
                        goto out;
        }

        do {
                seq = read_seqbegin(&svm->hmr_seq_lock);
                for (node = root->rb_node; node; this = 0) {
                        this = rb_entry(node, struct heca_memory_region, rb_node);

                        if (addr < this->addr)
                                node = node->rb_left;
                        else if (addr > this->addr)
                                if (addr < (this->addr + this->sz))
                                        break;
                                else
                                        node = node->rb_right;
                        else
                                break;
                }
        } while (read_seqretry(&svm->hmr_seq_lock, seq));

        if (likely(this))
                svm->hmr_cache = this;

out:
        return this;
}

static int insert_mr(struct heca_process *svm, struct heca_memory_region *mr)
{
        struct rb_root *root = &svm->hmr_tree_root;
        struct rb_node **new = &root->rb_node, *parent = NULL;
        struct heca_memory_region *this;
        int r;

        r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
        if (r)
                goto fail;

        write_seqlock(&svm->hmr_seq_lock);

        /* insert to radix tree */
        r = radix_tree_insert(&svm->hmr_id_tree_root, (unsigned long) mr->hmr_id,
                        mr);
        if (r)
                goto out;

        /* insert to rb tree */
        while (*new) {
                this = rb_entry(*new, struct heca_memory_region, rb_node);
                parent = *new;
                if (mr->addr < this->addr)
                        new = &((*new)->rb_left);
                else if (mr->addr > this->addr)
                        new = &((*new)->rb_right);
        }

        rb_link_node(&mr->rb_node, parent, new);
        rb_insert_color(&mr->rb_node, root);
out:
        radix_tree_preload_end();
        write_sequnlock(&svm->hmr_seq_lock);
fail:
        return r;
}

static void destroy_svm_mrs(struct heca_process *svm)
{
        struct rb_root *root = &svm->hmr_tree_root;

        do {
                struct heca_memory_region *mr;
                struct rb_node *node;

                write_seqlock(&svm->hmr_seq_lock);
                node = rb_first(root);
                if (!node) {
                        write_sequnlock(&svm->hmr_seq_lock);
                        break;
                }
                mr = rb_entry(node, struct heca_memory_region, rb_node);
                rb_erase(&mr->rb_node, root);
                write_sequnlock(&svm->hmr_seq_lock);
                heca_printk(KERN_INFO "removing dsm_id: %u svm_id: %u, mr_id: %u",
                                svm->hspace->hspace_id, svm->hproc_id, mr->hmr_id);
                synchronize_rcu();
                kfree(mr);
        } while(1);
}

static struct heca_process *find_local_svm_from_list(struct heca_space *dsm)
{
        struct heca_process *tmp_svm;

        list_for_each_entry (tmp_svm, &dsm->hprocs_list, hproc_ptr) {
                if (!is_svm_local(tmp_svm))
                        continue;
                heca_printk(KERN_DEBUG "dsm %d local svm is %d", dsm->hspace_id,
                                tmp_svm->hproc_id);
                grab_svm(tmp_svm);
                return tmp_svm;
        }
        return NULL;
}

int create_mr(struct hecaioc_hmr *udata)
{
        int ret = 0, i;
        struct heca_space *dsm;
        struct heca_memory_region *mr = NULL;
        struct heca_process *local_svm = NULL;

        dsm = find_dsm(udata->hspace_id);
        if (!dsm) {
                heca_printk(KERN_ERR "can't find dsm %d", udata->hspace_id);
                ret = -EFAULT;
                goto out;
        }

        local_svm = find_local_svm_from_list(dsm);
        if (!local_svm) {
                heca_printk(KERN_ERR "can't find local svm for dsm %d",
                                udata->hspace_id);
                ret = -EFAULT;
                goto out;
        }

        /* FIXME: Validate against every kind of overlap! */
        if (search_mr_by_addr(local_svm, (unsigned long) udata->addr)) {
                heca_printk(KERN_ERR "mr already exists at addr 0x%lx",
                                udata->addr);
                ret = -EEXIST;
                goto out;
        }

        mr = kzalloc(sizeof(struct heca_memory_region), GFP_KERNEL);
        if (!mr) {
                heca_printk(KERN_ERR "can't allocate memory for MR");
                ret = -ENOMEM;
                goto out_free;
        }

        mr->hmr_id = udata->hmr_id;
        mr->addr = (unsigned long) udata->addr;
        mr->sz = udata->sz;

        if (insert_mr(local_svm, mr)){
                heca_printk(KERN_ERR "insert MR failed  addr 0x%lx",
                                udata->addr);
                ret = -EFAULT;
                goto out_free;
        }
        mr->descriptor = dsm_get_descriptor(dsm->hspace_id, udata->hproc_ids);
        if (!mr->descriptor) {
                heca_printk(KERN_ERR "can't find MR descriptor for svm_ids");
                ret = -EFAULT;
                goto out_remove_tree;
        }

        for (i = 0; udata->hproc_ids[i]; i++) {
                struct heca_process *owner;
                u32 svm_id = udata->hproc_ids[i];

                owner = find_svm(dsm, svm_id);
                if (!owner) {
                        heca_printk(KERN_ERR "[i=%d] can't find svm %d",
                                        i, svm_id);
                        ret = -EFAULT;
                        goto out_remove_tree;
                }

                if (is_svm_local(owner)) {
                        mr->flags |= MR_LOCAL;
                }

                release_svm(owner);
        }

        if (udata->flags & UD_COPY_ON_ACCESS) {
                mr->flags |= MR_COPY_ON_ACCESS;
                if (udata->flags & UD_SHARED)
                        goto out_remove_tree;
        } else if (udata->flags & UD_SHARED) {
                mr->flags |= MR_SHARED;
        }

        if (!(mr->flags & MR_LOCAL) && (udata->flags & UD_AUTO_UNMAP)) {
                ret = unmap_range(dsm, mr->descriptor, local_svm->pid, mr->addr,
                                mr->sz);
        }

        create_mr_sysfs_entry(local_svm, mr);
        goto out;

out_remove_tree:
        rb_erase(&mr->rb_node, &local_svm->hmr_tree_root);
out_free:
        kfree(mr);
out:
        if (local_svm)
                release_svm(local_svm);
        heca_printk(KERN_INFO "id [%d] addr [0x%lx] sz [0x%lx] --> ret %d",
                        udata->hmr_id, udata->addr, udata->sz, ret);
        return ret;
}

int unmap_ps(struct hecaioc_ps *udata)
{
        int r = -EFAULT;
        struct heca_space *dsm = NULL;
        struct heca_process *local_svm = NULL;
        struct heca_memory_region *mr = NULL;
        struct mm_struct *mm = find_mm_by_pid(udata->pid);

        if (!mm) {
                heca_printk(KERN_ERR "can't find pid %d", udata->pid);
                goto out;
        }

        local_svm = find_local_svm_from_mm(mm);
        if (!local_svm)
                goto out;

        dsm = local_svm->hspace;

        mr = search_mr_by_addr(local_svm, (unsigned long) udata->addr);
        if (!mr)
                goto out;

        r = unmap_range(dsm, mr->descriptor, udata->pid, (unsigned long)
                        udata->addr, udata->sz);

out:
        if (local_svm)
                release_svm(local_svm);
        return r;
}

int pushback_ps(struct hecaioc_ps *udata)
{
        int r = -EFAULT;
        unsigned long addr, start_addr;
        struct page *page;
        struct mm_struct *mm = find_mm_by_pid(udata->pid);

        if (!mm) {
                heca_printk(KERN_ERR "can't find pid %d", udata->pid);
                goto out;
        }

        addr = start_addr = ((unsigned long) udata->addr) & PAGE_MASK;
        for (addr = start_addr; addr < start_addr + udata->sz;
                        addr += PAGE_SIZE) {
                page = dsm_find_normal_page(mm, addr);
                if (!page || !trylock_page(page))
                        continue;

                r = !push_back_if_remote_dsm_page(page);
                if (r)
                        unlock_page(page);
        }

out:
        return r;
}

/*
 * rcm funcs
 */
int init_rcm(void)
{
        init_kmem_request_cache();
        init_kmem_deferred_gup_cache();
        init_dsm_cache_kmem();
        init_dsm_reader_kmem();
        init_dsm_prefetch_cache_kmem();
        dsm_init_descriptors();
        return 0;
}

int fini_rcm(void)
{
        destroy_dsm_cache_kmem();
        destroy_dsm_prefetch_cache_kmem();
        destroy_kmem_request_cache();
        destroy_kmem_deferred_gup_cache();
        dsm_destroy_descriptors();
        return 0;
}

int destroy_rcm_listener(struct heca_module_state *dsm_state);

int create_rcm_listener(struct heca_module_state *dsm_state, unsigned long ip,
                unsigned short port)
{
        int ret = 0;
        struct heca_connections_manager *rcm = kzalloc(sizeof(struct heca_connections_manager), GFP_KERNEL);

        if (!rcm)
                return -ENOMEM;

        mutex_init(&rcm->hcm_mutex);
        seqlock_init(&rcm->connections_lock);
        rcm->node_ip = ip;
        rcm->connections_rb_tree_root = RB_ROOT;

        rcm->cm_id = rdma_create_id(server_event_handler, rcm, RDMA_PS_TCP,
                        IB_QPT_RC);
        if (IS_ERR(rcm->cm_id)) {
                rcm->cm_id = NULL;
                ret = PTR_ERR(rcm->cm_id);
                heca_printk(KERN_ERR "Failed rdma_create_id: %d", ret);
                goto failed;
        }

        rcm->sin.sin_family = AF_INET;
        rcm->sin.sin_addr.s_addr = rcm->node_ip;
        rcm->sin.sin_port = port;

        ret = rdma_bind_addr(rcm->cm_id, (struct sockaddr *)&rcm->sin);
        if (ret) {
                heca_printk(KERN_ERR "Failed rdma_bind_addr: %d", ret);
                goto failed;
        }

        rcm->pd = ib_alloc_pd(rcm->cm_id->device);
        if (IS_ERR(rcm->pd)) {
                ret = PTR_ERR(rcm->pd);
                rcm->pd = NULL;
                heca_printk(KERN_ERR "Failed id_alloc_pd: %d", ret);
                goto failed;
        }

        rcm->listen_cq = ib_create_cq(rcm->cm_id->device, listener_cq_handle,
                        NULL, rcm, 2, 0);
        if (IS_ERR(rcm->listen_cq)) {
                ret = PTR_ERR(rcm->listen_cq);
                rcm->listen_cq = NULL;
                heca_printk(KERN_ERR "Failed ib_create_cq: %d", ret);
                goto failed;
        }

        if ((ret = ib_req_notify_cq(rcm->listen_cq, IB_CQ_NEXT_COMP))) {
                heca_printk(KERN_ERR "Failed ib_req_notify_cq: %d", ret);
                goto failed;
        }

        rcm->mr = ib_get_dma_mr(rcm->pd, IB_ACCESS_LOCAL_WRITE |
                        IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE);
        if (IS_ERR(rcm->mr)) {
                ret = PTR_ERR(rcm->mr);
                rcm->mr = NULL;
                heca_printk(KERN_ERR "Failed ib_get_dma_mr: %d", ret);
                goto failed;
        }

        dsm_state->hcm = rcm;

        ret = rdma_listen(rcm->cm_id, 2);
        if (ret)
                heca_printk(KERN_ERR "Failed rdma_listen: %d", ret);
        return 0;

failed:
        destroy_rcm_listener(dsm_state);
        return ret;
}

static int rcm_disconnect(struct heca_connections_manager *rcm)
{
        struct rb_root *root = &rcm->connections_rb_tree_root;
        struct rb_node *node = rb_first(root);
        struct heca_connection_element *ele;

        while (node) {
                ele = rb_entry(node, struct heca_connection_element, rb_node);
                node = rb_next(node);
                if (atomic_cmpxchg(&ele->alive, 1, 0)) {
                        rdma_disconnect(ele->cm_id);
                        destroy_connection(ele);
                }
        }

        while (rb_first(root))
                ;

        return 0;
}

int destroy_rcm_listener(struct heca_module_state *dsm_state)
{
        int rc = 0;
        struct heca_connections_manager *rcm = dsm_state->hcm;

        heca_printk(KERN_DEBUG "<enter>");

        if (!rcm)
                goto done;

        if (!list_empty(&dsm_state->hspaces_list)) {
                heca_printk(KERN_INFO "can't delete rcm - dsms exist");
                rc = -EBUSY;
        }

        rcm_disconnect(rcm);

        if (!rcm->cm_id)
                goto destroy;

        if (rcm->cm_id->qp) {
                ib_destroy_qp(rcm->cm_id->qp);
                rcm->cm_id->qp = NULL;
        }

        if (rcm->listen_cq) {
                ib_destroy_cq(rcm->listen_cq);
                rcm->listen_cq = NULL;
        }

        if (rcm->mr) {
                ib_dereg_mr(rcm->mr);
                rcm->mr = NULL;
        }

        if (rcm->pd) {
                ib_dealloc_pd(rcm->pd);
                rcm->pd = NULL;
        }

        rdma_destroy_id(rcm->cm_id);
        rcm->cm_id = NULL;

destroy:
        mutex_destroy(&rcm->hcm_mutex);
        kfree(rcm);
        dsm_state->hcm = NULL;

done:
        heca_printk(KERN_DEBUG "<exit> %d", rc);
        return rc;
}


