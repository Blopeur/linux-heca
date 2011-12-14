/*
 * dsm_def.h
 *
 *  Created on: 7 Jul 2011
 *      Author: john
 */

#ifndef DSM_DEF_H_
#define DSM_DEF_H_

#include <rdma/rdma_cm.h>
#include <rdma/ib_verbs.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/rwlock_types.h>
#include <linux/types.h>
#include <linux/time.h>
#include <linux/gfp.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <asm/atomic.h>

#include <dsm/dsm_stats.h>

//#define ULONG_MAX       0xFFFFFFFFFFFFFFFF

#define RDMA_PAGE_SIZE PAGE_SIZE

#define MAX_CAP_SCQ 256
#define MAX_CAP_RCQ 1024

#define TX_BUF_ELEMENTS_NUM MAX_CAP_SCQ
#define RX_BUF_ELEMENTS_NUM MAX_CAP_RCQ

#define PAGE_POOL_SIZE (MAX_CAP_SCQ + MAX_CAP_RCQ)*2

/**
 * RDMA_INFO
 */
#define RDMA_INFO_CL 4
#define RDMA_INFO_SV 3
#define RDMA_INFO_READY_CL 2
#define RDMA_INFO_READY_SV 1
#define RDMA_INFO_NULL 0

/**
 * DSM_MESSAGE
 */

#define REQUEST_PAGE                    0x0000 // We Request a page
#define REQUEST_PAGE_PULL               0x0001 // We Request a page pull
#define PAGE_REQUEST_REPLY              0x0002 // We Reply to a page request
#define PAGE_REQUEST_REDIRECT           0x0004 // We don't have the page  but we know where it is , we redirect
#define PAGE_INFO_UPDATE                0x0008 // We send an update of the page location
#define TRY_REQUEST_PAGE                0x0010 // We try to pull the page
#define TRY_REQUEST_PAGE_FAIL           0x0020 // We try to get the page failed
#define DSM_MSG_ERR                     0x8000 // ERROR
/*
 * DSM DATA structure
 */

struct dsm_vm_id {
    u16 dsm_id;
    u8 svm_id;

};

static inline u32 dsm_vm_id_to_u32(struct dsm_vm_id *id) {
    u32 val = id->dsm_id;

    val = val << 8;

    val |= id->svm_id;

    return val;

}

static inline u16 u32_to_dsm_id(u32 val) {
    return val >> 8;

}

static inline u8 u32_to_vm_id(u32 val) {
    return val & 0xFF;

}

struct dsm {
    u16 dsm_id;

    struct list_head svm_ls;
    struct list_head ls;
};

struct dsm_kobjects {
    struct kobject * dsm_kobject;
    struct kobject * memory_kobject;
    struct kobject * rdma_kobject;
};

struct rcm {
    int node_ip;

    struct rdma_cm_id *cm_id;
    struct ib_device *dev;
    struct ib_pd *pd;
    struct ib_mr *mr;

    struct ib_cq *listen_cq;

    spinlock_t rcm_lock;

    spinlock_t route_lock;

    struct rb_root root_conn;
    struct rb_root root_route;

    struct sockaddr_in sin;

    struct list_head dsm_ls;
    struct rb_root red_page_root;
    struct dsm_kobjects dsm_kobjects;

    struct workqueue_struct * dsm_wq;

};
struct rdma_info_data {

    void *send_mem;
    void *recv_mem;

    struct rdma_info *send_info;
    struct rdma_info *recv_info;
    struct rdma_info *remote_info;

    struct ib_sge recv_sge;
    struct ib_recv_wr recv_wr;
    struct ib_recv_wr *recv_bad_wr;

    struct ib_sge send_sge;
    struct ib_send_wr send_wr;
    struct ib_send_wr *send_bad_wr;
    int exchanged;
};

struct page_pool_ele {

    void * page_buf;
    struct page * mem_page;
    struct list_head page_ptr;

};

struct page_pool {

    int nb_full_element;

    struct list_head page_pool_list;
    struct list_head page_empty_pool_list;
    struct list_head page_release_list;
    struct list_head page_recycle_list;

    spinlock_t page_pool_list_lock;
    spinlock_t page_pool_empty_list_lock;
    spinlock_t page_release_lock;
    spinlock_t page_recycle_lock;

    struct work_struct page_release_work;

};

struct rx_buffer {
    struct rx_buf_ele * rx_buf;
};

struct tx_buffer {
    struct tx_buf_ele * tx_buf;

    struct list_head tx_free_elements_list;
    struct list_head tx_free_elements_list_reply;
    struct list_head request_queue;
    spinlock_t request_queue_lock;
    spinlock_t tx_free_elements_list_lock;
    spinlock_t tx_free_elements_list_reply_lock;

    struct completion completion_free_tx_element;

};

