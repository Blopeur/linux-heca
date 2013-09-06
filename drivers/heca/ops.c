/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 */
#include <linux/pagemap.h>

#include "hutils.h"
#include "hproc.h"


#include "ioctl.h"
#include "trace.h"
#include "struct.h"
#include "ops.h"
#include "pull.h"
#include "push.h"
#include "conn.h"
#include "base.h"
#include "task.h"

/*
 * send an rdma message. if a tx_e is available, prepare it according to the
 * arguments and send the message. otherwise, try and queue the request with
 * the same args. if not enough mem to queue the request, we have no choice but
 * to reschedule, in hope an existing tx_e will push a page and free mem. but in
 * this case, when we wake, we might find an available tx_e.
 *
 * note: message ordering is not guaranteed. while previous requests are being
 * flushed, a new msg might take a tx element that has been released in the
 * meanwhile.
 */
static int heca_send_msg(struct heca_connection *ele, u32 hspace_id, u32 mr_id,
                u32 src_id, u32 dest_id, unsigned long local_addr,
                unsigned long shared_addr, struct page *page, int type,
                int (*func)(struct tx_buffer_element *),
                struct heca_page_cache *hpc, struct heca_page_pool_element *ppe,
                struct heca_message *msg, int need_ppe)
{
        struct tx_buffer_element *tx_e = NULL;

        might_sleep();
        while (1) {
                tx_e = try_get_next_empty_tx_ele(ele, 1);
                if (likely(tx_e)) {
                        return heca_send_tx_e(ele, tx_e, !!msg, type,
                                        hspace_id, mr_id, src_id, dest_id,
                                        local_addr, shared_addr, hpc, page,
                                        ppe, need_ppe, func, msg);
                }

                if (!add_heca_request(NULL, ele, type, hspace_id, src_id, mr_id,
                                        dest_id, shared_addr, func, hpc, page,
                                        ppe, need_ppe, msg)) {
                        return 1;
                }

                cond_resched();
        }
}

/*
 * same as hspace_send_msg, only with different preparation of the tx_e, and
 * different method of queueing the args. hspace_send_tx_e receives response=1.
 */
static int heca_send_response(struct heca_connection *conn, int type,
                struct heca_message *msg)
{
        return heca_send_msg(conn, msg->hspace_id, msg->mr_id,
                        msg->src_id, msg->dest_id, 0,
                        msg->req_addr, NULL, type, NULL, NULL, NULL, msg, 0);
}

static struct kmem_cache *kmem_deferred_gup_cache;

static inline void init_kmem_deferred_gup_cache_elm(void *obj)
{
        struct heca_deferred_gup *dgup = (struct heca_deferred_gup *) obj;
        memset(dgup, 0, sizeof(struct heca_deferred_gup));
}

void init_kmem_deferred_gup_cache(void)
{
        kmem_deferred_gup_cache = kmem_cache_create("kmem_deferred_gup_cache",
                        sizeof(struct heca_deferred_gup), 0,
                        SLAB_HWCACHE_ALIGN | SLAB_TEMPORARY,
                        init_kmem_deferred_gup_cache_elm);
}

void destroy_kmem_deferred_gup_cache(void)
{
        kmem_cache_destroy(kmem_deferred_gup_cache);
}

static void release_kmem_deferred_gup_cache_elm(struct heca_deferred_gup *req)
{
        kmem_cache_free(kmem_deferred_gup_cache, req);
}

static int send_request_heca_page_pull(struct heca_process *fault_hproc,
                struct heca_memory_region *fault_mr,
                struct heca_process_list hprocs, unsigned long addr)
{
        struct tx_buffer_element *tx_elms[hprocs.num];
        struct heca_request *reqs[hprocs.num];
        struct heca_connection *cons[hprocs.num];
        int i, j, r = 0;

        for_each_valid_hproc(hprocs, i) {
                struct heca_process *hproc;

                reqs[i] = NULL;
                tx_elms[i] = NULL;

                hproc = find_hproc(fault_hproc->hspace, hprocs.ids[i]);
                if (unlikely(!hproc))
                        continue;

                cons[i] = hproc->connection;
                hproc_put(hproc);

                tx_elms[i] = try_get_next_empty_tx_ele(cons[i], 1);
                if (unlikely(!tx_elms[i])) {
                        reqs[i] = alloc_heca_request();
                        if (unlikely(!reqs[i]))
                                goto nomem;
                }
        }

