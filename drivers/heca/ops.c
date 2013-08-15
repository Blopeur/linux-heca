/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 */
#include <linux/pagemap.h>
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
 */
static int dsm_send_msg(struct heca_connection *ele, u32 dsm_id, u32 mr_id,
                u32 src_id, u32 dest_id, unsigned long local_addr,
                unsigned long shared_addr, struct page *page, int type,
                int (*func)(struct tx_buffer_element *), struct heca_page_cache *dpc,
                struct heca_page_pool_element *ppe, struct heca_message *msg,
                int need_ppe)
{
        struct tx_buffer_element *tx_e = NULL;

        might_sleep();
        while (1) {
                tx_e = try_get_next_empty_tx_ele(ele, 1);
                if (likely(tx_e)) {
                        return dsm_send_tx_e(ele, tx_e, !!msg, type,
                                        dsm_id, mr_id, src_id, dest_id,
                                        local_addr, shared_addr, dpc, page,
                                        ppe, need_ppe, func, msg);
                }

                if (!add_dsm_request(NULL, ele, type, dsm_id, src_id, mr_id,
                                        dest_id, shared_addr, func, dpc, page,
                                        ppe, need_ppe, msg)) {
                        return 1;
                }

                cond_resched();
        }
}

/*
 * same as dsm_send_msg, only with different preparation of the tx_e, and
 * different method of queueing the args. dsm_send_tx_e receives response=1.
 */
