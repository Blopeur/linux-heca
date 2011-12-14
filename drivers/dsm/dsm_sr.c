/*
 * dsm_sr.c
 *
 *  Created on: 26 Jul 2011
 *      Author: john
 */

#include <dsm/dsm_sr.h>
#include <dsm/dsm_core.h>
#include <dsm/dsm_stats.h>

static struct kmem_cache *kmem_request_cache;

void init_kmem_request_cache(void) {
    kmem_request_cache = kmem_cache_create("dsm_request",
            sizeof(struct dsm_request), 0, SLAB_HWCACHE_ALIGN | SLAB_TEMPORARY,
            NULL);
}

void destroy_kmem_request_cache(void) {
    kmem_cache_destroy(kmem_request_cache);
}

static struct dsm_request * get_dsm_request(void) {
    struct dsm_request * req = NULL;
    req = kmem_cache_alloc(kmem_request_cache, GFP_KERNEL);
    BUG_ON(!req);
    return req;
}
static void release_dsm_request(struct dsm_request *req) {
    kmem_cache_free(kmem_request_cache, req);
}

static int send_request_dsm_page_pull(struct subvirtual_machine *svm,
        struct subvirtual_machine *fault_svm, uint64_t addr) {

    struct conn_element * ele = svm->ele;
    struct tx_buffer *tx = &ele->tx_buffer;
    struct tx_buf_ele *tx_e;
    int ret = 0;
    struct dsm_request *req;

    printk("[send_request_dsm_page_pull]requesting page pull  addr %p\n",
            (void *) addr);

    spin_lock(&tx->request_queue_lock);
    if (list_empty(&tx->request_queue)) {
        spin_unlock(&tx->request_queue_lock);
        tx_e = try_get_next_empty_tx_ele(ele);
        if (tx_e) {
            create_page_pull_request(ele, tx_e, fault_svm->id, svm->id, addr);
            tx_e->callback.func = NULL;
            ret = tx_dsm_send(ele, tx_e);
            return ret;
        }
    } else {
        spin_unlock(&tx->request_queue_lock);
    }

    req = get_dsm_request();
    req->type = REQUEST_PAGE_PULL;
    req->addr = addr;
    req->fault_svm = fault_svm;
    req->func = NULL;
    req->svm = svm;

    spin_lock(&tx->request_queue_lock);
    list_add_tail(&req->queue, &tx->request_queue);
    spin_unlock(&tx->request_queue_lock);

    queue_work(ele->rcm->dsm_wq, &ele->recv_work);

    return ret;

}

int request_dsm_page(struct page * page, struct subvirtual_machine *svm,
        struct subvirtual_machine *fault_svm, uint64_t addr,
        void(*func)(struct tx_buf_ele *), int try) {

    struct conn_element * ele = svm->ele;
    struct tx_buffer *tx = &ele->tx_buffer;
    struct tx_buf_ele *tx_e;
    int ret = 0;
    struct dsm_request *req;

//    printk("[request_dsm_page]svm %p, ele %p txbuff %p , addr %p, try %d\n",
//            svm, svm->ele, tx, (void *) addr, try);

    spin_lock(&tx->request_queue_lock);
    if (list_empty(&tx->request_queue)) {
        spin_unlock(&tx->request_queue_lock);

        tx_e = try_get_next_empty_tx_ele(ele);
        if (tx_e) {

            if (try)
                create_page_request(ele, tx_e, fault_svm->id, svm->id, addr,
                        page, TRY_REQUEST_PAGE);

            else
                create_page_request(ele, tx_e, fault_svm->id, svm->id, addr,
                        page, REQUEST_PAGE);

            tx_e->callback.func = func;

            ret = tx_dsm_send(ele, tx_e);
            return ret;
        }
    } else
        spin_unlock(&tx->request_queue_lock);

    req = get_dsm_request();

    if (try)
        req->type = TRY_REQUEST_PAGE;
    else
        req->type = REQUEST_PAGE;

    req->addr = addr;
    req->fault_svm = fault_svm;
    req->func = func;
    req->page = page;
    req->svm = svm;

    spin_lock(&tx->request_queue_lock);
    list_add_tail(&req->queue, &tx->request_queue);
    spin_unlock(&tx->request_queue_lock);

    queue_work(ele->rcm->dsm_wq, &ele->recv_work);

    return ret;

}

/**
 * Read the message received in the wc, to get the offset and then find where the page has been written
 *
 * RETURN 0 if a page exists in the buffer at this offset
 *               -1 if the page cannot be found
 */
int process_response(struct conn_element *ele, struct tx_buf_ele * tx_buf_e) {

    if (tx_buf_e->callback.func)
        tx_buf_e->callback.func(tx_buf_e);

    release_replace_page(ele, tx_buf_e);
    release_tx_element(ele, tx_buf_e);

    return 0;
}