        /*
         * we have to iterate all hprocs, and rely on tx_elms or reqs, since some
         * might have been dropped since the previous iteration.
         */
        might_sleep();
        for (i = 0; i < hprocs.num; i++) {
                if (tx_elms[i]) {
                        /* note that dest_id == local_hproc */
                        r |= heca_send_tx_e(cons[i], tx_elms[i], 0,
                                        MSG_REQ_PUSH,
                                        fault_hproc->hspace->hspace_id,
                                        fault_mr->hmr_id, hprocs.ids[i],
                                        fault_hproc->hproc_id,
                                        addr + fault_mr->addr, addr, NULL, NULL,
                                        NULL, 0, NULL, NULL);
                } else if (reqs[i]) {
                        /* can't fail, reqs[i] already allocated */
                        j = add_heca_request(reqs[i], cons[i],
                                        MSG_REQ_PUSH,
                                        fault_hproc->hspace->hspace_id,
                                        hprocs.ids[i], fault_mr->hmr_id,
                                        fault_hproc->hproc_id, addr, NULL, NULL,
                                        NULL, NULL, 0, NULL);
                        BUG_ON(j);
                }
        }

        return r;

nomem:
        for (j = 0; j < i; j++) {
                if (tx_elms[j])
                        release_tx_element(cons[j], tx_elms[j]);
                else if (reqs[j])
                        release_heca_request(reqs[j]);
        }
        return -ENOMEM;
}

static int send_hproc_status_update(struct heca_connection *conn,
                struct heca_message *msg)
{
        return heca_send_response(conn, MSG_RES_HPROC_FAIL, msg);
}

static int heca_request_query(struct heca_process *hproc,
                struct heca_process *owner, struct heca_memory_region *mr,
                unsigned long shared_addr, struct heca_page_cache *hpc)
{
        return heca_send_msg(owner->connection, hproc->hspace->hspace_id,
                        mr->hmr_id, hproc->hproc_id, owner->hproc_id,
                        shared_addr + mr->addr, shared_addr, NULL,
                        MSG_REQ_QUERY, process_query_info, hpc, NULL, NULL, 0);
}

/*
 * request another node to make sure it registers the address as belonging to
 * us. the only_unmap flag means that we will continue sending the request until
 * a page will actually be unmapped. without the flag, we will be content with
 * only changing the pte on the other side to point to us.
 */
int heca_claim_page(struct heca_process *fault_hproc,
                struct heca_process *remote_hproc,
                struct heca_memory_region *fault_mr, unsigned long addr,
                struct page *page, int only_unmap)
{
        u32 type = only_unmap? MSG_REQ_CLAIM : MSG_REQ_CLAIM_TRY;

        trace_heca_claim_page(fault_hproc->hspace->hspace_id,
                        fault_hproc->hproc_id, remote_hproc->hproc_id,
                        fault_mr->hmr_id, addr, addr - fault_mr->addr, type);

        return heca_send_msg(remote_hproc->connection,
                        fault_hproc->hspace->hspace_id, fault_mr->hmr_id,
                        fault_hproc->hproc_id, remote_hproc->hproc_id,
                        addr, addr - fault_mr->addr, page, type,
                        NULL, NULL, NULL, NULL, 0);
}

int heca_request_page(struct page *page, struct heca_process *remote_hproc,
                struct heca_process *fault_hproc,
                struct heca_memory_region *fault_mr, unsigned long addr,
                int (*func)(struct tx_buffer_element *), int tag,
                struct heca_page_cache *hpc, struct heca_page_pool_element *ppe)
{
        int type;

        switch (tag) {
        case PUSH_RES_TAG:
                type = MSG_REQ_PUSHED_PAGE;
                break;
        case READ_TAG:
                type = MSG_REQ_READ;
                break;
        case PULL_TAG:
                type = MSG_REQ_PAGE;
                break;
        case PREFETCH_TAG:
                type = (fault_mr->flags & MR_SHARED)?
                        MSG_REQ_READ : MSG_REQ_PAGE;
                break;
        default:
                BUG();
        }

