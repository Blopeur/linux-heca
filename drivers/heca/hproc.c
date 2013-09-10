/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */
#include <linux/rcupdate.h>
#include <linux/errno.h>
#include <linux/pagemap.h>

#include "transport.h"
#include "hecatonchire.h"
#include "hproc.h"
#include "hutils.h"

#include "base.h"
#include "task.h"
#include "pull.h"
#include "trace.h"
#include "ops.h"
#include "push.h"
#include "conn.h"

#define HPROC_KOBJECT           "%u"
#define HMRS_KSET               "memory_regions"

#define to_hproc(p)             container_of(p, struct heca_process, kobj)
#define to_hproc_attr(pa)       container_of(pa, struct hproc_attr, attr)

/*
 * Hproc refcount
 */

struct heca_process *hproc_get(struct heca_process *hproc)
{
        if(hproc)
                if(kobject_get(&hproc->kobj))
                        return hproc;
        return NULL;
}

struct heca_process * __must_check hproc_get_unless_zero(
                struct heca_process *hproc)
{
        if(hproc)
                if(kref_get_unless_zero(&hproc->kobj.kref))
                        return hproc;
        return NULL;
}

void hproc_put(struct heca_process *hproc)
{
        if(hproc)
                kobject_put(&hproc->kobj);
}

/*
 * Heca proc  Kobject
 */

struct hproc_attr {
        struct attribute attr;
        ssize_t(*show)(struct heca_process *, char *);
        ssize_t(*store)(struct heca_process *, char *, size_t);
};

static void kobj_hproc_release(struct kobject *k)
{
        struct heca_process *hproc = to_hproc(k);
        heca_printk(KERN_INFO "releasing hproc %p, hproc_id: %u hspace_id: %u ",
                        hproc, hproc->hproc_id, hproc->hspace->hspace_id );
        trace_heca_free_hproc(hproc->hproc_id);
        synchronize_rcu();
        kfree(hproc);
}

static ssize_t hproc_show(struct kobject *k, struct attribute *a,
                char *buffer)
{
        struct heca_process *hproc = to_hproc(k);
        struct hproc_attr *hproc_attr = to_hproc_attr(a);
        if (hproc_attr->show)
                return hproc_attr->show(hproc,buffer);
        return 0;
}

static struct hproc_attr *hproc_attr[] = {
        NULL
};

static struct sysfs_ops hproc_ops = {
        .show = hproc_show,
};

static struct kobj_type ktype_hproc = {
        .release = kobj_hproc_release,
        .sysfs_ops = &hproc_ops,
        .default_attrs = (struct attribute **) hproc_attr,
};


inline int is_hproc_local(struct heca_process *hproc)
{
        return !!hproc->mm;
}

static struct heca_process *_find_hproc_in_tree(
                struct radix_tree_root *root, unsigned long hproc_id)
{
        struct heca_process *hproc;
        struct heca_process **hprocp;

        rcu_read_lock();
repeat:
        hproc = NULL;
        hprocp = (struct heca_process **) radix_tree_lookup_slot(root,
                        (unsigned long) hproc_id);
        if (hprocp) {
                hproc = radix_tree_deref_slot((void**) hprocp);
                if (unlikely(!hproc))
                        goto out;
                if (radix_tree_exception(hproc)) {
                        if (radix_tree_deref_retry(hproc))
                                goto repeat;
                }

                if (!hproc_get_unless_zero(hproc))
                        goto repeat;

        }

out:
        rcu_read_unlock();
        return hproc;
}

inline struct heca_process *find_hproc(struct heca_space *hspace, u32 hproc_id)
{
        return _find_hproc_in_tree(&hspace->hprocs_tree_root,
                        (unsigned long) hproc_id);
}

inline struct heca_process *find_local_hproc_in_hspace(
                struct heca_space *hspace, struct mm_struct *mm)
{
        return _find_hproc_in_tree(&hspace->hprocs_mm_tree_root,
                        (unsigned long) mm);
}

inline struct heca_process *find_local_hproc_from_mm(struct mm_struct *mm)
{
        struct heca_module_state *mod = get_heca_module_state();

        return (likely(mod)) ?
                _find_hproc_in_tree(&mod->mm_tree_root, (unsigned long) mm) :
                NULL;
}

/*
 * We dec page's refcount for every missing remote response (it would have
 * happened in hspace_ppe_clear_release after sending an answer to remote hproc)
 */