int rx_tx_message_transfer(struct conn_element * ele,
        struct rx_buf_ele * rx_buf_e) {
    struct page_pool_ele * ppe;
    struct tx_buf_ele *tx_e = NULL;
    struct page * page;
    int ret = 0;
    struct dsm_request *req;
    struct tx_buffer *tx;

    page = dsm_extract_page_from_remote(rx_buf_e->dsm_msg);
    if (unlikely(!page)) {

        if (rx_buf_e->dsm_msg->type == TRY_REQUEST_PAGE) {
            tx = &ele->tx_buffer;
            spin_lock(&tx->request_queue_lock);
            if (list_empty(&tx->request_queue)) {
                spin_unlock(&tx->request_queue_lock);

                tx_e = try_get_next_empty_tx_ele(ele);
                if (tx_e) {
                    memcpy(tx_e->dsm_msg, rx_buf_e->dsm_msg,
                            sizeof(struct dsm_message));
                    tx_e->dsm_msg->type = TRY_REQUEST_PAGE_FAIL;
                    tx_e->wrk_req->dst_addr = NULL;
                    tx_e->callback.func = NULL;
                    goto send;
                }
            } else {
                spin_unlock(&tx->request_queue_lock);
            }

            req = get_dsm_request();
            req->type = TRY_REQUEST_PAGE_FAIL;
            req->func = NULL;
            memcpy(&req->dsm_msg, rx_buf_e->dsm_msg,
                    sizeof(struct dsm_message));
            spin_lock(&tx->request_queue_lock);
            list_add_tail(&req->queue, &tx->request_queue);
            spin_unlock(&tx->request_queue_lock);
            queue_work(ele->rcm->dsm_wq, &ele->recv_work);
            return ret;

        } else {
            printk(
                    "[rx_tx_message_transfer][FATAL_ERROR] - Couldn't grab a the page\n");
            return -1;
        }
    }

    tx_e = try_get_next_empty_tx_reply_ele(ele);
    BUG_ON(!tx_e);

    ppe = create_new_page_pool_element_from_page(ele, page);
    BUG_ON(!ppe);

    memcpy(tx_e->dsm_msg, rx_buf_e->dsm_msg, sizeof(struct dsm_message));

    tx_e->dsm_msg->type = PAGE_REQUEST_REPLY;
    tx_e->reply_work_req->wr.wr.rdma.remote_addr = tx_e->dsm_msg->dst_addr;
    tx_e->reply_work_req->wr.wr.rdma.rkey = tx_e->dsm_msg->rkey;
    tx_e->wrk_req->dst_addr = ppe;
    tx_e->reply_work_req->page_sgl.addr = (u64) ppe->page_buf;

    send: ret = tx_dsm_send(ele, tx_e);

    return ret;
}

int tx_dsm_send(struct conn_element * ele, struct tx_buf_ele *tx_e) {
    int ret = 0;

    switch (tx_e->dsm_msg->type) {
        case REQUEST_PAGE: {

            ret = ib_post_send(ele->cm_id->qp, &tx_e->wrk_req->wr_ele->wr,
                    &tx_e->wrk_req->wr_ele->bad_wr);
            break;
        }
        case REQUEST_PAGE_PULL: {

            ret = ib_post_send(ele->cm_id->qp, &tx_e->wrk_req->wr_ele->wr,
                    &tx_e->wrk_req->wr_ele->bad_wr);
            break;
        }
        case TRY_REQUEST_PAGE: {

            ret = ib_post_send(ele->cm_id->qp, &tx_e->wrk_req->wr_ele->wr,
                    &tx_e->wrk_req->wr_ele->bad_wr);
            break;
        }
        case TRY_REQUEST_PAGE_FAIL: {

            ret = ib_post_send(ele->cm_id->qp, &tx_e->wrk_req->wr_ele->wr,
                    &tx_e->wrk_req->wr_ele->bad_wr);
            break;
        }
        case PAGE_REQUEST_REPLY: {
            ret = ib_post_send(ele->cm_id->qp, &tx_e->reply_work_req->wr,
                    &tx_e->reply_work_req->wr_ele->bad_wr);
            break;
        }

        default: {
            printk(">[tx_flush_queue][ERROR] - wrong message status\n");
            ret = 1;
        }
    }
    if (unlikely(ret))
        printk(">[tx_flush_queue][ERROR] - ib_post_send failed ret : %d\n",
                ret);

    return ret;
}

/**
 * Before the connection can be used, the nodes need to have these information about each other :
 *      u8      flag;
 *      u16 node_ip;
 *      u64 buf_msg_addr;
 *      u32 rkey_msg;
 *      u64 buf_rx_addr;
 *      u32 rkey_rx;
 *      u32 rx_buf_size;
 */
