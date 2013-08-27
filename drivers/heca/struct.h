/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 */

#ifndef HECA_STRUCT_H_
#define HECA_STRUCT_H_

#include "rdma.h"
#include "transport_manager.h"

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
#include <linux/swap.h>
#include <linux/swapops.h>
#include <asm/atomic.h>
#include <linux/llist.h>
#include <linux/heca.h>

#define RDMA_PAGE_SIZE      PAGE_SIZE

#define MAX_HPROCS_PER_PAGE   2

#define GUP_DELAY           HZ*5    /* 5 second */
#define REQUEST_FLUSH_DELAY 50      /* 50 usec delay */

/*
 * HECA Messages
 */
#define MSG_REQ_PAGE                (1 << 0)
#define MSG_REQ_PAGE_TRY            (1 << 1)
#define MSG_REQ_READ                (1 << 2)
#define MSG_REQ_PAGE_PULL           (1 << 3)
#define MSG_REQ_CLAIM               (1 << 4)
#define MSG_REQ_CLAIM_TRY           (1 << 5)
#define MSG_REQ_QUERY               (1 << 6)
#define MSG_RES_PAGE                (1 << 7)
#define MSG_RES_PAGE_REDIRECT       (1 << 8)
#define MSG_RES_PAGE_FAIL           (1 << 9)
#define MSG_RES_HPROC_FAIL            (1 << 10)
#define MSG_RES_ACK                 (1 << 11)
#define MSG_RES_ACK_FAIL            (1 << 12)
#define MSG_RES_QUERY               (1 << 13)

/*
 * MEMORY REGION FLAGS
 */
#define MR_LOCAL                (1 << 0)
#define MR_COPY_ON_ACCESS       (1 << 1)
#define MR_SHARED               (1 << 2)

/*
 * Heca Space Page Pool Size
 * 1000 * 4 KB ~= 4 MB
 */
#define HSPACE_PAGE_POOL_SZ 1000

/*
 * BITOPS to identify page in flux
 */
#define HECA_INFLIGHT            0x04
#define HECA_INFLIGHT_BITPOS     0x02
#define HECA_PUSHING             0x08
#define HECA_PUSHING_BITPOS      0x03

/*
 * Useful macro for parsing heca processes
 */
#define for_each_valid_hproc(hprocs, i) \
        for (i = 0; i < (hprocs).num; i++) \
if (likely((hprocs).ids[i]))


/*
 * HECA DATA structure
 */
struct heca_space {
        u32 hspace_id;

        struct radix_tree_root hprocs_tree_root;
        struct radix_tree_root hprocs_mm_tree_root;

        struct mutex hspace_mutex;
        struct list_head hprocs_list;

        struct list_head hspace_ptr;

        struct kobject hspace_kobject;
        int nb_local_hprocs;
};

struct heca_space_kobjects {
        struct kobject *hspace_glob_kobject;
        struct kobject *rdma_kobject;
        struct kobject *domains_kobject;
};

struct heca_page_pool_element {
        void *page_buf;
        struct page *mem_page;
        struct llist_node llnode;
};

struct heca_space_page_pool {
        int cpu;
        struct heca_page_pool_element *hspace_page_pool[HSPACE_PAGE_POOL_SZ];
        int head;
        struct heca_connection *connection;
        struct work_struct work;
};

struct heca_message {
        /* hdr */
        u16 type;
        u64 req_addr;
        u64 dst_addr;
        u32 hspace_id;
        u32 mr_id;
        u32 src_id;
        u32 dest_id;
        u32 offset;
        u32 rkey;
};

struct heca_memory_region {
        unsigned long addr;
        unsigned long sz;
        u32 descriptor;
        u32 hmr_id;
        u32 flags;
        struct rb_node rb_node;
        struct kobject hmr_kobject;
};

struct heca_process {
        u32 hproc_id;
        int is_local;
        struct heca_space *hspace;
        struct heca_connection *connection;
        pid_t pid;
        struct mm_struct *mm;
        u32 descriptor;
        struct list_head hproc_ptr;

        struct radix_tree_root page_cache;
        spinlock_t page_cache_spinlock;

        struct radix_tree_root page_readers;
        spinlock_t page_readers_spinlock;

        struct radix_tree_root page_maintainers;
        spinlock_t page_maintainers_spinlock;

        struct radix_tree_root hmr_id_tree_root;
        struct rb_root hmr_tree_root;
        struct heca_memory_region *hmr_cache;
        seqlock_t hmr_seq_lock;

        struct rb_root push_cache;
        seqlock_t push_cache_lock;

        struct kobject hproc_kobject;

        struct llist_head delayed_gup;
        struct delayed_work delayed_gup_work;

        struct llist_head deferred_gups;
        struct work_struct deferred_gup_work;

        atomic_t refs;
};

struct tx_callback {
        int (*func)(struct tx_buffer_element *);
};

