/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#ifndef HSPACE_H_
#define HSPACE_H_

#include <linux/heca.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/radix-tree.h>
#include <linux/kobject.h>

struct heca_space {
        u32 hspace_id;

        spinlock_t radix_lock;
        struct radix_tree_root hprocs_tree_root;
        struct radix_tree_root hprocs_mm_tree_root;

        struct mutex hspace_mutex;
        struct list_head hprocs_list;

        struct list_head hspace_ptr;

        struct kobject kobj;
        struct kset *hprocs_kset;
        struct rcu_head rcu;
        struct kref kref;
};

struct heca_space *search_for_hspace(u32, int);

#define find_get_hspace(id)     search_for_hspace(id,1)
#define find_hspace(id)         search_for_hspace(id,0)



int hspace_put(struct heca_space *);
void hspace_get(struct heca_space *);
struct heca_space * __must_check hspace_get_unless_zero(struct heca_space *);
int instantiate_hspace(struct hecaioc_hspace *);
void teardown_hspace(struct heca_space *);
int teardown_hspace_by_id(__u32 );
#endif /* HSPACE_H_ */