int exchange_info(struct conn_element *ele, int id) {
    int flag = (int) ele->rid.remote_info->flag;
    int ret = 0;
    struct conn_element * ele_found;

    if (unlikely(!ele->rid.recv_info))
        goto err;

    switch (flag) {
        case RDMA_INFO_CL: {
            --ele->rid.send_info->flag;
            goto recv_send;
        }
        case RDMA_INFO_SV: {
            ret = dsm_recv_info(ele);
            if (ret) {
                printk(
                        ">[exchange_info] - Could not post the receive work request\n");
                goto err;
            }
            ele->rid.send_info->flag = ele->rid.send_info->flag - 2;
            ret = setup_recv_wr(ele);
            goto send;
        }
        case RDMA_INFO_READY_CL: {
            ele->rid.send_info->flag = ele->rid.send_info->flag - 2;
            ret = setup_recv_wr(ele);
            refill_recv_wr(ele,
                    &ele->rx_buffer.rx_buf[RX_BUF_ELEMENTS_NUM - 1]);
            ele->rid.remote_info->flag = RDMA_INFO_NULL;

            ele->remote_node_ip = (int) ele->rid.remote_info->node_ip;
            ele_found = search_rb_conn(ele->remote_node_ip);

            // We find that a connection is already open with that node - delete this connection request.
            if (ele_found) {
                if (ele->remote_node_ip != get_rcm()->node_ip) {
                    printk(
                            ">[exchange_info] - destroy_connection duplicate : %d\n former : %d\n",
                            ele->remote_node_ip, ele_found->remote_node_ip);
                    rdma_disconnect(ele->cm_id);
                } else {
                    printk("loopback, lets hope for the best\n");
                }
            }
            //ok, inserting this connection to the tree
            else {
                insert_rb_conn(ele);
                printk(
                        ">[exchange_info] inserted conn_element to rb_tree :  %d\n",
                        ele->remote_node_ip);
            }

            goto send;

        }
        case RDMA_INFO_READY_SV: {
            refill_recv_wr(ele,
                    &ele->rx_buffer.rx_buf[RX_BUF_ELEMENTS_NUM - 1]);

            ele->rid.remote_info->flag = RDMA_INFO_NULL;
            //Server acknowledged --> connection is complete.
            //start sending messages.
            goto out;
        }
        default: {
            printk(">[exchange_info] - UNKNOWN RDMA INFO FLAG\n");
            goto out;
        }
    }

    recv_send: ret = dsm_recv_info(ele);
    if (ret) {
        printk(">[exchange_info] - Could not post the receive work request\n");
        goto err;
    }

    send: ret = dsm_send_info(ele);
    if (ret < 0) {
        printk(">[exchange_info] - Could not post the send work request\n");
        goto err;
    }

    out: return ret;

    err: printk(">[exchange_info] - No receive info\n");
    return ret;

}

/**
 * Creating and posting the work request that sends its info over.
 *
 * RETURN dsm_post_send
 */

int dsm_send_info(struct conn_element *ele) {
    struct rdma_info_data *rid = &ele->rid;

    rid->send_sge.addr = (u64) rid->send_info;
    rid->send_sge.length = sizeof(struct rdma_info);
    rid->send_sge.lkey = ele->mr->lkey;

    rid->send_wr.next = NULL;
    rid->send_wr.wr_id = 0;
    rid->send_wr.sg_list = &rid->send_sge;
    rid->send_wr.num_sge = 1;
    rid->send_wr.opcode = IB_WR_SEND;
    rid->send_wr.send_flags = IB_SEND_SIGNALED;
    printk(">[dsm_send_info] - sending info\n");
    return ib_post_send(ele->cm_id->qp, &rid->send_wr, &rid->send_bad_wr);
}

/**
 * Creating and posting the work request that receives remote info
 *
 * RETURN ib_post_recv
 */
int dsm_recv_info(struct conn_element *ele) {
    struct rdma_info_data *rid = &ele->rid;

    rid->recv_sge.addr = (u64) rid->recv_info;
    rid->recv_sge.length = sizeof(struct rdma_info);
    rid->recv_sge.lkey = ele->mr->lkey;

    rid->recv_wr.next = NULL;
    rid->recv_wr.wr_id = 0; // DSM2: unique id - address of data_struct
    rid->recv_wr.num_sge = 1;
    rid->recv_wr.sg_list = &rid->recv_sge;

    return ib_post_recv(ele->cm_id->qp, &rid->recv_wr, &rid->recv_bad_wr);

}

int dsm_request_page_pull(struct mm_struct *mm, struct subvirtual_machine *svm,
        struct subvirtual_machine *fault_svm, unsigned long request_addr) {
    int ret = -1;
    unsigned long addr = request_addr & PAGE_MASK;
    down_read(&mm->mmap_sem);
    ret = dsm_try_push_page(mm, svm->id, addr);
    up_read(&mm->mmap_sem);

    if (!ret)
        send_request_dsm_page_pull(svm, fault_svm,
                (uint64_t) (addr - fault_svm->priv->offset));

    return ret;

}

