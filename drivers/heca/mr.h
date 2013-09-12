/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#ifndef MR_H_
#define MR_H_

#include <linux/types.h>
#include <linux/rbtree.h>

#include "ioctl.h"
#include "hproc.h"
#include "hutils.h"

struct heca_memory_region {
        unsigned long addr;
        unsigned long sz;
        u32 descriptor;
        u32 hmr_id;
        u32 flags;
        struct rb_node rb_node;

        struct rcu_head rcu;
        struct kref kref;
        struct kobject kobj;
};


struct heca_memory_region * __must_check hmr_get_unless_zero(
                struct heca_memory_region *);
void hmr_get(struct heca_memory_region *);
int hmr_put(struct heca_memory_region *);
struct heca_memory_region *find_heca_mr(struct heca_process *, u32);
struct heca_memory_region *search_heca_mr_by_addr(struct heca_process *,
                unsigned long);
int create_heca_mr(struct hecaioc_hmr *udata);
void teardown_heca_memory_region(struct heca_process *,
                struct heca_memory_region *);
#endif /* MR_H_ */