        /* note that src_id == remote_id, and dest_id == local_id */
        return heca_send_msg(remote_hproc->connection,
                        fault_hproc->hspace->hspace_id, fault_mr->hmr_id,
                        remote_hproc->hproc_id, fault_hproc->hproc_id,
                        addr, addr - fault_mr->addr, page, type, func, hpc, ppe,
                        NULL, 1);
}

int process_request_query(struct heca_connection *conn,
                struct rx_buffer_element *rx_e)
{
        struct heca_message *msg = rx_e->hmsg_buffer;
        struct heca_space *hspace;
        struct heca_process *hproc;
        struct heca_memory_region *mr;
        int r = -EFAULT;
        unsigned long addr;

        hspace = find_hspace(msg->hspace_id);
        if (unlikely(!hspace))
                goto fail;

        hproc = find_hproc(hspace, msg->dest_id);
        if (unlikely(!hproc))
                goto fail;

        mr = find_heca_mr(hproc, msg->mr_id);
        if (unlikely(!mr))
                goto out;

        addr = msg->req_addr + mr->addr;

        /* this cannot fail: if we don't have a valid hspace pte, the page is ours */
        msg->dest_id = heca_query_pte_info(hproc, addr);

        r = heca_send_response(conn, MSG_RES_QUERY, msg);

out:
        hproc_put(hproc);
fail:
        return r;
}

int process_query_info(struct tx_buffer_element *tx_e)
{
        struct heca_message *msg = tx_e->hmsg_buffer;
        struct heca_space *hspace;
        struct heca_process *hproc;
        struct heca_page_cache *hpc;
        struct heca_memory_region *mr;
        unsigned long addr;
        int r = -EFAULT;

        hspace = find_hspace(msg->hspace_id);
        if (!hspace)
                goto fail;

        hproc = find_hproc(hspace, msg->src_id);
        if (!hproc)
                goto fail;

        mr = find_heca_mr(hproc, msg->mr_id);
        if (!mr)
                goto out;

        addr = msg->req_addr + mr->addr;
        hpc = heca_cache_get_hold(hproc, addr);
        if (likely(hpc)) {
                if (likely(hpc == tx_e->wrk_req->hpc))
                        hpc->redirect_hproc_id = msg->dest_id;
                heca_release_pull_hpc(&hpc);
        }
        r = 0;

out:
        hproc_put(hproc);
fail:
        return r;
}

int process_pull_request(struct heca_connection *conn,
                struct rx_buffer_element *rx_buf_e)
{
        struct heca_process *local_hproc;
        struct heca_space *hspace;
        struct heca_message *msg;
        struct heca_memory_region *mr;
        int r = 0;

        BUG_ON(!rx_buf_e);
        BUG_ON(!rx_buf_e->hmsg_buffer);
        msg = rx_buf_e->hmsg_buffer;

        hspace = find_hspace(msg->hspace_id);
        if (unlikely(!hspace))
                goto fail;

        local_hproc = find_hproc(hspace, msg->src_id);
        if (unlikely(!local_hproc || !local_hproc->mm))
                goto fail;

        /* push only happens to mr owners! */
        mr = find_heca_mr(local_hproc, msg->mr_id);
        if (unlikely(!mr || !(mr->flags & MR_LOCAL) ||
                                (mr->flags & MR_COPY_ON_ACCESS)))
                goto fail;

        // we get -1 if something bad happened, or >0 if we had dpc or we requested the page
        if (heca_trigger_page_pull(hspace, local_hproc, mr, msg->req_addr) < 0)
                r = -1;
        hproc_put(local_hproc);

        return r;

fail:
        return send_hproc_status_update(conn, msg);
}

int process_hproc_status(struct heca_connection *conn,
                struct rx_buffer_element *rx_buf_e)
{
        heca_printk(KERN_DEBUG "removing hproc %d",
                        rx_buf_e->hmsg_buffer->src_id);
        teardown_hproc_by_id(rx_buf_e->hmsg_buffer->hspace_id,
                        rx_buf_e->hmsg_buffer->src_id);
        return 1;
}