static int dsm_send_response(struct heca_connection *ele, int type,
                struct heca_message *msg)
{
        return dsm_send_msg(ele, msg->dsm_id, msg->mr_id,
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

static int send_request_dsm_page_pull(struct heca_process *fault_svm,
                struct heca_memory_region *fault_mr, struct heca_process_list svms,
                unsigned long addr)
{
        struct tx_buffer_element *tx_elms[svms.num];
        struct heca_request *reqs[svms.num];
        struct heca_connection *eles[svms.num];
        int i, j, r = 0;

        for_each_valid_hproc(svms, i) {
                struct heca_process *svm;

                reqs[i] = NULL;
                tx_elms[i] = NULL;

                svm = find_hproc(fault_svm->hspace, svms.ids[i]);
                if (unlikely(!svm))
                        continue;

                eles[i] = svm->connection;
                release_hproc(svm);

                tx_elms[i] = try_get_next_empty_tx_ele(eles[i], 1);
                if (unlikely(!tx_elms[i])) {
                        reqs[i] = alloc_dsm_request();
                        if (unlikely(!reqs[i]))
                                goto nomem;
                }
        }

        /*
         * we have to iterate all svms, and rely on tx_elms or reqs, since some
         * might have been dropped since the previous iteration.
         */
        might_sleep();
        for (i = 0; i < svms.num; i++) {
                if (tx_elms[i]) {
                        /* note that dest_id == local_svm */
                        r |= dsm_send_tx_e(eles[i], tx_elms[i], 0,
                                        MSG_REQ_PAGE_PULL,
                                        fault_svm->hspace->hspace_id, fault_mr->hmr_id,
                                        svms.ids[i], fault_svm->hproc_id,
                                        addr + fault_mr->addr, addr, NULL, NULL,
                                        NULL, 0, NULL, NULL);
                } else if (reqs[i]) {
                        /* can't fail, reqs[i] already allocated */
                        j = add_dsm_request(reqs[i], eles[i], MSG_REQ_PAGE_PULL,
                                        fault_svm->hspace->hspace_id, svms.ids[i],
                                        fault_mr->hmr_id, fault_svm->hproc_id,
                                        addr, NULL, NULL, NULL, NULL, 0, NULL);
                        BUG_ON(j);
                }
        }

        return r;

nomem:
        for (j = 0; j < i; j++) {
                if (tx_elms[j])
                        release_tx_element(eles[j], tx_elms[j]);
                else if (reqs[j])
                        release_dsm_request(reqs[j]);
        }
        return -ENOMEM;
}

static int send_svm_status_update(struct heca_connection *ele,
                struct heca_message *msg)
{
        return dsm_send_response(ele, MSG_RES_SVM_FAIL, msg);
}

static int dsm_request_query(struct heca_process *svm,
                struct heca_process *owner, struct heca_memory_region *mr,
                unsigned long shared_addr, struct heca_page_cache *dpc)
{
        return dsm_send_msg(owner->connection, svm->hspace->hspace_id, mr->hmr_id,
                        svm->hproc_id, owner->hproc_id, shared_addr + mr->addr,
                        shared_addr, NULL, MSG_REQ_QUERY,
                        dsm_process_query_info, dpc, NULL, NULL, 0);
}

/*
 * request another node to make sure it registers the address as belonging to
 * us. the only_unmap flag means that we will continue sending the request until
 * a page will actually be unmapped. without the flag, we will be content with
 * only changing the pte on the other side to point to us.
 */
int dsm_claim_page(struct heca_process *fault_svm,
                struct heca_process *remote_svm,
                struct heca_memory_region *fault_mr, unsigned long addr,
                struct page *page, int only_unmap)
{
        u32 type = only_unmap? MSG_REQ_CLAIM : MSG_REQ_CLAIM_TRY;

        trace_dsm_claim_page(fault_svm->hspace->hspace_id, fault_svm->hproc_id,
                        remote_svm->hproc_id, fault_mr->hmr_id, addr,
                        addr - fault_mr->addr, type);

        return dsm_send_msg(remote_svm->connection, fault_svm->hspace->hspace_id,
                        fault_mr->hmr_id, fault_svm->hproc_id, remote_svm->hproc_id,
                        addr, addr - fault_mr->addr, page, type,
                        NULL, NULL, NULL, NULL, 0);
}

int request_dsm_page(struct page *page, struct heca_process *remote_svm,
                struct heca_process *fault_svm,
                struct heca_memory_region *fault_mr, unsigned long addr,
                int (*func)(struct tx_buffer_element *), int tag,
                struct heca_page_cache *dpc, struct heca_page_pool_element *ppe)
{
        int type;

        switch (tag) {
        case PULL_TRY_TAG:
                type = MSG_REQ_PAGE_TRY;
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
        return dsm_send_msg(remote_svm->connection, fault_svm->hspace->hspace_id,
                        fault_mr->hmr_id, remote_svm->hproc_id, fault_svm->hproc_id,
                        addr, addr - fault_mr->addr, page, type, func, dpc, ppe,
                        NULL, 1);
}

int dsm_process_request_query(struct heca_connection *ele, struct rx_buffer_element *rx_e)
{
        struct heca_message *msg = rx_e->hmsg_buffer;
        struct heca_space *dsm;
        struct heca_process *svm;
        struct heca_memory_region *mr;
        int r = -EFAULT;
        unsigned long addr;

        dsm = find_hspace(msg->dsm_id);
        if (unlikely(!dsm))
                goto fail;

        svm = find_hproc(dsm, msg->dest_id);
        if (unlikely(!svm))
                goto fail;

        mr = find_heca_mr(svm, msg->mr_id);
        if (unlikely(!mr))
                goto out;

        addr = msg->req_addr + mr->addr;

        /* this cannot fail: if we don't have a valid dsm pte, the page is ours */
        msg->dest_id = dsm_query_pte_info(svm, addr);

        r = dsm_send_response(ele, MSG_RES_QUERY, msg);

out:
        release_hproc(svm);
fail:
        return r;
}

int dsm_process_query_info(struct tx_buffer_element *tx_e)
{
        struct heca_message *msg = tx_e->hmsg_buffer;
        struct heca_space *dsm;
        struct heca_process *svm;
        struct heca_page_cache *dpc;
        struct heca_memory_region *mr;
        unsigned long addr;
        int r = -EFAULT;

        dsm = find_hspace(msg->dsm_id);
        if (!dsm)
                goto fail;

        svm = find_hproc(dsm, msg->src_id);
        if (!svm)
                goto fail;

        mr = find_heca_mr(svm, msg->mr_id);
        if (!mr)
                goto out;

        addr = msg->req_addr + mr->addr;
        dpc = dsm_cache_get_hold(svm, addr);
        if (likely(dpc)) {
                if (likely(dpc == tx_e->wrk_req->hpc))
                        dpc->redirect_hproc_id = msg->dest_id;
                dsm_release_pull_dpc(&dpc);
        }
        r = 0;

out:
        release_hproc(svm);
fail:
        return r;
}

int process_pull_request(struct heca_connection *ele, struct rx_buffer_element *rx_buf_e)
{
        struct heca_process *local_svm;
        struct heca_space *dsm;
        struct heca_message *msg;
        struct heca_memory_region *mr;
        int r = 0;

        BUG_ON(!rx_buf_e);
        BUG_ON(!rx_buf_e->hmsg_buffer);
        msg = rx_buf_e->hmsg_buffer;

        dsm = find_hspace(msg->dsm_id);
        if (unlikely(!dsm))
                goto fail;

        local_svm = find_hproc(dsm, msg->src_id);
        if (unlikely(!local_svm || !local_svm->mm))
                goto fail;

        /* push only happens to mr owners! */
        mr = find_heca_mr(local_svm, msg->mr_id);
        if (unlikely(!mr || !(mr->flags & MR_LOCAL) ||
                                (mr->flags & MR_COPY_ON_ACCESS)))
                goto fail;

        // we get -1 if something bad happened, or >0 if we had dpc or we requested the page
        if (dsm_trigger_page_pull(dsm, local_svm, mr, msg->req_addr) < 0)
                r = -1;
        release_hproc(local_svm);

        return r;

fail:
        return send_svm_status_update(ele, msg);
}

int process_svm_status(struct heca_connection *ele, struct rx_buffer_element *rx_buf_e)
{
        heca_printk(KERN_DEBUG "removing svm %d", rx_buf_e->hmsg_buffer->src_id);
        remove_hproc(rx_buf_e->hmsg_buffer->dsm_id, rx_buf_e->hmsg_buffer->src_id);
        return 1;
}

int process_page_redirect(struct heca_connection *ele, struct tx_buffer_element *tx_e,
                u32 redirect_svm_id)
{
        struct heca_page_cache *dpc = tx_e->wrk_req->hpc;
        struct page *page = tx_e->wrk_req->dst_addr->mem_page;
        u64 req_addr = tx_e->hmsg_buffer->req_addr;
        int (*func)(struct tx_buffer_element *) = tx_e->callback.func;
        struct heca_process *mr_owner = NULL, *remote_svm;
        struct heca_memory_region *fault_mr;
        int ret = -1;
        struct heca_process_list svms;

        tx_e->wrk_req->dst_addr->mem_page = NULL;
        dsm_ppe_clear_release(ele, &tx_e->wrk_req->dst_addr);
        release_tx_element(ele, tx_e);

        fault_mr = find_heca_mr(dpc->hproc, tx_e->hmsg_buffer->mr_id);
        if (!fault_mr)
                goto out;

        rcu_read_lock();
        svms = dsm_descriptor_to_svms(fault_mr->descriptor);
        rcu_read_unlock();

        mr_owner = find_any_hproc(dpc->hproc->hspace, svms);
        if (unlikely(!mr_owner))
                goto out;

        /*
         * this call requires no synchronization, it cannot be harmful in any way,
         * only wasteful in the worst case
         */
        dsm_request_query(dpc->hproc, mr_owner, fault_mr, req_addr, dpc);
        release_hproc(mr_owner);

        if (dpc->redirect_hproc_id)
                redirect_svm_id = dpc->redirect_hproc_id;

        remote_svm = find_hproc(dpc->hproc->hspace, redirect_svm_id);
        if (unlikely(!remote_svm))
                goto out;

        trace_redirect(dpc->hproc->hspace->hspace_id, dpc->hproc->hproc_id,
                        remote_svm->hproc_id, fault_mr->hmr_id,
                        req_addr + fault_mr->addr, req_addr, dpc->tag);
        ret = request_dsm_page(page, remote_svm, dpc->hproc, fault_mr, req_addr,
                        func, dpc->tag, dpc, NULL);
        release_hproc(remote_svm);

out:
        if (unlikely(ret)) {
                dsm_pull_req_failure(dpc);
                dsm_release_pull_dpc(&dpc);
        }
        return ret;
}

int process_page_response(struct heca_connection *ele, struct tx_buffer_element *tx_e)
{
        if (!tx_e->callback.func || tx_e->callback.func(tx_e))
                dsm_ppe_clear_release(ele, &tx_e->wrk_req->dst_addr);
        return 0;
}

static int try_redirect_page_request(struct heca_connection *ele,
                struct heca_message *msg, struct heca_process *remote_svm, u32 id)
{
        if (msg->type == MSG_REQ_PAGE_TRY || id == remote_svm->hproc_id)
                return -EFAULT;

        msg->dest_id = id;
        return dsm_send_response(ele, MSG_RES_PAGE_REDIRECT, msg);
}

static inline void defer_gup(struct heca_message *msg,
                struct heca_process *local_svm, struct heca_memory_region *mr,
                struct heca_process *remote_svm, struct heca_connection *ele)
{
        struct heca_deferred_gup *dgup = NULL;

retry:
        dgup = kmem_cache_alloc(kmem_deferred_gup_cache, GFP_KERNEL);
        if (unlikely(!dgup)) {
                might_sleep();
                goto retry;
        }
        dgup->connection_origin = ele;
        dgup->remote_hproc = remote_svm;
        dgup->hmr = mr;
        dsm_msg_cpy(&dgup->hmsg, msg);
        llist_add(&dgup->lnode, &local_svm->deferred_gups);
        schedule_work(&local_svm->deferred_gup_work);
}

int process_page_claim(struct heca_connection *ele, struct heca_message *msg)
{
        struct heca_space *dsm;
        struct heca_process *local_svm, *remote_svm;
        struct heca_memory_region *mr;
        unsigned long addr;
        int r = -EFAULT;

        dsm = find_hspace(msg->dsm_id);
        if (unlikely(!dsm))
                goto out;

        local_svm = find_hproc(dsm, msg->dest_id);
        if (unlikely(!local_svm))
                goto out;

        mr = find_heca_mr(local_svm, msg->mr_id);
        if (unlikely(!mr))
                goto out_svm;

        remote_svm = find_hproc(dsm, msg->src_id);
        if (unlikely(!remote_svm))
                goto out_svm;

        addr = msg->req_addr + mr->addr;

        BUG_ON(!local_svm->mm);
        r = dsm_try_unmap_page(local_svm, addr, remote_svm,
                        msg->type == MSG_REQ_CLAIM);

        /*
         * no locking required: if we were maintainers, no one can hand out read
         * copies right now, and we can safely invalidate. otherwise, the
         * maintainer is the one invalidating us - in which case it won't answer a
         * read request until it finishes.
         */
        if (r == 1) {
                if (dsm_lookup_page_read(local_svm, addr))
                        BUG_ON(!dsm_extract_page_read(local_svm, addr));
                else
                        dsm_invalidate_readers(local_svm, addr,
                                        remote_svm->hproc_id);
        }

        release_hproc(remote_svm);
out_svm:
        release_hproc(local_svm);
out:
        /*
         * for CLAIM requests, acknowledge if a page was actually unmapped;
         * for TRY_CLAIM requests, a pte change would also suffice.
         */
        ack_msg(ele, msg, (r < 0 || (r == 0 && msg->type == MSG_REQ_CLAIM))?
                        MSG_RES_ACK_FAIL : MSG_RES_ACK);
        return r;
}

static int dsm_retry_claim(struct heca_message *msg, struct page *page)
{
        struct heca_space *dsm;
        struct heca_process *svm = NULL, *remote_svm, *owner;
        struct heca_memory_region *mr;
        struct heca_process_list svms;
        struct heca_page_cache *dpc;

        dsm = find_hspace(msg->dsm_id);
        if (!dsm)
                goto fail;

        svm = find_hproc(dsm, msg->src_id);
        if (!svm)
                goto fail;

        mr = find_heca_mr(svm, msg->req_addr);
        if (!mr)
                goto fail;

        /*
         * we were trying to invalidate the maintainer's copy, but it took our copy
         * away from us in the meantime... this isn't safe or protected, we rely on
         * the maintainer not to do anything stupid (like invalidating a writeable
         * copy, or invalidating when it's trying to invalidate reader copies).
         */
        if (!dsm_pte_present(svm->mm, msg->req_addr + mr->addr))
                goto fail;

        rcu_read_lock();
        svms = dsm_descriptor_to_svms(mr->descriptor);
        rcu_read_unlock();

        owner = find_any_hproc(dsm, svms);
        /*
         * in the bizarre situation in which we can't seem to get the page, and we
         * don't have a valid directory, fall back to a regular fault (maybe dsm is
         * being removed?)
         */
        if (unlikely(!owner || owner == svm))
                goto fail;

        /*
         * this only happens when write-faulting on a page we are not
         * maintaining, in which case a dpc will be in-place until we finish.
         */
        dpc = dsm_cache_get(svm, msg->req_addr);
        BUG_ON(!dpc);

        dsm_request_query(svm, owner, mr, msg->req_addr, dpc);
        release_hproc(owner);
        /*
         * TODO: block here until the query finishes, otherwise issuing
         * another claim is wasteful/useless.
         */

        remote_svm = find_hproc(dsm, dpc->redirect_hproc_id);
        if (unlikely(!remote_svm))
                goto fail;

        dsm_claim_page(svm, remote_svm, mr, msg->req_addr, page, 1);
        release_hproc(svm);
        return 0;

fail:
        if (svm)
                release_hproc(svm);
        return -EFAULT;
}

int process_claim_ack(struct heca_connection *ele, struct tx_buffer_element *tx_e,
                struct heca_message *response)
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
                if (likely(!dsm_retry_claim(msg, page)))
                        return -EAGAIN;
        }

        if (page) {
                unlock_page(page);
                page_cache_release(page);
        }

        return 0;
}

static int process_page_request(struct heca_connection *origin_ele,
                struct heca_process *local_svm, struct heca_memory_region *mr,
                struct heca_process *remote_svm, struct heca_message *msg,
                int deferred)
{
        struct heca_page_pool_element *ppe;
        struct tx_buffer_element *tx_e = NULL;
        struct page *page;
        unsigned long addr = 0;
        struct heca_connection *ele = NULL;
        u32 redirect_id = 0;
        int res = 0;

        if (unlikely(!local_svm)) {
                send_svm_status_update(origin_ele, msg);
                goto fail;
        }

        if (unlikely(!remote_svm))
                goto fail;

        ele = remote_svm->connection;
        addr = msg->req_addr + mr->addr;
        BUG_ON(addr < mr->addr || addr > mr->addr + mr->sz);

        trace_process_page_request(local_svm->hspace->hspace_id, local_svm->hproc_id,
                        remote_svm->hproc_id, mr->hmr_id, addr, msg->req_addr,
                        msg->type);

retry:
        tx_e = try_get_next_empty_tx_reply_ele(ele);
        if (unlikely(!tx_e)) {
                cond_resched();
                goto retry;
        }
        BUG_ON(!tx_e);

        dsm_msg_cpy(tx_e->hmsg_buffer, msg);
        tx_e->hmsg_buffer->type = MSG_RES_PAGE;
        tx_e->reply_work_req->wr.wr.rdma.remote_addr = tx_e->hmsg_buffer->dst_addr;
        tx_e->reply_work_req->wr.wr.rdma.rkey = tx_e->hmsg_buffer->rkey;
        tx_e->reply_work_req->mm = local_svm->mm;
        tx_e->reply_work_req->addr = addr;

        res = dsm_extract_page_from_remote(local_svm, remote_svm, addr,
                        msg->type, &tx_e->reply_work_req->pte, &page,
                        &redirect_id, deferred, mr);
        if (unlikely(res != HECA_EXTRACT_SUCCESS))
                goto no_page;

        BUG_ON(!page);
        ppe = dsm_prepare_ppe(ele, page);
        if (!ppe)
                goto no_page;

        tx_e->wrk_req->dst_addr = ppe;
        tx_e->reply_work_req->page_sgl.addr = (u64) ppe->page_buf;

        trace_process_page_request_complete(local_svm->hspace->hspace_id,
                        local_svm->hproc_id, remote_svm->hproc_id, mr->hmr_id,
                        addr, msg->req_addr, msg->type);
        tx_dsm_send(ele, tx_e);
        release_hproc(local_svm);
        release_hproc(remote_svm);
        return 0;

no_page:
        release_tx_element_reply(ele, tx_e);

        /* redirect instead of answer */
        if (res == HECA_EXTRACT_REDIRECT) {
                if (try_redirect_page_request(ele, msg, remote_svm,
                                        redirect_id))
                        goto fail;
                goto out;

                /* defer and try to get the page again out of sequence */
        } else if (msg->type & (MSG_REQ_PAGE | MSG_REQ_READ)) {
                trace_dsm_defer_gup(local_svm->hspace->hspace_id, local_svm->hproc_id,
                                remote_svm->hproc_id, mr->hmr_id, addr,
                                msg->req_addr, msg->type);
                defer_gup(msg, local_svm, mr, remote_svm, origin_ele);
                /* we release the svms when we actually solve the gup */
                goto out_keep;
        }

fail:
        dsm_send_response(ele, MSG_RES_PAGE_FAIL, msg);
out:
        if (remote_svm)
                release_hproc(remote_svm);
        if (local_svm)
                release_hproc(local_svm);
out_keep:
        return -EINVAL;
}


/*
 * TODO: we really would like to do NOIO GUP with fast iteration over list in
 * order to process the GUP in the fastest order
 */
static inline void process_deferred_gups(struct heca_process *svm)
{
        struct heca_deferred_gup *dgup = NULL;
        struct llist_node *llnode = llist_del_all(&svm->deferred_gups);

        do {
                while (llnode) {
                        dgup = container_of(llnode, struct heca_deferred_gup, lnode);
                        llnode = llnode->next;
                        /* the deferred is set to one i.e if we need to gup we will block */
                        trace_dsm_defer_gup_execute(svm->hspace->hspace_id,
                                        svm->hproc_id, dgup->remote_hproc->hproc_id,
                                        dgup->hmr->hmr_id,
                                        dgup->hmsg.req_addr + dgup->hmr->addr,
                                        dgup->hmsg.req_addr,
                                        dgup->hmsg.type);
                        process_page_request(dgup->connection_origin, svm, dgup->hmr,
                                        dgup->remote_hproc, &dgup->hmsg, 1);
                        /* release the element */
                        release_kmem_deferred_gup_cache_elm(dgup);
                }
                llnode = llist_del_all(&svm->deferred_gups);
        } while (llnode);
}

void deferred_gup_work_fn(struct work_struct *w)
{
        struct heca_process *svm;

        svm = container_of(w, struct heca_process, deferred_gup_work);
        process_deferred_gups(svm);
}

int process_page_request_msg(struct heca_connection *ele, struct heca_message *msg)
{
        struct heca_process *local_svm = NULL, *remote_svm = NULL;
        struct heca_space *dsm = NULL;
        struct heca_memory_region *mr = NULL;

        dsm = find_hspace(msg->dsm_id);
        if (unlikely(!dsm))
                goto fail;

        local_svm = find_hproc(dsm, msg->src_id);
        if (unlikely(!local_svm))
                goto fail;

        mr = find_heca_mr(local_svm, msg->mr_id);
        if (unlikely(!mr))
                goto fail;

        remote_svm = find_hproc(dsm, msg->dest_id);
        if (unlikely(!remote_svm)) {
                release_hproc(local_svm);
                goto fail;
        }

        return process_page_request(ele, local_svm, mr, remote_svm, msg, 0);

fail:
        return -EFAULT;
}

int dsm_request_page_pull(struct heca_space *dsm, struct heca_process *fault_svm,
                struct page *page, unsigned long addr, struct mm_struct *mm,
                struct heca_memory_region *mr)
{
        struct heca_process_list svms;
        int ret = 0, i;