static void surrogate_push_remote_hproc(struct heca_process *hproc,
                struct heca_process *remote_hproc)
{
        struct rb_node *node;

        write_seqlock(&hproc->push_cache_lock);
        for (node = rb_first(&hproc->push_cache); node;) {
                struct heca_page_cache *hpc;
                int i;
                hpc = rb_entry(node, struct heca_page_cache, rb_node);
                node = rb_next(node);
                for (i = 0; i < hpc->hprocs.num; i++) {
                        if (hpc->hprocs.ids[i] == remote_hproc->hproc_id)
                                goto surrogate;
                }
                continue;

surrogate:
                if (likely(test_and_clear_bit(i, &hpc->bitmap))) {
                        page_cache_release(hpc->pages[0]);
                        atomic_dec(&hpc->nproc);
                        if (atomic_cmpxchg(&hpc->nproc, 1, 0) == 1 &&
                                        find_first_bit(&hpc->bitmap,
                                                hpc->hprocs.num) >=
                                        hpc->hprocs.num)
                                heca_push_cache_release(hpc->hproc, &hpc, 0);
                }
        }
        write_sequnlock(&hproc->push_cache_lock);
}

static void release_hproc_push_elements(struct heca_process *hproc)
{
        struct rb_node *node;

        write_seqlock(&hproc->push_cache_lock);
        for (node = rb_first(&hproc->push_cache); node;) {
                struct heca_page_cache *hpc;
                int i;

                hpc = rb_entry(node, struct heca_page_cache, rb_node);
                node = rb_next(node);
                /*
                 * dpc->hprocs has a pointer to the descriptor ids array, which already
                 * changed. we need to rely on the bitmap right now.
                 */
                for (i = 0; i < hpc->hprocs.num; i++) {
                        if (test_and_clear_bit(i, &hpc->bitmap))
                                page_cache_release(hpc->pages[0]);
                }
                heca_push_cache_release(hpc->hproc, &hpc, 0);
        }
        write_sequnlock(&hproc->push_cache_lock);
}

/*
 * pull ops tx_elements are only released after a response has returned.
 * therefore we can catch them and surrogate for them by iterating the tx
 * buffer.
 */
static void release_hproc_tx_elements(struct heca_process *hproc,
                struct heca_connection *conn)
{
        struct tx_buffer_element *tx_buf;
        int i;

        /* killed before it was first connected */
        if (!conn || !conn->tx_buffer.tx_buf)
                return;

        tx_buf = conn->tx_buffer.tx_buf;

        for (i = 0; i < conn->tx_buffer.len; i++) {
                struct tx_buffer_element *tx_e = &tx_buf[i];
                struct heca_message *msg = tx_e->hmsg_buffer;
                int types = MSG_REQ_PAGE | MSG_REQ_PUSHED_PAGE |
                        MSG_RES_PAGE_FAIL | MSG_REQ_READ;

                if (msg->type & types
                                && msg->hspace_id == hproc->hspace->hspace_id
                                && (msg->src_id == hproc->hproc_id
                                        || msg->dest_id == hproc->hproc_id)
                                && atomic_cmpxchg(&tx_e->used, 1, 2) == 1) {
                        struct heca_page_cache *dpc = tx_e->wrk_req->hpc;

                        heca_pull_req_failure(dpc);
                        tx_e->wrk_req->dst_addr->mem_page = NULL;
                        heca_release_pull_hpc(&dpc);
                        heca_ppe_clear_release(conn, &tx_e->wrk_req->dst_addr);

                        /* rdma processing already finished, we have to release ourselves */
                        smp_mb();
                        if (atomic_read(&tx_e->used) > 2)
                                try_release_tx_element(conn, tx_e);
                }
        }
}

static void release_hproc_queued_requests(struct heca_process *hproc,
                struct tx_buffer *tx)
{
        struct heca_request *req, *n;
        u32 hproc_id = hproc->hproc_id;

        mutex_lock(&tx->flush_mutex);
        heca_request_queue_merge(tx);
        list_for_each_entry_safe (req, n,
                        &tx->ordered_request_queue, ordered_list){
                if (req->remote_hproc_id == hproc_id ||
                                req->local_hproc_id == hproc_id) {
                        list_del(&req->ordered_list);
                        if (req->hpc && req->hpc->tag == PULL_TAG)
                                heca_release_pull_hpc(&req->hpc);
                        release_heca_request(req);
                }
        }
        mutex_unlock(&tx->flush_mutex);
}