int process_page_redirect(struct heca_connection *conn,
                struct tx_buffer_element *tx_e, u32 redirect_hproc_id)
{
        struct heca_page_cache *hpc = tx_e->wrk_req->hpc;
        struct page *page = tx_e->wrk_req->dst_addr->mem_page;
        u64 req_addr = tx_e->hmsg_buffer->req_addr;
        int (*func)(struct tx_buffer_element *) = tx_e->callback.func;
        struct heca_process *mr_owner = NULL, *remote_hproc;
        struct heca_memory_region *fault_mr;
        int ret = -1;
        struct heca_process_list hprocs;

        tx_e->wrk_req->dst_addr->mem_page = NULL;
        heca_ppe_clear_release(conn, &tx_e->wrk_req->dst_addr);
        release_tx_element(conn, tx_e);

        fault_mr = find_heca_mr(hpc->hproc, tx_e->hmsg_buffer->mr_id);
        if (!fault_mr)
                goto out;

        rcu_read_lock();
        hprocs = heca_descriptor_to_hprocs(fault_mr->descriptor);
        rcu_read_unlock();

        mr_owner = find_any_hproc(hpc->hproc->hspace, hprocs);
        if (unlikely(!mr_owner))
                goto out;

        /*
         * this call requires no synchronization, it cannot be harmful in any way,
         * only wasteful in the worst case
         */
        heca_request_query(hpc->hproc, mr_owner, fault_mr, req_addr, hpc);
        hproc_put(mr_owner);

        if (hpc->redirect_hproc_id)
                redirect_hproc_id = hpc->redirect_hproc_id;

        remote_hproc = find_hproc(hpc->hproc->hspace, redirect_hproc_id);
        if (unlikely(!remote_hproc))
                goto out;

        trace_heca_redirect(hpc->hproc->hspace->hspace_id, hpc->hproc->hproc_id,
                        remote_hproc->hproc_id, fault_mr->hmr_id,
                        req_addr + fault_mr->addr, req_addr, hpc->tag);
        ret = heca_request_page(page, remote_hproc, hpc->hproc, fault_mr,
                        req_addr, func, hpc->tag, hpc, NULL);
        hproc_put(remote_hproc);

out:
        if (unlikely(ret)) {
                heca_pull_req_failure(hpc);
                heca_release_pull_hpc(&hpc);
        }
        return ret;
}

int process_page_response(struct heca_connection *conn,
                struct tx_buffer_element *tx_e)
{
        if (!tx_e->callback.func || tx_e->callback.func(tx_e))
                heca_ppe_clear_release(conn, &tx_e->wrk_req->dst_addr);
        return 0;
}

static int try_redirect_page_request(struct heca_connection *conn,
                struct heca_message *msg, struct heca_process *remote_hproc,
                u32 id)
{
        if (msg->type == MSG_REQ_PUSHED_PAGE || id == remote_hproc->hproc_id)
                return -EFAULT;

        msg->dest_id = id;
        return heca_send_response(conn, MSG_RES_PAGE_REDIRECT, msg);
}

static inline void defer_gup(struct heca_message *msg,
                struct heca_process *local_hproc, struct heca_memory_region *mr,
                struct heca_process *remote_hproc, struct heca_connection *conn)
{
        struct heca_deferred_gup *dgup = NULL;

retry:
        dgup = kmem_cache_alloc(kmem_deferred_gup_cache, GFP_KERNEL);
        if (unlikely(!dgup)) {
                might_sleep();
                goto retry;
        }
        dgup->connection_origin = conn;
        dgup->remote_hproc = remote_hproc;
        dgup->hmr = mr;
        heca_msg_cpy(&dgup->hmsg, msg);
        llist_add(&dgup->lnode, &local_hproc->deferred_gups);
        schedule_work(&local_hproc->deferred_gup_work);
}

