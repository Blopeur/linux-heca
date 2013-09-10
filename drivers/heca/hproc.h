/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#ifndef HPROC_H_
#define HPROC_H_

#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <asm/atomic.h>
#include <linux/llist.h>
#include <linux/rbtree.h>
#include <linux/mm_types.h>
#include <linux/seqlock.h>
#include <linux/radix-tree.h>

#include "struct.h"
#include "hspace.h"
#include "mr.h"
#include "hutils.h"

/*
 * Useful macro for parsing heca processes
 */
#define for_each_valid_hproc(hprocs, i) \
        for (i = 0; i < (hprocs).num; i++) \
                if (likely((hprocs).ids[i]))

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

        struct kobject kobj;
        struct kset *hmrs_kset;

        struct llist_head delayed_gup;
        struct delayed_work delayed_gup_work;

        struct llist_head deferred_gups;
        struct work_struct deferred_gup_work;

};


inline struct heca_process *find_hproc(struct heca_space *, u32);
inline struct heca_process *find_local_hproc_in_hspace(struct heca_space *,
                struct mm_struct *);
inline struct heca_process *find_local_hproc_from_mm(struct mm_struct *);
int create_hproc(struct hecaioc_hproc *);
void teardown_hproc_by_id(u32, u32);
void teardown_hproc(struct heca_space *, struct heca_process *);
struct heca_process *find_any_hproc(struct heca_space *,
                struct heca_process_list);
struct heca_process *find_local_hproc_from_list(struct heca_space *);
int is_hproc_local(struct heca_process *);
struct heca_process *hproc_get(struct heca_process *);
struct heca_process * __must_check hproc_get_unless_zero(struct heca_process *);
void hproc_put(struct heca_process *);

#endif /* HPROC_H_ */
