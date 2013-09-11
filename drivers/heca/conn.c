/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 */
#include <linux/list.h>

#include "hutils.h"
#include "hproc.h"
#include "hspace.h"

#include "ioctl.h"
#include "trace.h"
#include "conn.h"
#include "base.h"
#include "struct.h"
#include "ops.h"
#include "transport.h"
#include "hecatonchire.h"

#define ntohll(x) be64_to_cpu(x)
#define htonll(x) cpu_to_be64(x)

static struct kmem_cache *kmem_heca_request_cache;

unsigned long inet_addr(const char *cp)
{
        unsigned int a, b, c, d;
        unsigned char arr[4];

        sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d);
        arr[0] = a;
        arr[1] = b;
        arr[2] = c;
        arr[3] = d;
        return *(unsigned int*) arr; /* network */
}

char *inet_ntoa(unsigned long s_addr, char *buf, int sz)
{
        unsigned char *b = (unsigned char *)&s_addr;

        if (!sz)
                return NULL;

        buf[0] = 0;
        snprintf(buf, sz - 1, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return buf;
}

char *port_ntoa(unsigned short port, char *buf, int sz)
{
        if (!sz)
                return NULL;

        buf[0] = 0;
        snprintf(buf, sz - 1, "%u", ntohs(port));
        return buf;
}

char *sockaddr_ntoa(struct sockaddr_in *sa, char *buf, int sz)
{
        char ip_str[20], port_str[10];

        if (!sz)
                return NULL;

        buf[0] = 0;
        inet_ntoa(sa->sin_addr.s_addr, ip_str, sizeof ip_str);
        port_ntoa(sa->sin_port, port_str, sizeof port_str);
        snprintf(buf, sz - 1, "%s:%s", ip_str, port_str);
        return buf;
}

char *conn_ntoa(struct sockaddr_in *local, struct sockaddr_in *remote,
                char *buf, int sz)
{
        char local_str[35], remote_str[35];

        if (!sz)
                return NULL;

        sockaddr_ntoa(local, local_str, sizeof local_str);
        sockaddr_ntoa(remote, remote_str, sizeof remote_str);
        buf[0] = 0;
        snprintf(buf, sz - 1, "%s-%s", local_str, remote_str);
        return buf;
}

static inline void init_kmem_request_cache_elm(void *obj)
{
        struct heca_request *req = (struct heca_request *) obj;
        memset(req, 0, sizeof(struct heca_request));
}

void init_kmem_heca_request_cache(void)
{
        kmem_heca_request_cache = kmem_cache_create("heca_request",
                        sizeof(struct heca_request), 0,
                        SLAB_HWCACHE_ALIGN | SLAB_TEMPORARY,
                        init_kmem_request_cache_elm);
}

void destroy_kmem_heca_request_cache(void)
{
        kmem_cache_destroy(kmem_heca_request_cache);
}

inline struct heca_request *alloc_heca_request(void)
{
        return kmem_cache_alloc(kmem_heca_request_cache, GFP_KERNEL);
}

inline void release_heca_request(struct heca_request *req)
{
        kmem_cache_free(kmem_heca_request_cache, req);
}

static inline int get_nb_tx_buff_elements(struct heca_connection *conn)
{
        return conn->qp_attr.cap.max_send_wr >> 1;
}

static inline int get_nb_rx_buff_elements(struct heca_connection *conn)
{
        return conn->qp_attr.cap.max_recv_wr;
}

static int get_max_pushed_reqs(struct heca_connection *conn)
{
        return get_nb_tx_buff_elements(conn) << 2;
}

static void schedule_delayed_request_flush(struct heca_connection *conn)
{
        schedule_work(&conn->delayed_request_flush_work);
}

static inline void queue_heca_request(struct heca_connection *conn,
                struct heca_request *req)
{
        trace_heca_queued_request(req->hspace_id, req->local_hproc_id,
                        req->remote_hproc_id, req->hmr_id, 0, req->addr,
                        req->type, -1);
        llist_add(&req->lnode, &conn->tx_buffer.request_queue);
        schedule_delayed_request_flush(conn);
}

int add_heca_request(struct heca_request *req, struct heca_connection *conn,
                u16 type, u32 hspace_id, u32 src_id, u32 mr_id, u32 dest_id,
                unsigned long addr, int (*func)(struct tx_buffer_element *),
                struct heca_page_cache *hpc, struct page *page,
                struct heca_page_pool_element *ppe, int need_ppe,
                struct heca_message *msg)
{
        if (!req) {
                req = kmem_cache_alloc(kmem_heca_request_cache, GFP_KERNEL);
                if (unlikely(!req))
                        return -ENOMEM;
        }

        req->type = type;
        req->hspace_id = hspace_id;
        req->hmr_id = mr_id;
        req->local_hproc_id = src_id;
        req->remote_hproc_id = dest_id;
        req->addr = addr;
        req->func = func;
        req->hpc = hpc;
        req->page = page;
        req->ppe = ppe;
        req->need_ppe = need_ppe;

        if (msg) {
                heca_msg_cpy(&req->hmsg, msg);
                req->response = 1;
        } else {
                req->response = 0;
        }

        queue_heca_request(conn, req);

        return 0;
}

inline int heca_request_queue_empty(struct heca_connection *conn)
{
        /* we are not 100% accurate but it's ok we can have a few sneaking in */
        return (llist_empty(&conn->tx_buffer.request_queue) &&
                        list_empty(&conn->tx_buffer.ordered_request_queue));
}

inline int heca_request_queue_full(struct heca_connection *conn)
{
        return conn->tx_buffer.request_queue_sz > get_max_pushed_reqs(conn);
}

/* this will copy the offset and rkey of the original and send them back! */
static inline void heca_tx_response_prepare(struct tx_buffer_element *tx_e,
                struct heca_message *msg)
{
        heca_msg_cpy(tx_e->hmsg_buffer, msg);
        tx_e->wrk_req->dst_addr = NULL;
}

static void heca_tx_prepare(struct heca_connection *conn,
                struct tx_buffer_element *tx_e, u32 hspace_id, u32 mr_id,
                u32 src_id, u32 dest_id, unsigned long shared_addr,
                struct heca_page_cache *hpc, struct page *page,
                struct heca_page_pool_element *ppe, int need_ppe)
{
        struct heca_message *msg = tx_e->hmsg_buffer;

        while (need_ppe && !ppe) {
                might_sleep();
                ppe = heca_prepare_ppe(conn, page);
                if (likely(ppe))
                        break;
                cond_resched();
        }

        msg->offset = tx_e->id;
        msg->hspace_id = hspace_id;
        msg->src_id = src_id;
        msg->dest_id = dest_id;
        msg->mr_id = mr_id;
        msg->req_addr = (u64) shared_addr;
        msg->dst_addr = (u64) (need_ppe? ppe->page_buf : 0);
        msg->rkey = conn->mr->rkey; /* TODO: is this needed? */

        tx_e->wrk_req->dst_addr = ppe;
        tx_e->wrk_req->hpc = hpc;
        tx_e->reply_work_req->mem_page = page;
}

int heca_send_tx_e(struct heca_connection *conn, struct tx_buffer_element *tx_e,
                int resp, int type, u32 hspace_id, u32 mr_id,
                u32 src_id, u32 dest_id, unsigned long local_addr,
                unsigned long shared_addr, struct heca_page_cache *hpc,
                struct page *page, struct heca_page_pool_element *ppe,
                int need_ppe, int (*func)(struct tx_buffer_element *),
                struct heca_message *msg)
{
        if (resp) {
                heca_tx_response_prepare(tx_e, msg);
        } else {
                heca_tx_prepare(conn, tx_e, hspace_id, mr_id, src_id, dest_id,
                                shared_addr, hpc, page, ppe, need_ppe);
        }

        tx_e->hmsg_buffer->type = type;
        tx_e->callback.func = func;

        trace_heca_send_request(hspace_id, src_id, dest_id, mr_id, local_addr,
                        shared_addr, type);

        return tx_heca_send(conn, tx_e);
}

void heca_request_queue_merge(struct tx_buffer *tx)
{
        struct list_head *head = &tx->ordered_request_queue;
        struct llist_node *llnode = llist_del_all(&tx->request_queue);

        while (llnode) {
                struct heca_request *req;

                req = container_of(llnode, struct heca_request, lnode);
                list_add_tail(&req->ordered_list, head);
                head = &req->ordered_list;
                llnode = llnode->next;
                tx->request_queue_sz++;
        }
}

static inline int flush_heca_request_queue(struct heca_connection *conn)
{
        struct tx_buffer *tx = &conn->tx_buffer;
        struct heca_request *req;
        struct tx_buffer_element *tx_e = NULL;
        int ret = 0;

        mutex_lock(&tx->flush_mutex);
        heca_request_queue_merge(tx);
        while (!list_empty(&tx->ordered_request_queue)) {
                tx_e = try_get_next_empty_tx_ele(conn, 0);
                if (!tx_e) {
                        ret = 1;
                        break;
                }
                tx->request_queue_sz--;
                req = list_first_entry(&tx->ordered_request_queue,
                                struct heca_request, ordered_list);
                trace_heca_flushing_requests(tx->request_queue_sz);
                heca_send_tx_e(conn, tx_e, req->response, req->type,
                                req->hspace_id, req->hmr_id,
                                req->local_hproc_id, req->remote_hproc_id, 0,
                                req->addr, req->hpc, req->page, req->ppe,
                                req->need_ppe, req->func, &req->hmsg);
                list_del(&req->ordered_list);
                release_heca_request(req);
        }
        mutex_unlock(&tx->flush_mutex);
        return ret;

}

static void delayed_request_flush_work_fn(struct work_struct *w)
{
        struct heca_connection *conn;
        udelay(REQUEST_FLUSH_DELAY);
        conn = container_of(w, struct heca_connection,
                        delayed_request_flush_work);
        if (flush_heca_request_queue(conn))
                schedule_delayed_request_flush(conn);
}

static void destroy_connection_work(struct work_struct *work)
{
        struct heca_transport_manager *htm = get_heca_module_state()->htm;
        struct rb_root *root;
        struct rb_node *node, *next;
        struct heca_connection *conn;
        unsigned long seq;

        do {
                seq = read_seqbegin(&htm->connections_lock);
                root = &htm->connections_rb_tree_root;
                for (node = rb_first(root); node; node = next) {
                        conn = rb_entry(node, struct heca_connection, rb_node);
                        next = rb_next(node);
                        if (atomic_cmpxchg(&conn->alive, -1, 0) == -1)
                                destroy_connection(conn);
                }
        } while (read_seqretry(&htm->connections_lock, seq));

        kfree(work);
}

static inline void schedule_destroy_conns(void)
{
        struct work_struct *work = kmalloc(sizeof(struct work_struct),
                        GFP_KERNEL);
        INIT_WORK(work, destroy_connection_work);
        schedule_work(work);
}

static inline void queue_recv_work(struct heca_connection *conn)
{
        rcu_read_lock();
        if (atomic_read(&conn->alive))
                queue_work(get_heca_module_state()->heca_rx_wq,
                                &conn->recv_work);
        rcu_read_unlock();
}

static inline void queue_send_work(struct heca_connection *conn)
{
        rcu_read_lock();
        if (atomic_read(&conn->alive))
                queue_work(get_heca_module_state()->heca_tx_wq,
                                &conn->send_work);
        rcu_read_unlock();
}

static inline void handle_tx_element(struct heca_connection *conn,
                struct tx_buffer_element *tx_e,
                int (*callback)(struct heca_connection *,
                        struct tx_buffer_element *))
{
        /* if tx_e->used > 2, we're racing with release_heca_tx_elements */
        if (atomic_add_return(1, &tx_e->used) == 2) {
                if (callback)
                        callback(conn, tx_e);
                try_release_tx_element(conn, tx_e);
        }
}

static int refill_recv_wr(struct heca_connection *conn,
                struct rx_buffer_element *rx_e)
{
        int ret = 0;
        ret = ib_post_recv(conn->cm_id->qp, &rx_e->recv_wrk_rq_ele->sq_wr,
                        &rx_e->recv_wrk_rq_ele->bad_wr);
        if (ret)
                heca_printk(KERN_ERR "Failed ib_post_recv(offset=%d): %d",
                                rx_e->id, ret);

        return ret;
}

static int heca_recv_message_handler(struct heca_connection *conn,
                struct rx_buffer_element *rx_e)
{
        struct tx_buffer_element *tx_e = NULL;

        trace_heca_rx_msg(rx_e->hmsg_buffer->hspace_id,
                        rx_e->hmsg_buffer->src_id, rx_e->hmsg_buffer->dest_id,
                        rx_e->hmsg_buffer->mr_id, 0,
                        rx_e->hmsg_buffer->req_addr, rx_e->hmsg_buffer->type,
                        rx_e->hmsg_buffer->offset);

        switch (rx_e->hmsg_buffer->type) {
        case MSG_RES_PAGE:
                BUG_ON(rx_e->hmsg_buffer->offset < 0 ||
                                rx_e->hmsg_buffer->offset >=
                                conn->tx_buffer.len);
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                handle_tx_element(conn, tx_e, process_page_response);
                break;
        case MSG_RES_PAGE_REDIRECT:
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                process_page_redirect(conn, tx_e, rx_e->hmsg_buffer->dest_id);
                break;
        case MSG_RES_PAGE_FAIL:
                BUG_ON(rx_e->hmsg_buffer->offset < 0 ||
                                rx_e->hmsg_buffer->offset >=
                                conn->tx_buffer.len);
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                tx_e->hmsg_buffer->type = MSG_RES_PAGE_FAIL;
                handle_tx_element(conn, tx_e, process_page_response);
                break;
        case MSG_REQ_PUSHED_PAGE:
        case MSG_REQ_PAGE:
        case MSG_REQ_READ:
                process_page_request_msg(conn, rx_e->hmsg_buffer);
                break;
        case MSG_REQ_CLAIM:
        case MSG_REQ_CLAIM_TRY:
                process_page_claim(conn, rx_e->hmsg_buffer);
                break;
        case MSG_REQ_PUSH:
                process_pull_request(conn, rx_e);
                ack_msg(conn, rx_e->hmsg_buffer, MSG_RES_ACK);
                break;
        case MSG_RES_HPROC_FAIL:
                process_hproc_status(conn, rx_e);
                break;
        case MSG_RES_ACK:
        case MSG_RES_ACK_FAIL:
                BUG_ON(rx_e->hmsg_buffer->offset < 0 ||
                                rx_e->hmsg_buffer->offset >=
                                conn->tx_buffer.len);
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                if (tx_e->hmsg_buffer->type &
                                (MSG_REQ_CLAIM | MSG_REQ_CLAIM_TRY))
                        process_claim_ack(conn, tx_e, rx_e->hmsg_buffer);
                handle_tx_element(conn, tx_e, NULL);
                break;
        case MSG_REQ_QUERY:
                process_request_query(conn, rx_e);
                break;
        case MSG_RES_QUERY:
                BUG_ON(rx_e->hmsg_buffer->offset < 0 ||
                                rx_e->hmsg_buffer->offset >=
                                conn->tx_buffer.len);
                tx_e = &conn->tx_buffer.tx_buf[rx_e->hmsg_buffer->offset];
                process_query_info(tx_e);
                handle_tx_element(conn, tx_e, NULL);
                break;
        default:
                heca_printk(KERN_ERR "unhandled message stats addr: %p, status %d id %d",
                                rx_e, rx_e->hmsg_buffer->type, rx_e->id);
                goto err;
        }

        refill_recv_wr(conn, rx_e);
        return 0;
err:
        return 1;
}

static int heca_send_message_handler(struct heca_connection *conn,
                struct tx_buffer_element *tx_e)
{
        trace_heca_tx_msg(tx_e->hmsg_buffer->hspace_id,
                        tx_e->hmsg_buffer->src_id,
                        tx_e->hmsg_buffer->dest_id, -1, 0,
                        tx_e->hmsg_buffer->req_addr, tx_e->hmsg_buffer->type,
                        tx_e->hmsg_buffer->offset);

        switch (tx_e->hmsg_buffer->type) {
        case MSG_RES_PAGE:
                if (!pte_present(tx_e->reply_work_req->pte)) {
                        heca_clear_swp_entry_flag(tx_e->reply_work_req->mm,
                                        tx_e->reply_work_req->addr,
                                        tx_e->reply_work_req->pte,
                                        HECA_INFLIGHT_BITPOS);
                }
                heca_ppe_clear_release(conn, &tx_e->wrk_req->dst_addr);
                release_tx_element_reply(conn, tx_e);
                break;

                /* we can immediately discard the tx_e */
        case MSG_RES_ACK:
        case MSG_RES_ACK_FAIL:
        case MSG_RES_PAGE_FAIL:
        case MSG_RES_HPROC_FAIL:
        case MSG_RES_QUERY:
        case MSG_RES_PAGE_REDIRECT:
                release_tx_element(conn, tx_e);
                break;

                /* we keep the tx_e alive, to process the response */
        case MSG_REQ_PAGE:
        case MSG_REQ_READ:
        case MSG_REQ_PUSHED_PAGE:
        case MSG_REQ_PUSH:
        case MSG_REQ_CLAIM:
        case MSG_REQ_CLAIM_TRY:
        case MSG_REQ_QUERY:
                try_release_tx_element(conn, tx_e);
                break;
        default:
                heca_printk(KERN_ERR "unhandled message stats  addr: %p, status %d , id %d",
                                tx_e, tx_e->hmsg_buffer->type, tx_e->id);
                return 1;
        }
        return 0;
}

/* FIXME handle cq events*/
static void heca_cq_event_handler(struct ib_event *event, void *data)
{
        heca_printk(KERN_DEBUG "event %u  data %p", event->event, data);
}

/*FIXME handle errors */
void listener_cq_handle(struct ib_cq *cq, void *cq_context)
{
        struct ib_wc wc;
        int ret = 0;

        if (ib_req_notify_cq(cq, IB_CQ_SOLICITED))
                heca_printk(KERN_INFO "Failed ib_req_notify_cq");

        if ((ret = ib_poll_cq(cq, 1, &wc)) > 0) {
                if (likely(wc.status == IB_WC_SUCCESS)) {
                        switch (wc.opcode) {
                        case IB_WC_RECV:
                                break;
                        default: {
                                heca_printk(KERN_ERR "expected opcode %d got %d",
                                                IB_WC_SEND, wc.opcode);
                                break;
                        }
                        }
                } else
                        heca_printk(KERN_ERR "Unexpected type of wc");
        } else if (unlikely(ret < 0)) {
                heca_printk(KERN_ERR "recv FAILUREi %d", ret);
        }
}

static void heca_send_poll(struct ib_cq *cq)
{
        struct ib_wc wc;
        struct heca_connection *ele = (struct heca_connection *) cq->cq_context;

        while (ib_poll_cq(cq, 1, &wc) > 0) {
                if (unlikely(wc.status != IB_WC_SUCCESS ||
                                        wc.opcode != IB_WC_SEND))
                        continue;

                if (unlikely(ele->rid.exchanged))
                        ele->rid.exchanged--;
                else
                        heca_send_message_handler(ele,
                                        &ele->tx_buffer.tx_buf[wc.wr_id]);
        }
}

static void reg_rem_info(struct heca_connection *conn)
{
        conn->rid.remote_info->node_ip = ntohl(conn->rid.recv_buf->node_ip);
        conn->rid.remote_info->buf_rx_addr = ntohll(conn->rid.recv_buf->buf_rx_addr);
        conn->rid.remote_info->buf_msg_addr = ntohll(conn->rid.recv_buf->buf_msg_addr);
        conn->rid.remote_info->rx_buf_size = ntohl(conn->rid.recv_buf->rx_buf_size);
        conn->rid.remote_info->rkey_msg = ntohl(conn->rid.recv_buf->rkey_msg);
        conn->rid.remote_info->rkey_rx = ntohl(conn->rid.recv_buf->rkey_rx);
        conn->rid.remote_info->flag = conn->rid.recv_buf->flag;
}

static int heca_send_info(struct heca_connection *conn)
{
        struct rdma_info_data *rid = &conn->rid;

        rid->send_sge.addr = rid->send_dma.addr;
        rid->send_sge.length = rid->send_dma.size;
        rid->send_sge.lkey = conn->mr->lkey;

        rid->send_wr.next = NULL;
        rid->send_wr.wr_id = 0;
        rid->send_wr.sg_list = &rid->send_sge;
        rid->send_wr.num_sge = 1;
        rid->send_wr.opcode = IB_WR_SEND;
        rid->send_wr.send_flags = IB_SEND_SIGNALED;

        return ib_post_send(conn->cm_id->qp, &rid->send_wr, &rid->send_bad_wr);
}

static int heca_recv_info(struct heca_connection *conn)
{
        struct rdma_info_data *rid = &conn->rid;

        rid->recv_sge.addr = rid->recv_dma.addr;
        rid->recv_sge.length = rid->recv_dma.size;
        rid->recv_sge.lkey = conn->mr->lkey;

        rid->recv_wr.next = NULL;
        rid->recv_wr.wr_id = 0; // HECA2: unique id - address of data_struct
        rid->recv_wr.num_sge = 1;
        rid->recv_wr.sg_list = &rid->recv_sge;

        return ib_post_recv(conn->cm_id->qp, &rid->recv_wr, &rid->recv_bad_wr);
}

static int setup_recv_wr(struct heca_connection *conn)
{
        int i;
        struct rx_buffer_element *rx = conn->rx_buffer.rx_buf;

        if (unlikely(!rx))
                return -1;

        /* last rx elm reserved for initial info exchange */
        for (i = 0; i < conn->rx_buffer.len - 1; ++i) {
                if (refill_recv_wr(conn, &rx[i]))
                        return -1;
        }
        return 0;
}

static int exchange_info(struct heca_connection *conn, int id)
{
        int flag = (int) conn->rid.remote_info->flag;
        int ret = 0;
        struct heca_connection * conn_found;

        BUG_ON(!conn);

        if (unlikely(!conn->rid.recv_buf))
                goto err;
        flag = (int) conn->rid.remote_info->flag;

        switch (flag) {
        case RDMA_INFO_CL: {
                conn->rid.send_buf->flag = RDMA_INFO_SV;
                goto recv_send;
        }
        case RDMA_INFO_SV: {
                ret = heca_recv_info(conn);
                if (ret) {
                        heca_printk("could not post the receive work request");
                        goto err;
                }
                conn->rid.send_buf->flag = RDMA_INFO_READY_CL;
                ret = setup_recv_wr(conn);
                goto send;
        }
        case RDMA_INFO_READY_CL: {
                conn->rid.send_buf->flag = RDMA_INFO_READY_SV;
                ret = setup_recv_wr(conn);
                refill_recv_wr(conn,
                                &conn->rx_buffer.rx_buf[conn->rx_buffer.len - 1]);
                conn->rid.remote_info->flag = RDMA_INFO_NULL;

                conn->remote_node_ip = (u32) conn->rid.remote_info->node_ip;
                conn->remote.sin_addr.s_addr = (u32) conn->rid.remote_info->node_ip;
                conn->local = get_heca_module_state()->htm->sin;
                conn_found = search_rb_conn(conn->remote_node_ip);

                if (conn_found) {
                        if (conn->remote_node_ip !=
                                        get_heca_module_state()->htm->node_ip) {
                                char curr[20], prev[20];

                                inet_ntoa(conn->remote_node_ip,
                                                curr, sizeof curr);
                                inet_ntoa(conn_found->remote_node_ip,
                                                prev, sizeof prev);
                                heca_printk("destroy_connection duplicate: %s former: %s",
                                                curr, prev);
                                rdma_disconnect(conn->cm_id);
                        } else {
                                heca_printk("loopback, lets hope for the best");
                        }
                        erase_rb_conn(conn);
                } else {
                        char curr[20];

                        complete(&conn->completion);
                        insert_rb_conn(conn);
                        inet_ntoa(conn->remote_node_ip, curr, sizeof curr);
                        heca_printk("inserted conn_element to rb_tree: %s",
                                        curr);
                }
                goto send;

        }
        case RDMA_INFO_READY_SV: {
                refill_recv_wr(conn, &conn->rx_buffer.rx_buf[conn->rx_buffer.len - 1]);
                conn->rid.remote_info->flag = RDMA_INFO_NULL;
                //Server acknowledged --> connection is complete.
                //start sending messages.
                complete(&conn->completion);
                goto out;
        }
        default: {
                heca_printk(KERN_ERR "unknown RDMA info flag");
                goto out;
        }
        }

recv_send:
        ret = heca_recv_info(conn);
        if (ret < 0) {
                heca_printk(KERN_ERR "could not post the receive work request");
                goto err;
        }

send:
        ret = heca_send_info(conn);
        if (ret < 0) {
                heca_printk(KERN_ERR "could not post the send work request");
                goto err;
        }

out:
        return ret;

err:
        heca_printk(KERN_ERR "no receive info");
        return ret;
}

static void conn_recv_poll(struct ib_cq *cq)
{
        struct ib_wc wc;
        struct heca_connection *conn = (struct heca_connection *) cq->cq_context;

        while (ib_poll_cq(cq, 1, &wc) == 1) {
                if (likely(wc.status == IB_WC_SUCCESS)) {
                        if (unlikely(wc.opcode != IB_WC_RECV)) {
                                heca_printk(KERN_INFO "expected opcode %d got %d",
                                                IB_WC_RECV, wc.opcode);
                                continue;
                        }
                } else {
                        if (wc.status == IB_WC_WR_FLUSH_ERR) {
                                heca_printk(KERN_INFO "rx id %llx status %d vendor_err %x",
                                                wc.wr_id, wc.status,
                                                wc.vendor_err);
                        } else {
                                heca_printk(KERN_ERR "rx id %llx status %d vendor_err %x",
                                                wc.wr_id, wc.status,
                                                wc.vendor_err);
                        }
                        continue;
                }

                if (conn->rid.remote_info->flag) {
                        BUG_ON(wc.byte_len != sizeof(struct heca_rdma_info));
                        reg_rem_info(conn);
                        exchange_info(conn, wc.wr_id);
                } else {
                        BUG_ON(wc.byte_len != sizeof(struct heca_message));
                        BUG_ON(wc.wr_id < 0 || wc.wr_id >= conn->rx_buffer.len);
                        heca_recv_message_handler(conn,
                                        &conn->rx_buffer.rx_buf[wc.wr_id]);
                }
        }
}

static void send_cq_handle_work(struct work_struct *work)
{
        struct heca_connection *conn = container_of(work,
                        struct heca_connection, send_work);
        int ret = 0;

        heca_send_poll(conn->qp_attr.send_cq);
        ret = ib_req_notify_cq(conn->qp_attr.send_cq,
                        IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS);
        heca_send_poll(conn->qp_attr.send_cq);
        if (ret > 0)
                queue_send_work(conn);
}

static void recv_cq_handle_work(struct work_struct *work)
{
        struct heca_connection *conn = container_of(work,
                        struct heca_connection, recv_work);
        int ret = 0;

        conn_recv_poll(conn->qp_attr.recv_cq);
        ret = ib_req_notify_cq(conn->qp_attr.recv_cq,
                        IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS);
        conn_recv_poll(conn->qp_attr.recv_cq);
        if (ret > 0)
                queue_recv_work(conn);
}

static void send_cq_handle(struct ib_cq *cq, void *cq_context)
{
        queue_send_work((struct heca_connection *) cq->cq_context);
}

static void recv_cq_handle(struct ib_cq *cq, void *cq_context)
{
        queue_recv_work((struct heca_connection *) cq->cq_context);
}

static int connect_client(struct rdma_cm_id *id)
{
        int r;
        struct rdma_conn_param param;

        memset(&param, 0, sizeof(struct rdma_conn_param));
        param.responder_resources = 1;
        param.initiator_depth = 1;
        param.retry_count = 10;

        r = rdma_connect(id, &param);
        if (r)
                heca_printk(KERN_ERR "Failed rdma_connect: %d", r);

        return r;
}

static inline void setup_IB_attr(struct ib_qp_init_attr *attr,
                struct ib_device_attr dev_attr)
{
        attr->cap.max_send_wr = min(dev_attr.max_qp_wr, IB_MAX_CAP_SCQ);
        attr->cap.max_recv_wr = min(dev_attr.max_qp_wr, IB_MAX_CAP_RCQ);
        attr->cap.max_send_sge = min(dev_attr.max_sge, IB_MAX_SEND_SGE);
        attr->cap.max_recv_sge = min(dev_attr.max_sge, IB_MAX_RECV_SGE);
}

static inline void setup_IW_attr(struct ib_qp_init_attr *attr,
                struct ib_device_attr dev_attr)
{
        attr->cap.max_send_wr = min(dev_attr.max_qp_wr, IW_MAX_CAP_SCQ);
        attr->cap.max_recv_wr = min(dev_attr.max_qp_wr, IW_MAX_CAP_RCQ);
        attr->cap.max_send_sge = min(dev_attr.max_sge, IW_MAX_SEND_SGE);
        attr->cap.max_recv_sge = min(dev_attr.max_sge, IW_MAX_RECV_SGE);
}

static inline int setup_qp_attr(struct heca_connection *conn)
{
        struct ib_qp_init_attr * attr = &conn->qp_attr;
        int ret = -1;
        struct ib_device_attr dev_attr;

        if (ib_query_device(conn->cm_id->device, &dev_attr)) {
                heca_printk("Query device failed for %s",
                                conn->cm_id->device->name);
                goto out;
        }
        attr->sq_sig_type = IB_SIGNAL_ALL_WR;
        attr->qp_type = IB_QPT_RC;
        attr->port_num = conn->cm_id->port_num;
        attr->qp_context = (void *) conn;
        switch (rdma_node_get_transport(conn->cm_id->device->node_type)) {
        case RDMA_TRANSPORT_IB:
                setup_IB_attr(attr, dev_attr);
                break;
        case RDMA_TRANSPORT_IWARP:
                setup_IW_attr(attr, dev_attr);
                break;
        default:
                return -1;
        }

        ret = 0;
out:
        return ret;
}

static int create_qp(struct heca_connection *conn)
{
        int ret = -1;
        struct ib_qp_init_attr * attr;

        attr = &conn->qp_attr;

        if (unlikely(!conn->cm_id))
                goto exit;

        if (unlikely(!conn->pd))
                goto exit;

        ret = rdma_create_qp(conn->cm_id, conn->pd, attr);

exit:
        return ret;
}

static int setup_qp(struct heca_connection *conn)
{
        int ret = 0;

        INIT_WORK(&conn->send_work, send_cq_handle_work);
        INIT_WORK(&conn->recv_work, recv_cq_handle_work);

        conn->qp_attr.send_cq = ib_create_cq(conn->cm_id->device,
                        send_cq_handle, heca_cq_event_handler, (void *) conn,
                        conn->qp_attr.cap.max_send_wr, 0);
        if (IS_ERR(conn->qp_attr.send_cq)) {
                heca_printk(KERN_ERR "Cannot create cq");
                goto err1;
        }

        if (ib_req_notify_cq(conn->qp_attr.send_cq, IB_CQ_NEXT_COMP)) {
                heca_printk(KERN_ERR "Cannot notify cq");
                goto err2;
        }

        conn->qp_attr.recv_cq = ib_create_cq(conn->cm_id->device,
                        recv_cq_handle, heca_cq_event_handler, (void *) conn,
                        conn->qp_attr.cap.max_recv_wr,0);
        if (IS_ERR(conn->qp_attr.recv_cq)) {
                heca_printk(KERN_ERR "Cannot create cq");
                goto err3;
        }

        if (ib_req_notify_cq(conn->qp_attr.recv_cq, IB_CQ_NEXT_COMP)) {
                heca_printk(KERN_ERR "Cannot notify cq");
                goto err4;
        }

        if (create_qp(conn)) {
                goto err5;
                heca_printk(KERN_ERR "QP not created --> Cancelled");
        }

        return ret;

err5: ret++;
err4: ret++;
      ib_destroy_cq(conn->qp_attr.recv_cq);
err3: ret++;
err2: ret++;
      ib_destroy_cq(conn->qp_attr.send_cq);
err1: ret++;
      heca_printk(KERN_ERR "Could not setup the qp, error %d occurred", ret);
      return ret;
}

static void init_tx_wr(struct tx_buffer_element *tx_ele, u32 lkey, int id)
{
        BUG_ON(!tx_ele);
        BUG_ON(!tx_ele->wrk_req);
        BUG_ON(!tx_ele->wrk_req->wr_ele);

        tx_ele->wrk_req->wr_ele->wr.wr_id = (u64) id;
        tx_ele->wrk_req->wr_ele->wr.opcode = IB_WR_SEND;
        tx_ele->wrk_req->wr_ele->wr.send_flags = IB_SEND_SIGNALED;
        tx_ele->wrk_req->wr_ele->wr.num_sge = 1;
        tx_ele->wrk_req->wr_ele->wr.sg_list =
                (struct ib_sge *) &tx_ele->wrk_req->wr_ele->sg;

        tx_ele->wrk_req->wr_ele->sg.addr = tx_ele->heca_dma.addr;
        tx_ele->wrk_req->wr_ele->sg.length = tx_ele->heca_dma.size;
        tx_ele->wrk_req->wr_ele->sg.lkey = lkey;

        tx_ele->wrk_req->wr_ele->wr.next = NULL;
}

static void init_reply_wr(struct heca_reply_work_request *rwr, u64 msg_addr,
                u32 lkey, int id)
{
        struct ib_sge *reply_sge;

        BUG_ON(!rwr);
        BUG_ON(!rwr->hwr_ele);

        reply_sge = &rwr->hwr_ele->sg;
        BUG_ON(!reply_sge);
        reply_sge->addr = msg_addr;
        reply_sge->length = sizeof(struct heca_message);
        reply_sge->lkey = lkey;

        rwr->hwr_ele->heca_dma.addr = msg_addr;
        rwr->hwr_ele->wr.next = NULL;
        rwr->hwr_ele->wr.num_sge = 1;
        rwr->hwr_ele->wr.send_flags = IB_SEND_SIGNALED;
        rwr->hwr_ele->wr.opcode = IB_WR_SEND;
        rwr->hwr_ele->wr.sg_list = (struct ib_sge *) &rwr->hwr_ele->sg;
        rwr->hwr_ele->wr.wr_id = id;
}

static void init_page_wr(struct heca_reply_work_request *rwr, u32 lkey, int id)
{
        rwr->page_sgl.addr = 0;
        rwr->page_sgl.length = PAGE_SIZE;
        rwr->page_sgl.lkey = lkey;

        rwr->wr.next = &rwr->hwr_ele->wr;
        rwr->wr.sg_list = (struct ib_sge *) &rwr->page_sgl;
        rwr->wr.send_flags = IB_SEND_SIGNALED;
        rwr->wr.opcode = IB_WR_RDMA_WRITE;
        rwr->wr.num_sge = 1;
        rwr->wr.wr_id = id;
}

static void init_tx_ele(struct tx_buffer_element *tx_ele,
                struct heca_connection *conn, int id)
{
        BUG_ON(!tx_ele);
        tx_ele->id = id;
        init_tx_wr(tx_ele, conn->mr->lkey, tx_ele->id);
        init_reply_wr(tx_ele->reply_work_req, tx_ele->heca_dma.addr,
                        conn->mr->lkey, tx_ele->id);
        BUG_ON(!conn->mr);
        init_page_wr(tx_ele->reply_work_req, conn->mr->lkey, tx_ele->id);
        tx_ele->hmsg_buffer->dest_id = conn->mr->rkey;
        tx_ele->hmsg_buffer->offset = tx_ele->id;
}

static void destroy_tx_buffer(struct heca_connection *conn)
{
        int i;
        struct tx_buffer_element *tx_buf = conn->tx_buffer.tx_buf;

        if (!tx_buf)
                return;
        cancel_work_sync(&conn->delayed_request_flush_work);

        for (i = 0; i < conn->tx_buffer.len; ++i) {
                if (tx_buf[i].heca_dma.addr) {
                        ib_dma_unmap_single(conn->cm_id->device,
                                        tx_buf[i].heca_dma.addr,
                                        tx_buf[i].heca_dma.size,
                                        tx_buf[i].heca_dma.dir);
                }
                kfree(tx_buf[i].hmsg_buffer);
                kfree(tx_buf[i].wrk_req->wr_ele);
                kfree(tx_buf[i].wrk_req);
        }

        kfree(tx_buf);
        conn->tx_buffer.tx_buf = 0;
}

static void destroy_rx_buffer(struct heca_connection *conn)
{
        int i;
        struct rx_buffer_element *rx = conn->rx_buffer.rx_buf;

        if (!rx)
                return;

        for (i = 0; i < conn->rx_buffer.len; ++i) {
                if (rx[i].heca_dma.addr) {
                        ib_dma_unmap_single(conn->cm_id->device,
                                        rx[i].heca_dma.addr,
                                        rx[i].heca_dma.size,
                                        rx[i].heca_dma.dir);
                }
                kfree(rx[i].hmsg_buffer);
                kfree(rx[i].recv_wrk_rq_ele);
        }
        kfree(rx);
        conn->rx_buffer.rx_buf = 0;
}

static int create_tx_buffer(struct heca_connection *conn)
{
        int i, ret = 0;
        struct tx_buffer_element *tx_buff_e;

        BUG_ON(!conn);
        BUG_ON(IS_ERR(conn->cm_id));
        BUG_ON(!conn->cm_id->device);
        might_sleep();

        conn->tx_buffer.len = get_nb_tx_buff_elements(conn);
        tx_buff_e = kzalloc((sizeof(struct tx_buffer_element) *
                                conn->tx_buffer.len), GFP_KERNEL);
        if (unlikely(!tx_buff_e)) {
                heca_printk(KERN_ERR "Can't allocate memory");
                return -ENOMEM;
        }
        conn->tx_buffer.tx_buf = tx_buff_e;

        for (i = 0; i < conn->tx_buffer.len; ++i) {
                tx_buff_e[i].hmsg_buffer = kzalloc(sizeof(struct heca_message),
                                GFP_KERNEL);
                if (!tx_buff_e[i].hmsg_buffer) {
                        heca_printk(KERN_ERR "Failed to allocate .heca_buf");
                        ret = -ENOMEM;
                        goto err;
                }

                tx_buff_e[i].heca_dma.dir = DMA_TO_DEVICE;
                tx_buff_e[i].heca_dma.size = sizeof(struct heca_message);
                tx_buff_e[i].heca_dma.addr = ib_dma_map_single(
                                conn->cm_id->device,
                                tx_buff_e[i].hmsg_buffer,
                                tx_buff_e[i].heca_dma.size,
                                tx_buff_e[i].heca_dma.dir);
                if (unlikely(!tx_buff_e[i].heca_dma.addr)) {
                        heca_printk(KERN_ERR "unable to create ib mapping");
                        ret = -EFAULT;
                        goto err;
                }

                tx_buff_e[i].wrk_req = kzalloc(sizeof(struct heca_msg_work_request),
                                GFP_KERNEL);
                if (!tx_buff_e[i].wrk_req) {
                        heca_printk(KERN_ERR "Failed to allocate wrk_req");
                        ret = -ENOMEM;
                        goto err;
                }

                tx_buff_e[i].wrk_req->wr_ele = kzalloc(sizeof(struct heca_work_request_element),
                                GFP_KERNEL);
                if (!tx_buff_e[i].wrk_req->wr_ele) {
                        heca_printk(KERN_ERR "Failed to allocate wrk_req->wr_ele");
                        ret = -ENOMEM;
                        goto err;
                }
                tx_buff_e[i].wrk_req->wr_ele->heca_dma = tx_buff_e[i].heca_dma;

                tx_buff_e[i].reply_work_req = kzalloc(sizeof(struct heca_reply_work_request),
                                GFP_KERNEL);
                if (!tx_buff_e[i].reply_work_req) {
                        heca_printk(KERN_ERR "Failed to allocate reply_work_req");
                        ret = -ENOMEM;
                        goto err;
                }

                tx_buff_e[i].reply_work_req->hwr_ele = kzalloc(
                                sizeof(struct heca_work_request_element),
                                GFP_KERNEL);
                if (!tx_buff_e[i].reply_work_req->hwr_ele) {
                        heca_printk(KERN_ERR "Failed to allocate reply_work_req->wr_ele");
                        ret = -ENOMEM;
                        goto err;
                }
                init_tx_ele(&tx_buff_e[i], conn, i);
        }
        goto done;

err:
        BUG_ON(!tx_buff_e);
        destroy_tx_buffer(conn);
        kfree(tx_buff_e);
done: return ret;
}

static void init_rx_ele(struct rx_buffer_element *rx_ele,
                struct heca_connection *conn)
{
        struct heca_recv_work_req_element *rwr = rx_ele->recv_wrk_rq_ele;
        struct ib_sge *recv_sge = &rwr->recv_sgl;

        recv_sge->addr = rx_ele->heca_dma.addr;
        recv_sge->length = rx_ele->heca_dma.size;
        recv_sge->lkey = conn->mr->lkey;

        rwr->sq_wr.next = NULL;
        rwr->sq_wr.num_sge = 1;
        rwr->sq_wr.sg_list = &rwr->recv_sgl;
        rwr->sq_wr.wr_id = rx_ele->id;
}

static int create_rx_buffer(struct heca_connection *conn)
{
        int i;
        int undo = 0;
        struct rx_buffer_element *rx;

        conn->rx_buffer.len = get_nb_rx_buff_elements(conn);
        rx = kzalloc((sizeof(struct rx_buffer_element) * conn->rx_buffer.len),
                        GFP_KERNEL);
        if (!rx)
                goto err_buf;
        conn->rx_buffer.rx_buf = rx;

        for (i = 0; i < conn->rx_buffer.len; ++i) {
                rx[i].hmsg_buffer = kzalloc(sizeof(struct heca_message),
                                GFP_KERNEL);
                if (!rx[i].hmsg_buffer)
                        goto err1;

                rx[i].heca_dma.size = sizeof(struct heca_message);
                rx[i].heca_dma.dir = DMA_BIDIRECTIONAL;
                rx[i].heca_dma.addr = ib_dma_map_single(conn->cm_id->device,
                                rx[i].hmsg_buffer,
                                rx[i].heca_dma.size,
                                rx[i].heca_dma.dir);
                if (!rx[i].heca_dma.addr)
                        goto err2;

                rx[i].recv_wrk_rq_ele = kzalloc(sizeof(struct heca_recv_work_req_element),
                                GFP_KERNEL);
                if (!rx[i].recv_wrk_rq_ele)
                        goto err3;

                rx[i].id = i;
                init_rx_ele(&rx[i], conn);
        }

        return 0;

err3:
        ib_dma_unmap_single(conn->cm_id->device, rx[i].heca_dma.addr,
                        rx[i].heca_dma.size, rx[i].heca_dma.dir);
err2:
        kfree(rx[i].hmsg_buffer);
err1:
        for (undo = 0; undo < i; ++undo) {
                ib_dma_unmap_single(conn->cm_id->device, rx[undo].heca_dma.addr,
                                rx[undo].heca_dma.size, rx[undo].heca_dma.dir);
                kfree(rx[undo].hmsg_buffer);
                kfree(rx[undo].recv_wrk_rq_ele);
        }
        kfree(rx);
        conn->rx_buffer.rx_buf = 0;
err_buf:
        heca_printk(KERN_ERR "RX BUFFER NOT CREATED");
        return -1;
}

static void format_rdma_info(struct heca_connection *conn)
{
        conn->rid.send_buf->node_ip = htonl(conn->htm->node_ip);
        conn->rid.send_buf->buf_rx_addr = htonll((u64) conn->rx_buffer.rx_buf);
        conn->rid.send_buf->buf_msg_addr = htonll((u64) conn->tx_buffer.tx_buf);
        conn->rid.send_buf->rx_buf_size = htonl(conn->rx_buffer.len);
        conn->rid.send_buf->rkey_msg = htonl(conn->mr->rkey);
        conn->rid.send_buf->rkey_rx = htonl(conn->mr->rkey);
        conn->rid.send_buf->flag = RDMA_INFO_CL;
}

static int create_rdma_info(struct heca_connection *conn)
{
        int size = sizeof(struct heca_rdma_info);
        struct rdma_info_data *rid = &conn->rid;

        rid->send_buf = kzalloc(size, GFP_KERNEL);
        if (unlikely(!rid->send_buf))
                goto send_mem_err;

        rid->send_dma.size = size;
        rid->send_dma.dir = DMA_TO_DEVICE;
        rid->send_dma.addr = ib_dma_map_single(conn->cm_id->device,
                        rid->send_buf,
                        rid->send_dma.size,
                        rid->send_dma.dir);
        if (unlikely(!rid->send_dma.addr))
                goto send_info_err;

        rid->recv_buf = kzalloc(size, GFP_KERNEL);
        if (unlikely(!rid->recv_buf))
                goto recv_mem_err;

        rid->recv_dma.size = size;
        rid->recv_dma.dir = DMA_FROM_DEVICE;
        rid->recv_dma.addr = ib_dma_map_single(conn->cm_id->device,
                        rid->recv_buf,
                        rid->recv_dma.size,
                        rid->recv_dma.dir);
        if (unlikely(!rid->send_dma.addr))
                goto recv_info_err;

        rid->remote_info = kzalloc(size, GFP_KERNEL);
        if (unlikely(!rid->remote_info))
                goto remote_info_buffer_err;

        rid->remote_info->flag = RDMA_INFO_CL;
        rid->exchanged = 2;
        format_rdma_info(conn);
        return 0;

remote_info_buffer_err:
        heca_printk(KERN_ERR "ERROR : NO REMOTE INFO BUFFER");
        ib_dma_unmap_single(conn->cm_id->device, rid->recv_dma.addr,
                        rid->recv_dma.size, rid->recv_dma.dir);

recv_info_err:
        heca_printk(KERN_ERR "ERROR : NO RECV INFO BUFFER");
        kfree(rid->recv_buf);

recv_mem_err:
        heca_printk(KERN_ERR "no memory allocated for the reception buffer");
        ib_dma_unmap_single(conn->cm_id->device, rid->send_dma.addr,
                        rid->send_dma.size, rid->send_dma.dir);

send_info_err:
        heca_printk(KERN_ERR "ERROR : NO SEND INFO BUFFER");
        kfree(rid->send_buf);

send_mem_err:
        heca_printk(KERN_ERR "no memory allocated for the sending buffer");
        return -1;
}

static int init_tx_lists(struct heca_connection *conn)
{
        int i;
        struct tx_buffer *tx = &conn->tx_buffer;
        int max_tx_send = conn->tx_buffer.len / 3;

        tx->request_queue_sz = 0;
        init_llist_head(&tx->request_queue);
        init_llist_head(&tx->tx_free_elements_list);
        init_llist_head(&tx->tx_free_elements_list_reply);
        spin_lock_init(&tx->tx_free_elements_list_lock);
        spin_lock_init(&tx->tx_free_elements_list_reply_lock);
        INIT_LIST_HEAD(&tx->ordered_request_queue);
        mutex_init(&tx->flush_mutex);
        INIT_WORK(&conn->delayed_request_flush_work,
                        delayed_request_flush_work_fn);

        for (i = 0; i < max_tx_send; ++i)
                release_tx_element(conn, &tx->tx_buf[i]);

        for (; i < conn->tx_buffer.len; ++i)
                release_tx_element_reply(conn, &tx->tx_buf[i]);

        return 0;
}

static int setup_connection(struct heca_connection *conn, int type)
{
        int ret = 0, err = 0;
        struct rdma_conn_param conn_param;

        conn->pd = ib_alloc_pd(conn->cm_id->device);
        if (!conn->pd)
                goto err1;
        conn->mr = ib_get_dma_mr(conn->pd,
                        IB_ACCESS_LOCAL_WRITE |
                        IB_ACCESS_REMOTE_READ |
                        IB_ACCESS_REMOTE_WRITE);
        if (!conn->mr)
                goto err2;
        if (setup_qp_attr(conn))
                goto err3;
        if (setup_qp(conn))
                goto err4;
        if (create_tx_buffer(conn))
                goto err5;
        if (create_rx_buffer(conn))
                goto err6;
        if (heca_init_page_pool(conn))
                goto err7;
        if (create_rdma_info(conn))
                goto err8;
        if (init_tx_lists(conn))
                goto err9;

        if (type) {
                heca_recv_info(conn);

                memset(&conn_param, 0, sizeof(struct rdma_conn_param));
                conn_param.responder_resources = 1;
                conn_param.initiator_depth = 1;

                if (rdma_accept(conn->cm_id, &conn_param))
                        goto err10;
        }

        return ret;

err10: err++;
err9: err++;
err8: err++;
err7: err++;
err6: err++;
err5: err++;
err4: err++;
err3: err++;
err2: err++;
err1: err++;
      heca_printk(KERN_ERR "Could not setup connection: error %d", err);
      return err;
}

static int client_event_handler(struct rdma_cm_id *id, struct rdma_cm_event *ev)
{
        int ret = 0, err = 0;
        struct heca_connection *conn = id->context;

        switch (ev->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
                ret = rdma_resolve_route(id, 2000);
                if (ret)
                        goto err1;
                break;

        case RDMA_CM_EVENT_ROUTE_RESOLVED:
                ret = setup_connection(conn, 0);
                if (ret)
                        goto err2;

                ret = connect_client(id);
                if (ret) {
                        complete(&conn->completion);
                        goto err3;
                }

                atomic_set(&conn->alive, 1);
                break;

        case RDMA_CM_EVENT_ESTABLISHED:
                ret = heca_recv_info(conn);
                if (ret)
                        goto err4;

                ret = heca_send_info(conn);
                if (ret < 0)
                        goto err5;

                break;

        case RDMA_CM_EVENT_DISCONNECTED:
                if (likely(atomic_cmpxchg(&conn->alive, 1, -1) == 1))
                        schedule_destroy_conns();
                break;

        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
                heca_printk(KERN_ERR, "could not connect, %d", ev->event);
                complete(&conn->completion);
                break;

        case RDMA_CM_EVENT_DEVICE_REMOVAL:
        case RDMA_CM_EVENT_ADDR_CHANGE:
                heca_printk(KERN_ERR "unexpected event: %d", ev->event);
                ret = rdma_disconnect(id);
                if (unlikely(ret))
                        goto disconnect_err;
                break;

        default:
                heca_printk(KERN_ERR "no special handling: %d", ev->event);
                break;
        }

        return ret;

err5:
        err++;
err4:
        err++;
err3:
        err++;
err2:
        err++;
err1:
        err++;
        ret = rdma_disconnect(id);
        heca_printk(KERN_ERR, "fatal error %d", err);
        if (unlikely(ret))
                goto disconnect_err;

        return ret;

disconnect_err:
        heca_printk(KERN_ERR, "disconection failed");
        return ret;
}

int server_event_handler(struct rdma_cm_id *id, struct rdma_cm_event *ev)
{
        int ret = 0;
        struct heca_connection *conn = 0;
        struct heca_transport_manager *htm;

        switch (ev->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
                break;

        case RDMA_CM_EVENT_CONNECT_REQUEST:
                conn = vzalloc(sizeof(struct heca_connection));
                if (!conn)
                        goto out;

                init_completion(&conn->completion);
                htm = id->context;
                conn->htm = htm;
                conn->cm_id = id;
                id->context = conn;

                ret = setup_connection(conn, 1);
                if (ret) {
                        heca_printk(KERN_ERR "setup_connection failed: %d",
                                        ret);
                        goto err;
                }


                atomic_set(&conn->alive, 1);
                break;

        case RDMA_CM_EVENT_ESTABLISHED:
                break;

        case RDMA_CM_EVENT_DISCONNECTED:
                conn = id->context;
                if (likely(atomic_cmpxchg(&conn->alive, 1, -1) == 1))
                        schedule_destroy_conns();
                break;

        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_ADDR_CHANGE:
                heca_printk(KERN_ERR "unexpected event: %d", ev->event);

                ret = rdma_disconnect(id);
                if (unlikely(ret))
                        goto disconnect_err;
                break;

        default:
                heca_printk(KERN_ERR "no special handling: %d", ev->event);
                break;
        }

out:
        return ret;

disconnect_err:
        heca_printk(KERN_ERR "disconnect failed");
err:
        vfree(conn);
        conn = 0;
        return ret;
}

inline void heca_msg_cpy(struct heca_message *dst, struct heca_message *orig)
{
        dst->hspace_id = orig->hspace_id;
        dst->src_id = orig->src_id;
        dst->dest_id = orig->dest_id;
        dst->type = orig->type;
        dst->offset = orig->offset;
        dst->req_addr = orig->req_addr;
        dst->dst_addr = orig->dst_addr;
        dst->rkey = orig->rkey;
}

static void free_rdma_info(struct heca_connection *conn)
{
        if (conn->rid.send_dma.addr) {
                ib_dma_unmap_single(conn->cm_id->device,
                                conn->rid.send_dma.addr,
                                conn->rid.send_dma.size,
                                conn->rid.send_dma.dir);
                kfree(conn->rid.send_buf);
        }

        if (conn->rid.recv_dma.addr) {
                ib_dma_unmap_single(conn->cm_id->device,
                                conn->rid.recv_dma.addr,
                                conn->rid.recv_dma.size,
                                conn->rid.recv_dma.dir);
                kfree(conn->rid.recv_buf);
        }

        if (conn->rid.remote_info) {
                kfree(conn->rid.remote_info);
        }

        memset(&conn->rid, 0, sizeof(struct rdma_info_data));
}

void release_tx_element(struct heca_connection *conn,
                struct tx_buffer_element *tx_e)
{
        struct tx_buffer *tx = &conn->tx_buffer;
        atomic_set(&tx_e->used, 0);
        atomic_set(&tx_e->released, 0);
        llist_add(&tx_e->tx_buf_ele_ptr, &tx->tx_free_elements_list);
}

void release_tx_element_reply(struct heca_connection *conn,
                struct tx_buffer_element *tx_e)
{
        struct tx_buffer *tx = &conn->tx_buffer;
        atomic_set(&tx_e->used, 0);
        atomic_set(&tx_e->released, 0);
        llist_add(&tx_e->tx_buf_ele_ptr, &tx->tx_free_elements_list_reply);
}

void try_release_tx_element(struct heca_connection *conn,
                struct tx_buffer_element *tx_e)
{
        if (atomic_add_return(1, &tx_e->released) == 2)
                release_tx_element(conn, tx_e);
}

static int create_connection(struct heca_transport_manager *htm,
                unsigned long ip,
                unsigned short port)
{
        struct rdma_conn_param param;
        struct heca_connection *conn;

        conn = vzalloc(sizeof(struct heca_connection));
        if (unlikely(!conn))
                goto err;

        memset(&param, 0, sizeof(struct rdma_conn_param));
        param.responder_resources = 1;
        param.initiator_depth = 1;
        param.retry_count = 10;

        conn->local.sin_family = AF_INET;
        conn->local.sin_addr.s_addr = htm->sin.sin_addr.s_addr;
        conn->local.sin_port = 0;

        conn->remote.sin_family = AF_INET;
        conn->remote.sin_addr.s_addr = ip;
        conn->remote.sin_port = port;

        init_completion(&conn->completion);
        conn->remote_node_ip = ip;
        insert_rb_conn(conn);

        conn->htm = htm;
        conn->cm_id = rdma_create_id(client_event_handler, conn, RDMA_PS_TCP,
                        IB_QPT_RC);
        if (IS_ERR(conn->cm_id))
                goto err1;


        return rdma_resolve_addr(conn->cm_id, (struct sockaddr *) &conn->local,
                        (struct sockaddr*) &conn->remote, 2000);

err1: erase_rb_conn(conn);
      vfree(conn);
err: return -1;
}

int connect_hproc(struct heca_space *hspace, struct heca_process *hproc,
                unsigned long ip_addr, unsigned short port)
{
        int r = 0;
        struct heca_connection *conn;
        struct heca_module_state *heca_state = get_heca_module_state();

        conn = search_rb_conn(ip_addr);
        if (conn) {
                heca_printk(KERN_ERR "has existing connection to %pI4",
                                &ip_addr);
                goto done;
        }

        r = create_connection(heca_state->htm, ip_addr, port);
        if (r) {
                heca_printk(KERN_ERR "create_connection failed %d", r);
                goto failed;
        }

        might_sleep();
        conn = search_rb_conn(ip_addr);
        if (!conn) {
                heca_printk(KERN_ERR "conneciton does not exist", r);
                r = -ENOLINK;
                goto failed;
        }

        wait_for_completion(&conn->completion);
        if (!atomic_read(&conn->alive)) {
                heca_printk(KERN_ERR "conneciton is not alive ... aborting");
                r = -ENOLINK;
                goto failed;
        }

done:
        hproc->connection = conn;

failed:
        return r;
}

struct tx_buffer_element *try_get_next_empty_tx_ele(
                struct heca_connection *conn,
                int require_empty_list)
{
        struct tx_buffer_element *tx_e = NULL;
        struct llist_node *llnode = NULL;

        spin_lock(&conn->tx_buffer.tx_free_elements_list_lock);
        if (!require_empty_list || heca_request_queue_empty(conn))
                llnode = llist_del_first(&conn->tx_buffer.tx_free_elements_list);
        spin_unlock(&conn->tx_buffer.tx_free_elements_list_lock);

        if (llnode) {
                tx_e = container_of(llnode, struct tx_buffer_element,
                                tx_buf_ele_ptr);
                atomic_set(&tx_e->used, 1);
        }
        return tx_e;
}

struct tx_buffer_element *try_get_next_empty_tx_reply_ele(
                struct heca_connection *conn)
{
        struct tx_buffer_element *tx_e = NULL;
        struct llist_node *llnode;

        spin_lock(&conn->tx_buffer.tx_free_elements_list_reply_lock);
        llnode = llist_del_first(&conn->tx_buffer.tx_free_elements_list_reply);
        spin_unlock(&conn->tx_buffer.tx_free_elements_list_reply_lock);

        if (llnode) {
                tx_e = container_of(llnode, struct tx_buffer_element,
                                tx_buf_ele_ptr);
                atomic_set(&tx_e->used, 1);
        }
        return tx_e;
}

static void remove_hprocs_for_conn(struct heca_connection *conn)
{
        int local_left;
        struct heca_space *hspace, *tmp_hspace;
        struct heca_process *hproc, *tmp_hproc;

        list_for_each_entry_safe (hspace, tmp_hspace,
                        &get_heca_module_state()->hspaces_list, hspace_ptr) {
                local_left=0;
                list_for_each_entry_safe (hproc, tmp_hproc,
                                &hspace->hprocs_list, hproc_ptr) {
                        if (hproc->connection == conn)
                                teardown_hproc(hspace, hproc);
                        else
                                local_left += is_hproc_local(hproc);
                }
                if(!local_left)
                        teardown_hspace(hspace);
        }
}

int destroy_connection(struct heca_connection *conn)
{
        int ret = 0;

        remove_hprocs_for_conn(conn);

        if (likely(conn->cm_id)) {
                synchronize_rcu();
                cancel_work_sync(&conn->recv_work);
                cancel_work_sync(&conn->send_work);

                if (likely(conn->cm_id->qp))
                        ret |= ib_destroy_qp(conn->cm_id->qp);

                if (likely(conn->qp_attr.send_cq))
                        ret |= ib_destroy_cq(conn->qp_attr.send_cq);

                if (likely(conn->qp_attr.recv_cq))
                        ret |= ib_destroy_cq(conn->qp_attr.recv_cq);

                if (likely(conn->mr))
                        ret |= ib_dereg_mr(conn->mr);

                if (likely(conn->pd))
                        ret |= ib_dealloc_pd(conn->pd);

                destroy_rx_buffer(conn);
                destroy_tx_buffer(conn);
                free_rdma_info(conn);
                rdma_destroy_id(conn->cm_id);
        }

        heca_destroy_page_pool(conn);

        erase_rb_conn(conn);
        vfree(conn);

        return ret;
}

/*
 * Can either fail with:
 *  > -ENOMEM - in which case we sleep and let ib work thread finish.
 *  > -ENOTCONN - meaning the connection has been disrupted; we handle this
 *                in destroy_connection.
 *  > -EINVAL (or other) - we sent wrong output, shouldn't happen.
 *
 */
int tx_heca_send(struct heca_connection *conn, struct tx_buffer_element *tx_e)
{
        int ret;
        int type = tx_e->hmsg_buffer->type;

retry:
        switch (type) {
        case MSG_REQ_PAGE:
        case MSG_REQ_PUSHED_PAGE:
        case MSG_REQ_READ:
        case MSG_REQ_CLAIM:
        case MSG_REQ_CLAIM_TRY:
        case MSG_REQ_QUERY:
        case MSG_REQ_PUSH:
        case MSG_RES_PAGE_REDIRECT:
        case MSG_RES_PAGE_FAIL:
        case MSG_RES_HPROC_FAIL:
        case MSG_RES_ACK:
        case MSG_RES_ACK_FAIL:
        case MSG_RES_QUERY:
                ret = ib_post_send(conn->cm_id->qp, &tx_e->wrk_req->wr_ele->wr,
                                &tx_e->wrk_req->wr_ele->bad_wr);
                break;
        case MSG_RES_PAGE:
                ret = ib_post_send(conn->cm_id->qp, &tx_e->reply_work_req->wr,
                                &tx_e->reply_work_req->hwr_ele->bad_wr);
                break;
        default:
                BUG();
        }

        /*
         * we have no other choice but to postpone and try again (no memory for a
         * queued request). this should happen mainly with softiwarp.
         */
        if (unlikely(ret == -ENOMEM)) {
                cond_resched();
                goto retry;
        }

        if (ret && ret != -ENOTCONN) {
                heca_printk("ib_post_send() returned %d on type 0x%x",
                                ret, type);
                BUG();
        }
        return ret;
}