int process_page_claim(struct heca_connection *conn, struct heca_message *msg)
{
        struct heca_space *hspace;
        struct heca_process *local_hproc, *remote_proc;
        struct heca_memory_region *mr;
        unsigned long addr;
        int r = -EFAULT;

        hspace = find_hspace(msg->hspace_id);
        if (unlikely(!hspace))
                goto out;

        local_hproc = find_hproc(hspace, msg->dest_id);
        if (unlikely(!local_hproc))
                goto out;

        mr = find_heca_mr(local_hproc, msg->mr_id);
        if (unlikely(!mr))
                goto out_hproc;

        remote_proc = find_hproc(hspace, msg->src_id);
        if (unlikely(!remote_proc))
                goto out_hproc;

        addr = msg->req_addr + mr->addr;

        BUG_ON(!local_hproc->mm);
        r = heca_try_unmap_page(local_hproc, addr, remote_proc,
                        msg->type == MSG_REQ_CLAIM);

        /*
         * no locking required: if we were maintainers, no one can hand out read
         * copies right now, and we can safely invalidate. otherwise, the
         * maintainer is the one invalidating us - in which case it won't answer a
         * read request until it finishes.
         */
        if (r == 1) {
                if (heca_lookup_page_read(local_hproc, addr))
                        BUG_ON(!heca_extract_page_read(local_hproc, addr));
                else
                        heca_invalidate_readers(local_hproc, addr,
                                        remote_proc->hproc_id);
        }

        hproc_put(remote_proc);
out_hproc:
        hproc_put(local_hproc);
out:
        /*
         * for CLAIM requests, acknowledge if a page was actually unmapped;
         * for TRY_CLAIM requests, a pte change would also suffice.
         */
        ack_msg(conn, msg, (r < 0 || (r == 0 && msg->type == MSG_REQ_CLAIM))?
                        MSG_RES_ACK_FAIL : MSG_RES_ACK);
        return r;
}

static int heca_retry_claim(struct heca_message *msg, struct page *page)
{
        struct heca_space *hspace;
        struct heca_process *hproc = NULL, *remote_hproc, *owner;
        struct heca_memory_region *mr;
        struct heca_process_list hprocs;
        struct heca_page_cache *hpc;

        hspace = find_hspace(msg->hspace_id);
        if (!hspace)
                goto fail;

        hproc = find_hproc(hspace, msg->src_id);
        if (!hproc)
                goto fail;

        mr = find_heca_mr(hproc, msg->mr_id);
        if (!mr)
                goto fail;

        /*
         * we were trying to invalidate the maintainer's copy, but it took our copy
         * away from us in the meantime... this isn't safe or protected, we rely on
         * the maintainer not to do anything stupid (like invalidating a writeable
         * copy, or invalidating when it's trying to invalidate reader copies).
         */
        if (!heca_pte_present(hproc->mm, msg->req_addr + mr->addr))
                goto fail;

        rcu_read_lock();
        hprocs = heca_descriptor_to_hprocs(mr->descriptor);
        rcu_read_unlock();

        owner = find_any_hproc(hspace, hprocs);
        /*
         * in the bizarre situation in which we can't seem to get the page, and we
         * don't have a valid directory, fall back to a regular fault (maybe hspace is
         * being removed?)
         */
        if (unlikely(!owner || owner == hproc))
                goto fail;

        /*
         * this only happens when write-faulting on a page we are not
         * maintaining, in which case a dpc will be in-place until we finish.
         */
        hpc = heca_cache_get(hproc, msg->req_addr);
        BUG_ON(!hpc);

        heca_request_query(hproc, owner, mr, msg->req_addr, hpc);
        hproc_put(owner);
        /*
         * TODO: block here until the query finishes, otherwise issuing
         * another claim is wasteful/useless.
         */

        remote_hproc = find_hproc(hspace, hpc->redirect_hproc_id);
        if (unlikely(!remote_hproc))
                goto fail;

        heca_claim_page(hproc, remote_hproc, mr, msg->req_addr, page, 1);
        hproc_put(hproc);
        return 0;

fail:
        if (hproc)
                hproc_put(hproc);
        return -EFAULT;
}

int process_claim_ack(struct heca_connection *conn,
                struct tx_buffer_element *tx_e, struct heca_message *response)
{
        struct heca_message *msg = tx_e->hmsg_buffer;
        struct page *page = tx_e->reply_work_req->mem_page;

        tx_e->reply_work_req->mem_page = NULL;

        /*
         * this only happens when we request a maintainer of a page to hand us over
         * the maintenance, and the remote node signals it is not the maintainer.
         *
         * we keep on retrying, while constantly querying the mr owner for
         * up-to-date info. while theoretically this may go on forever, querying is
         * far faster in practice, so our achilles should catch the turtle easily.
         */
        if (unlikely(msg->type == MSG_REQ_CLAIM &&
                                response->type == MSG_RES_ACK_FAIL)) {
                if (likely(!heca_retry_claim(msg, page)))
                        return -EAGAIN;
        }