static void destroy_hproc_mrs(struct heca_process *hproc)
{
        struct rb_root *root = &hproc->hmr_tree_root;

        heca_printk(KERN_INFO "Releasing MRs of hproc: %p , id: %u ", hproc,
                        hproc->hproc_id);
        do {
                struct heca_memory_region *mr;
                struct rb_node *node;

                write_seqlock(&hproc->hmr_seq_lock);
                node = rb_first(root);
                if (!node) {
                        write_sequnlock(&hproc->hmr_seq_lock);
                        break;
                }
                mr = rb_entry(node, struct heca_memory_region, rb_node);
                mr = hmr_get_unless_zero(mr);
                if(mr){
                        write_sequnlock(&hproc->hmr_seq_lock);
                        teardown_heca_memory_region(hproc, mr);
                }
                else
                        write_sequnlock(&hproc->hmr_seq_lock);
        } while(1);

        kset_unregister(hproc->hmrs_kset);
}

static inline void release_local_hproc(struct heca_process *hproc)
{
        struct heca_module_state *heca_state = get_heca_module_state();
        struct rb_root *root;
        struct rb_node *node;
       /*FIXME : do we need to protect the scan ?*/
        if (heca_state->htm) {
                root = &heca_state->htm->connections_rb_tree_root;
                for (node = rb_first(root);
                                node; node = rb_next(node)) {
                        struct heca_connection *conn;

                        conn = rb_entry(node,
                                        struct heca_connection,
                                        rb_node);
                        BUG_ON(!conn);
                        release_hproc_queued_requests(hproc,
                                        &conn->tx_buffer);
                        release_hproc_tx_elements(hproc, conn);
                }
        }
        release_hproc_push_elements(hproc);
        destroy_hproc_mrs(hproc);
}

static inline void release_connection_remote_hproc(struct heca_space *hspace,
                struct heca_process *hproc)
{
        if (hproc->connection) {
                struct heca_process *local_hproc;

                release_hproc_queued_requests(hproc,
                                &hproc->connection->tx_buffer);
                release_hproc_tx_elements(hproc, hproc->connection);

                /* potentially very expensive way to do this */
                list_for_each_entry (local_hproc, &hspace->hprocs_list,
                                hproc_ptr) {
                        if (is_hproc_local(local_hproc))
                                surrogate_push_remote_hproc(local_hproc, hproc);
                }

        }
}


/* this function is also call with the hspace mutex held */
static inline int hproc_exist(struct heca_space *hspace, u32 id)
{
        struct heca_process *local_hproc;
        list_for_each_entry (local_hproc, &hspace->hprocs_list, hproc_ptr) {
                if (local_hproc->hproc_id == id)
                        return 0;
        }
        return 1;
}

/* the hspace_mutex needs to be take to execute this function */
static inline void deregister_hproc(struct heca_module_state *heca_state,
                struct heca_space *hspace, struct heca_process *hproc)
{
        radix_tree_delete(&hspace->hprocs_tree_root,
                        (unsigned long) hproc->hproc_id);
        if (is_hproc_local(hproc)) {
                cancel_delayed_work_sync(&hproc->delayed_gup_work);
                // to make sure everything is clean
                dequeue_and_gup_cleanup(hproc);
                radix_tree_delete(&hspace->hprocs_mm_tree_root,
                                (unsigned long) hproc->mm);
                mutex_lock(&heca_state->heca_state_mutex);
                radix_tree_delete(&heca_state->mm_tree_root,
                                (unsigned long) hproc->mm);
                mutex_unlock(&heca_state->heca_state_mutex);
        }

        remove_hproc_from_descriptors(hproc);
        list_del(&hproc->hproc_ptr);
}

static inline int register_hproc(struct heca_module_state *heca_state,
                struct heca_space *hspace, struct heca_process *new_hproc)
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


        r = radix_tree_insert(&hspace->hprocs_tree_root,
                        (unsigned long) new_hproc->hproc_id, new_hproc);
        if (r)
                goto preload_out;

        if (is_hproc_local(new_hproc)) {
                r = radix_tree_insert(&hspace->hprocs_mm_tree_root,
                                (unsigned long) new_hproc->mm, new_hproc);
                if(r)
                        goto hspace_mm_fail;
                mutex_lock(&heca_state->heca_state_mutex);
                r = radix_tree_insert(&heca_state->mm_tree_root,
                                (unsigned long) new_hproc->mm, new_hproc);
                mutex_unlock(&heca_state->heca_state_mutex);
                if(r)
                        goto hstate_mm_fail;
        }

        list_add(&new_hproc->hproc_ptr, &hspace->hprocs_list);
        radix_tree_preload_end();
        return r;