struct tx_buffer_element {
        int id;
        struct heca_message *hmsg_buffer;
        struct map_dma heca_dma;
        struct heca_msg_work_request *wrk_req;
        struct heca_reply_work_request *reply_work_req;
        struct llist_node tx_buf_ele_ptr;

        struct tx_callback callback;
        atomic_t used;
        atomic_t released;
};

struct rx_buffer_element {
        int id;
        struct heca_message *hmsg_buffer;
        struct map_dma heca_dma;
        //The one for catching the request in the first place
        struct heca_recv_work_req_element *recv_wrk_rq_ele;
};

struct heca_request {
        u16 type;
        u32 hspace_id;
        u32 local_hproc_id;
        u32 remote_hproc_id;
        u32 hmr_id;
        struct page *page;
        struct heca_page_pool_element *ppe;
        uint64_t addr;
        int (*func)(struct tx_buffer_element *);
        struct heca_message hmsg;
        struct heca_page_cache *hpc;
        int response;
        int need_ppe;

        struct llist_node lnode;
        struct list_head ordered_list;
};

struct heca_deferred_gup {
        struct heca_message hmsg;
        struct heca_process *remote_hproc;
        struct heca_connection *connection_origin;
        struct heca_memory_region *hmr;
        struct llist_node lnode;
};


struct heca_module_state {
        struct heca_connections_manager *hcm;
        struct mutex heca_state_mutex;
        spinlock_t radix_lock;
        struct radix_tree_root hspaces_tree_root;
        struct radix_tree_root mm_tree_root;
        struct list_head hspaces_list;

        struct heca_space_kobjects hspaces_kobjects;
        struct workqueue_struct * heca_rx_wq;
        struct workqueue_struct * heca_tx_wq;
};

struct heca_process_list {
        u32 hspace_id;
        u32 *ids;
        int num;
};

struct heca_page_cache {
        struct heca_process *hproc;
        unsigned long addr;
        u32 tag; /* used to diff between pull ops, and to store dsc for push ops */

        struct page *pages[MAX_HPROCS_PER_PAGE];
        struct heca_process_list hprocs;
        /* memory barrier are ok with these atomic */
        atomic_t found;
        atomic_t nproc;
        int released;
        unsigned long bitmap;
        u32 redirect_hproc_id;

        struct rb_node rb_node;
};

struct heca_delayed_fault {
        unsigned long addr;
        struct llist_node node;
};

struct heca_pte_data {
        struct vm_area_struct *vma;
        pgd_t *pgd;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *pte;
};

struct heca_swp_data {
        struct heca_space *hspace;
        struct heca_process_list hprocs;
        u32 flags;
};

struct heca_page_reader {
        u32 hproc_id;
        struct heca_page_reader *next;
};

void heca_init_descriptors(void);
void heca_destroy_descriptors(void);
u32 heca_get_descriptor(u32, u32 *);
inline pte_t heca_descriptor_to_pte(u32, u32);
inline struct heca_process_list heca_descriptor_to_hprocs(u32);
void remove_hproc_from_descriptors(struct heca_process *);
int swp_entry_to_heca_data(swp_entry_t, struct heca_swp_data *);
int heca_swp_entry_same(swp_entry_t, swp_entry_t);
void heca_clear_swp_entry_flag(struct mm_struct *, unsigned long, pte_t, int);
void init_heca_cache_kmem(void);
void destroy_heca_cache_kmem(void);
struct heca_page_cache *heca_alloc_hpc(struct heca_process *,
                unsigned long, struct heca_process_list, int, int);
void heca_dealloc_hpc(struct heca_page_cache **);
struct heca_page_cache *heca_cache_get(struct heca_process *,
                unsigned long);
struct heca_page_cache *heca_cache_get_hold(struct heca_process *,
                unsigned long);
struct heca_page_cache *heca_cache_release(struct heca_process *,
                unsigned long);
void heca_destroy_page_pool(struct heca_connection *);
int heca_init_page_pool(struct heca_connection *);
struct heca_page_pool_element *heca_fetch_ready_ppe(
                struct heca_connection *);
struct heca_page_pool_element *heca_prepare_ppe(struct heca_connection *,
                struct page *);
void heca_ppe_clear_release(struct heca_connection *,
                struct heca_page_pool_element **);
void init_heca_reader_kmem(void);
u32 heca_lookup_page_read(struct heca_process *, unsigned long);
u32 heca_extract_page_read(struct heca_process *, unsigned long);
int heca_flag_page_read(struct heca_process *, unsigned long, u32);
int heca_cache_add(struct heca_process *, unsigned long, int, int,
                struct heca_page_cache **);
struct heca_page_reader *heca_delete_readers(struct heca_process *,
                unsigned long);
struct heca_page_reader *heca_lookup_readers(struct heca_process *,
                unsigned long);
int heca_add_reader(struct heca_process *, unsigned long, u32);
inline void heca_free_page_reader(struct heca_page_reader *);

#endif /* HECA_STRUCT_H_ */