        if (page) {
                unlock_page(page);
                page_cache_release(page);
        }

        return 0;
}

static int process_page_request(struct heca_connection *origin_conn,
                struct heca_process *local_hproc, struct heca_memory_region *mr,
                struct heca_process *remote_hproc, struct heca_message *msg,
                int deferred)
{
        struct heca_page_pool_element *ppe;
        struct tx_buffer_element *tx_e = NULL;
        struct page *page;
        unsigned long addr = 0;
        struct heca_connection *conn = NULL;
        u32 redirect_id = 0;
        int res = 0;

        if (unlikely(!local_hproc)) {
                send_hproc_status_update(origin_conn, msg);
                goto fail;
        }

        if (unlikely(!remote_hproc))
                goto fail;

        conn = remote_hproc->connection;
        addr = msg->req_addr + mr->addr;
        BUG_ON(addr < mr->addr || addr > mr->addr + mr->sz);

        trace_heca_process_page_request(local_hproc->hspace->hspace_id,
                        local_hproc->hproc_id, remote_hproc->hproc_id,
                        mr->hmr_id, addr, msg->req_addr,
                        msg->type);

retry:
        tx_e = try_get_next_empty_tx_reply_ele(conn);
        if (unlikely(!tx_e)) {
                cond_resched();
                goto retry;
        }
        BUG_ON(!tx_e);

        heca_msg_cpy(tx_e->hmsg_buffer, msg);
        tx_e->hmsg_buffer->type = MSG_RES_PAGE;
        tx_e->reply_work_req->wr.wr.rdma.remote_addr = tx_e->hmsg_buffer->dst_addr;
        tx_e->reply_work_req->wr.wr.rdma.rkey = tx_e->hmsg_buffer->rkey;
        tx_e->reply_work_req->mm = local_hproc->mm;
        tx_e->reply_work_req->addr = addr;

        if (msg->type == MSG_REQ_PUSHED_PAGE) {
                res = heca_lookup_page_in_remote(local_hproc, remote_hproc,
                                addr, &page);
                /* TODO: this is for send_msg_handler, easily improvable */
                tx_e->reply_work_req->pte.pte = 0;
        } else {
                res = heca_extract_page_from_remote(local_hproc, remote_hproc,
                                addr, msg->type, &tx_e->reply_work_req->pte,
                                &page, &redirect_id, deferred, mr);
        }
        if (unlikely(res != HECA_EXTRACT_SUCCESS))
                goto no_page;

        BUG_ON(!page);
        ppe = heca_prepare_ppe(conn, page);
        if (!ppe)
                goto no_page;

        tx_e->wrk_req->dst_addr = ppe;
        tx_e->reply_work_req->page_sgl.addr = (u64) ppe->page_buf;

        trace_heca_process_page_request_complete(local_hproc->hspace->hspace_id,
                        local_hproc->hproc_id, remote_hproc->hproc_id,
                        mr->hmr_id, addr, msg->req_addr, msg->type);
        tx_heca_send(conn, tx_e);
        hproc_put(local_hproc);
        hproc_put(remote_hproc);
        return 0;

no_page:
        release_tx_element_reply(conn, tx_e);

        /* redirect instead of answer */
        if (res == HECA_EXTRACT_REDIRECT) {
                if (try_redirect_page_request(conn, msg, remote_hproc,
                                        redirect_id))
                        goto fail;
                goto out;

        /* defer and try to get the page again out of sequence */
        } else if (msg->type & (MSG_REQ_PAGE | MSG_REQ_READ)) {
                trace_heca_defer_gup(local_hproc->hspace->hspace_id,
                                local_hproc->hproc_id, remote_hproc->hproc_id,
                                mr->hmr_id, addr, msg->req_addr, msg->type);
                defer_gup(msg, local_hproc, mr, remote_hproc, origin_conn);
                /* we release the hprocs when we actually solve the gup */
                goto out_keep;
        }

fail:
        heca_send_response(conn, MSG_RES_PAGE_FAIL, msg);
out:
        if (remote_hproc)
                hproc_put(remote_hproc);
        if (local_hproc)
                hproc_put(local_hproc);
out_keep:
        return -EINVAL;
}


/*
 * TODO: we really would like to do NOIO GUP with fast iteration over list in
 * order to process the GUP in the fastest order
 */