hstate_mm_fail:

        radix_tree_delete(&hspace->hprocs_mm_tree_root,
                        (unsigned long) new_hproc->mm);
hspace_mm_fail:

        radix_tree_delete(&hspace->hprocs_tree_root,
                        (unsigned long) new_hproc->hproc_id);
preload_out:
        radix_tree_preload_end();
out:
        return r;
}

int create_hproc(struct hecaioc_hproc *hproc_info)
{
        struct heca_module_state *heca_state = get_heca_module_state();
        int r = 0;
        struct heca_space *hspace;
        struct heca_process *new_hproc = NULL;

        /* allocate a new hproc */
        new_hproc = kzalloc(sizeof(*new_hproc), GFP_KERNEL);
        if (!new_hproc) {
                heca_printk(KERN_ERR "failed kzalloc");
                return -ENOMEM;
        }

        mutex_lock(&hspace->hspace_mutex);

        /* already exists? */
        if(!hproc_exist(hspace, hproc_info->hproc_id)){
                r = -EEXIST;
                goto hproc_exist;
        }

        /* initial hproc data */
        new_hproc->hproc_id = hproc_info->hproc_id;
        new_hproc->is_local = hproc_info->is_local;
        new_hproc->pid = hproc_info->pid;
        new_hproc->hspace = hspace;
        /* assign descriptor for remote hproc */
        if (!is_hproc_local(new_hproc)) {
                u32 hproc_ids[] = {new_hproc->hproc_id, 0};
                r = connect_hproc(hproc_info->hspace_id, hproc_info->hproc_id,
                                hproc_info->remote.sin_addr.s_addr,
                                hproc_info->remote.sin_port);
                if (r) {
                        goto connect_fail;
                }
                new_hproc->descriptor = heca_get_descriptor(hspace->hspace_id,
                                hproc_ids);
        } else {

                struct mm_struct *mm;

                mm = find_mm_by_pid(new_hproc->pid);
                if (!mm) {
                        heca_printk(KERN_ERR "can't find pid %d",
                                        new_hproc->pid);
                        r = -ESRCH;
                        goto no_mm;
                }
                new_hproc->mm = mm;
                new_hproc->hmr_tree_root = RB_ROOT;
                seqlock_init(&new_hproc->hmr_seq_lock);
                new_hproc->hmr_cache = NULL;

                init_llist_head(&new_hproc->delayed_gup);
                INIT_DELAYED_WORK(&new_hproc->delayed_gup_work,
                                delayed_gup_work_fn);
                init_llist_head(&new_hproc->deferred_gups);
                INIT_WORK(&new_hproc->deferred_gup_work, deferred_gup_work_fn);

                spin_lock_init(&new_hproc->page_cache_spinlock);
                spin_lock_init(&new_hproc->page_readers_spinlock);
                spin_lock_init(&new_hproc->page_maintainers_spinlock);
                INIT_RADIX_TREE(&new_hproc->page_cache, GFP_ATOMIC);
                INIT_RADIX_TREE(&new_hproc->page_readers, GFP_ATOMIC);
                INIT_RADIX_TREE(&new_hproc->page_maintainers, GFP_ATOMIC);
                new_hproc->push_cache = RB_ROOT;
                seqlock_init(&new_hproc->push_cache_lock);
        }

        /* register hproc by id and mm_struct (must come before hspace_get_descriptor) */
        r = register_hproc(heca_state, hspace, new_hproc);
        if (r)
                goto reg_fail;

        /* SYFS stuff*/
        new_hproc->kobj.kset = hspace->hprocs_kset;
        r = kobject_init_and_add(&new_hproc->kobj, &ktype_hproc, NULL,
                        HPROC_KOBJECT, new_hproc->hproc_id);
        if(r){
                goto kobj_err;
        }
        new_hproc->hmrs_kset = kset_create_and_add(HMRS_KSET, NULL,
                        &new_hproc->kobj);
        if(!new_hproc->hmrs_kset)
                goto kset_fail;
        mutex_unlock(&hspace->hspace_mutex);

        heca_printk(KERN_INFO "hproc %p, res %d, hspace_id %u, hproc_id: %u --> ret %d",
                        new_hproc, r, hproc_info->hspace_id,
                        hproc_info->hproc_id, r);
        return r;

hproc_exist:
connect_fail:
no_mm:
        mutex_unlock(&hspace->hspace_mutex);
        kfree(new_hproc);
        new_hproc = NULL;
        return r;
reg_fail:
        mutex_unlock(&hspace->hspace_mutex);
        if(!is_hproc_local(new_hproc)){
                release_connection_remote_hproc(hspace, new_hproc);
                remove_hproc_from_descriptors(new_hproc);
        }

        kfree(new_hproc);
        new_hproc = NULL;
        return r;

kset_fail:
        kobject_del(&new_hproc->kobj);
kobj_err:
        deregister_hproc(heca_state, hspace, new_hproc);
        mutex_unlock(&hspace->hspace_mutex);
        if(!is_hproc_local(new_hproc))
        {
                release_connection_remote_hproc(hspace, new_hproc);
                remove_hproc_from_descriptors(new_hproc);
        }
        kobject_put(&new_hproc->kobj);
        return r;
}