struct conn_element {
    struct rcm *rcm;

    int remote_node_ip;
    struct rdma_info_data rid;

    struct ib_mr *mr;
    struct ib_pd *pd;
    struct rdma_cm_id *cm_id;
    struct ib_cq *send_cq;
    struct ib_cq *recv_cq;

    struct work_struct send_work;
    struct work_struct recv_work;

    struct rx_buffer rx_buffer;
    struct tx_buffer tx_buffer;

    struct page_pool page_pool;
    struct rb_node rb_node;

    struct con_element_stats stats;

};

struct rdma_info {

    u8 flag;
    u32 node_ip;
    u64 buf_msg_addr;
    u32 rkey_msg;
    u64 buf_rx_addr;
    u32 rkey_rx;
    u32 rx_buf_size;

};

struct dsm_message {

    u32 offset;
    u32 dest;
    u32 src;
    u64 req_addr;
    u64 dst_addr;
    u32 rkey;
    u16 type;

};

/*
 * region represents local area of VM memory.
 */
struct mem_region {
    unsigned long addr;
    unsigned long sz;
    struct subvirtual_machine *svm;

    struct list_head ls;
    struct rcu_head rcu;

};

struct private_data {

    struct rb_root root_swap;

    struct mm_struct *mm;

    unsigned long offset;
    struct subvirtual_machine *svm;

    struct list_head head;

};

struct subvirtual_machine {
    struct conn_element *ele;
    struct dsm_vm_id id;
    struct list_head mr_ls;
    struct list_head ls;

    struct private_data *priv;
    struct rcu_head rcu_head;
    struct rb_node rb_node;

};

struct work_request_ele {
    struct conn_element *ele;

    struct ib_send_wr wr;
    struct ib_sge sg;
    struct ib_send_wr *bad_wr;

    struct dsm_message *dsm_msg;

};

struct msg_work_request {
    struct work_request_ele *wr_ele;
    struct page_pool_ele * dst_addr;

};

struct recv_work_req_ele {
    struct conn_element * ele;

    struct ib_recv_wr sq_wr;
    struct ib_recv_wr *bad_wr;
    struct ib_sge recv_sgl;

};

struct reply_work_request {
    //The one for sending back a message
    struct work_request_ele *wr_ele;

    //The one for sending the page
    struct ib_send_wr wr;
    struct ib_send_wr *bad_wr;
    struct page * mem_page;
    void *page_buf;
    struct ib_sge page_sgl;

};

struct tx_callback {

    void (*func)(struct tx_buf_ele *);
};

struct tx_buf_ele {
    int id;

    void *mem;
    struct dsm_message *dsm_msg;
    struct msg_work_request *wrk_req;
    struct reply_work_request *reply_work_req;
    struct list_head tx_buf_ele_ptr;

    struct tx_callback callback;

    struct tx_dsm_stats stats;

};

struct rx_buf_ele {
    int id;

    void *mem;
    struct dsm_message *dsm_msg;
    //The one for catching the request in the first place
    struct recv_work_req_ele *recv_wrk_rq_ele;

};

struct dsm_request {
    u16 type;
    struct page * page;
    struct subvirtual_machine *svm;
    struct subvirtual_machine *fault_svm;
    uint64_t addr;
    void(*func)(struct tx_buf_ele *);
    struct dsm_message dsm_msg;
    struct list_head queue;
};

/*
 * CTL info
 */
#define DSM_IO                          0xFF
#define DSM_SVM                         _IOW(DSM_IO, 0xA0, struct svm_data)
#define DSM_CONNECT                     _IOW(DSM_IO, 0xA1, struct svm_data)
#define DSM_UNMAP_RANGE                 _IOW(DSM_IO, 0xA2, struct unmap_data)
#define DSM_MR                          _IOW(DSM_IO, 0xA3, struct mr_data)
#define PAGE_SWAP                       _IOW(DSM_IO, 0xA4, struct dsm_message)
#define UNMAP_PAGE                      _IOW(DSM_IO, 0xA5, struct unmap_data)
#define DSM_GET_STAT                    _IOW(DSM_IO, 0xA6, struct svm_data)
#define DSM_GEN_STAT                    _IOW(DSM_IO, 0xA7, struct svm_data)
#define DSM_TRY_PUSH_BACK_PAGE          _IOW(DSM_IO, 0xA8, struct unmap_data)

struct svm_data {
    int dsm_id;
    int svm_id;
    unsigned long offset;
    char *ip;
    int port;

};

struct mr_data {
    int dsm_id;
    int svm_id;
    unsigned long start_addr;
    unsigned long size;

};

struct unmap_data {
    unsigned long addr;
    size_t sz;
    struct dsm_vm_id id;
};

#endif /* DSM_DEF_H_ */