static inline void process_deferred_gups(struct heca_process *hproc)
{
        struct heca_deferred_gup *dgup = NULL;
        struct llist_node *llnode = llist_del_all(&hproc->deferred_gups);

        do {
                while (llnode) {
                        dgup = container_of(llnode, struct heca_deferred_gup,
                                        lnode);
                        llnode = llnode->next;
                        /* the deferred is set to one i.e if we need to gup we will block */
                        trace_heca_defer_gup_execute(hproc->hspace->hspace_id,
                                        hproc->hproc_id,
                                        dgup->remote_hproc->hproc_id,
                                        dgup->hmr->hmr_id,
                                        dgup->hmsg.req_addr + dgup->hmr->addr,
                                        dgup->hmsg.req_addr,
                                        dgup->hmsg.type);
                        process_page_request(dgup->connection_origin, hproc,
                                        dgup->hmr, dgup->remote_hproc,
                                        &dgup->hmsg, 1);
                        /* release the element */
                        release_kmem_deferred_gup_cache_elm(dgup);
                }
                llnode = llist_del_all(&hproc->deferred_gups);
        } while (llnode);
}

void deferred_gup_work_fn(struct work_struct *w)
{
        struct heca_process *hproc;

        hproc = container_of(w, struct heca_process, deferred_gup_work);
        process_deferred_gups(hproc);
}

int process_page_request_msg(struct heca_connection *conn,
                struct heca_message *msg)
{
        struct heca_process *local_hproc = NULL, *remote_hproc = NULL;
        struct heca_space *hspace = NULL;
        struct heca_memory_region *mr = NULL;

        hspace = find_hspace(msg->hspace_id);
        if (unlikely(!hspace))
                goto fail;

        local_hproc = find_hproc(hspace, msg->src_id);
        if (unlikely(!local_hproc))
                goto fail;

        mr = find_heca_mr(local_hproc, msg->mr_id);
        if (unlikely(!mr))
                goto fail;

        remote_hproc = find_hproc(hspace, msg->dest_id);
        if (unlikely(!remote_hproc)) {
                hproc_put(local_hproc);
                goto fail;
        }

        return process_page_request(conn, local_hproc, mr,
                        remote_hproc, msg, 0);

fail:
        return -EFAULT;
}

int heca_request_page_pull(struct heca_space *hspace,
                struct heca_process *fault_hproc, struct page *page,
                unsigned long addr, struct mm_struct *mm,
                struct heca_memory_region *mr)
{
        struct heca_process_list hprocs;
        int ret = 0, i;

        rcu_read_lock();
        hprocs = heca_descriptor_to_hprocs(mr->descriptor);
        rcu_read_unlock();

        /*
         * This is a useful heuristic; it's possible that tx_elms have been freed in
         * the meanwhile, but we don't have to use them now as a work thread will
         * use them anyway to free the req_queue.
         */
        for_each_valid_hproc(hprocs, i) {
                struct heca_process *hproc = find_hproc(hspace, hprocs.ids[i]);
                int full = heca_request_queue_full(hproc->connection);

                hproc_put(hproc);
                if (full)
                        return -ENOMEM;
        }

        ret = heca_prepare_page_for_push(fault_hproc, hprocs, page, addr, mm,
                        mr->descriptor);
        if (unlikely(ret))
                goto out;

        ret = send_request_heca_page_pull(fault_hproc, mr, hprocs,
                        addr - mr->addr);
        if (unlikely(ret == -ENOMEM))
                heca_cancel_page_push(fault_hproc, addr, page);

out:
        return ret;
}

int ack_msg(struct heca_connection *conn, struct heca_message *msg, u32 type)
{
        return heca_send_response(conn, type, msg);
}

int unmap_range(struct heca_space *hspace, int dsc, pid_t pid,
                unsigned long addr, unsigned long sz)
{
        int r = 0;
        unsigned long it = addr, end = (addr + sz - 1);
        struct mm_struct *mm;

        BUG_ON(!pid);

        mm = find_mm_by_pid(pid);

        for (it = addr; it < end; it += PAGE_SIZE) {
                r = hproc_flag_page_remote(mm, hspace, dsc, it);
                if (r)
                        break;
        }

        return r;
}