static int remove_hproc(struct heca_space *hspace, struct heca_process *hproc)
{

        struct heca_module_state *heca_state = get_heca_module_state();
        mutex_lock(&hspace->hspace_mutex);
        if(hproc_exist(hspace, hproc->hproc_id)){
                mutex_unlock(&hspace->hspace_mutex);
                return -1;
        }
        deregister_hproc(heca_state, hspace, hproc);
        /*
         * we removed the hproc from all descriptors and trees, so we won't make any
         * new operations concerning it. now we only have to make sure to cancel
         * all pending operations involving this hproc, and it will be safe to remove
         * it.
         *
         * we cannot actually hold until every operation is complete, so we rely on
         * refcounting. and yet we try to catch every operation, and be a surrogate
         * for it, if possible; otherwise we just trust it to drop the refcount when
         * it finishes. the main point is catching all operations, not leaving
         * anything unattended (thus creating a resource leak).
         *
         * we catch all pending operations using (by order) the queued requests
         * lists, the tx elements buffers, and the push caches of hprocs.
         *
         * FIXME: what about pull operations, in which we remove_hproc() after
         * find_hproc(), but before tx_hspace_send()??? We can't disable preemption
         * there, but we might lookup_hproc() after we send, and handle the case in
         * which it isn't!
         * FIXME: the same problem is valid for push operations!
         */
        if (is_hproc_local(hproc))
                release_local_hproc(hproc);
        else
                release_connection_remote_hproc(hspace, hproc);

        /* we remove the kobject entry */
        mutex_unlock(&hspace->hspace_mutex);
        kobject_del(&hproc->kobj);
        return 0;
}


struct heca_process *find_any_hproc(struct heca_space *hspace,
                struct heca_process_list hprocs)
{
        int i;
        struct heca_process *hproc;

        for_each_valid_hproc(hprocs, i) {
                hproc = find_hproc(hspace, hprocs.ids[i]);
                if (likely(hproc))
                        return hproc;
        }

        return NULL;
}


/* FIXME:this return the first local hproc , not all of them ...*/
struct heca_process *find_local_hproc_from_list(struct heca_space *hspace)
{
        struct heca_process *tmp_hproc;

        list_for_each_entry (tmp_hproc, &hspace->hprocs_list, hproc_ptr) {
                if (!is_hproc_local(tmp_hproc))
                        continue;
                tmp_hproc = hproc_get_unless_zero(tmp_hproc);
                return tmp_hproc;
        }
        return NULL;
}
/*
 * Teardown operation
 */

void  teardown_hproc(struct heca_space *hspace, struct heca_process *hproc){
        /* cleanup the hproc */
        if(!remove_hproc(hspace, hproc))
                kobject_put(&hproc->kobj);
        /* final put for releasing the object*/
        kobject_put(&hproc->kobj);

}

void teardown_hproc_by_id(u32 hspace_id, u32 hproc_id)
{
        struct heca_space *hspace;
        struct heca_process *hproc = NULL;

        hspace = find_hspace(hspace_id);
        if (!hspace) {
                return;
        }
        hproc = find_hproc(hspace, hproc_id);
        if (hproc) {
                teardown_hproc(hspace, hproc);
        }


}