        rcu_read_lock();
        svms = dsm_descriptor_to_svms(mr->descriptor);
        rcu_read_unlock();

        /*
         * This is a useful heuristic; it's possible that tx_elms have been freed in
         * the meanwhile, but we don't have to use them now as a work thread will
         * use them anyway to free the req_queue.
         */
        for_each_valid_hproc(svms, i) {
                struct heca_process *svm = find_hproc(dsm, svms.ids[i]);
                int full = request_queue_full(svm->connection);

                release_hproc(svm);
                if (full)
                        return -ENOMEM;
        }

        ret = dsm_prepare_page_for_push(fault_svm, svms, page, addr, mm,
                        mr->descriptor);
        if (unlikely(ret))
                goto out;

        ret = send_request_dsm_page_pull(fault_svm, mr, svms, addr - mr->addr);
        if (unlikely(ret == -ENOMEM))
                dsm_cancel_page_push(fault_svm, addr, page);

out:
        return ret;
}

int ack_msg(struct heca_connection *ele, struct heca_message *msg, u32 type)
{
        return dsm_send_response(ele, type, msg);
}

int unmap_range(struct heca_space *dsm, int dsc, pid_t pid, unsigned long addr,
                unsigned long sz)
{
        int r = 0;
        unsigned long it = addr, end = (addr + sz - 1);
        struct mm_struct *mm;

        BUG_ON(!pid);

        mm = find_mm_by_pid(pid);

        for (it = addr; it < end; it += PAGE_SIZE) {
                r = dsm_flag_page_remote(mm, dsm, dsc, it);
                if (r)
                        break;
        }

        return r;
}

