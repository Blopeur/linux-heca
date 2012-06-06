/*
 * rb.c
 **  Created on: 7 Jul 2011
 *      Author: Benoit
 */

#include <dsm/dsm_module.h>

static struct dsm_module_state *dsm_state;

struct dsm_module_state * create_dsm_module_state(void) {
    dsm_state = kzalloc(sizeof(struct dsm_module_state), GFP_KERNEL);
    BUG_ON(!(dsm_state));
    INIT_RADIX_TREE(&dsm_state->dsm_tree_root, GFP_KERNEL & ~__GFP_WAIT);
    INIT_RADIX_TREE(&dsm_state->mm_tree_root, GFP_KERNEL & ~__GFP_WAIT);
    INIT_LIST_HEAD(&dsm_state->dsm_list);
    mutex_init(&dsm_state->dsm_state_mutex);
    dsm_state->dsm_tx_wq = alloc_workqueue("dsm_rx_wq", WQ_HIGHPRI | WQ_MEM_RECLAIM,0);
    dsm_state->dsm_rx_wq = alloc_workqueue("dsm_tx_wq", WQ_HIGHPRI | WQ_MEM_RECLAIM,0);
    return dsm_state;
}
EXPORT_SYMBOL(create_dsm_module_state);

void destroy_dsm_module_state(void) {
    mutex_destroy(&dsm_state->dsm_state_mutex);
    destroy_workqueue(dsm_state->dsm_tx_wq);
    destroy_workqueue(dsm_state->dsm_rx_wq);
    kfree(dsm_state);
}
EXPORT_SYMBOL(destroy_dsm_module_state);

struct dsm_module_state * get_dsm_module_state(void) {
    return dsm_state;
}
EXPORT_SYMBOL(get_dsm_module_state);

struct dsm *find_dsm(u32 id) {
    struct dsm_module_state *dsm_state = get_dsm_module_state();
    struct dsm *dsm;
    struct dsm **dsmp;
    struct radix_tree_root *root;

    rcu_read_lock();
    root = &dsm_state->dsm_tree_root;
    repeat: dsm = NULL;
    dsmp = (struct dsm **) radix_tree_lookup_slot(root, (unsigned long) id);
    if (dsmp) {
        dsm = radix_tree_deref_slot((void **) dsmp);
        if (unlikely(!dsm))
            goto out;
        if (radix_tree_exception(dsm)) {
            if (radix_tree_deref_retry(dsm))
                goto repeat;
        }
    }
    out: rcu_read_unlock();
    return dsm;
}
EXPORT_SYMBOL(find_dsm);

static struct subvirtual_machine *_find_svm_in_tree(
        struct radix_tree_root *root, unsigned long svm_id) {

    struct subvirtual_machine *svm;
    struct subvirtual_machine **svmp;

    repeat: svm = NULL;
    svmp = (struct subvirtual_machine **) radix_tree_lookup_slot(root,
            (unsigned long) svm_id);
    if (svmp) {
        svm = radix_tree_deref_slot((void**) svmp);
        if (unlikely(!svm))
            goto out;
        if (radix_tree_exception(svm)) {
            if (radix_tree_deref_retry(svm))
                goto repeat;
        }
    }

    out: return svm;
}
;

struct subvirtual_machine *find_svm(struct dsm *dsm, u32 svm_id) {
    struct subvirtual_machine *svm;

    rcu_read_lock();
    svm = _find_svm_in_tree(&dsm->svm_tree_root, (unsigned long) svm_id);
    rcu_read_unlock();

    return svm;
}
EXPORT_SYMBOL(find_svm);

struct subvirtual_machine *find_local_svm_in_dsm(struct dsm * dsm,
        struct mm_struct *mm) {
    struct subvirtual_machine *svm;

    rcu_read_lock();
    svm = _find_svm_in_tree(&dsm->svm_mm_tree_root, (unsigned long) mm);
    rcu_read_unlock();

    return svm;
}
EXPORT_SYMBOL(find_local_svm_in_dsm);

struct subvirtual_machine *find_local_svm(struct mm_struct *mm) {
    struct subvirtual_machine *svm;

    rcu_read_lock();
    svm = _find_svm_in_tree(&get_dsm_module_state()->mm_tree_root,
            (unsigned long) mm);
    rcu_read_unlock();

    return svm;
}
EXPORT_SYMBOL(find_local_svm);

void insert_rb_conn(struct conn_element *ele) {
    struct rcm *rcm = get_dsm_module_state()->rcm;
    struct rb_root *root;
    struct rb_node **new;
    struct rb_node *parent = NULL;
    struct conn_element *this;

    write_seqlock(&rcm->conn_lock);
    root = &rcm->root_conn;
    new = &root->rb_node;
    while (*new) {
        this = rb_entry(*new, struct conn_element, rb_node);
        parent = *new;
        if (ele->remote_node_ip < this->remote_node_ip)
            new = &((*new)->rb_left);
        else if (ele->remote_node_ip > this->remote_node_ip)
            new = &((*new)->rb_right);
    }
    rb_link_node(&ele->rb_node, parent, new);
    rb_insert_color(&ele->rb_node, root);
    write_sequnlock(&rcm->conn_lock);

}
EXPORT_SYMBOL(insert_rb_conn);

// Return NULL if no element contained within tree.
struct conn_element* search_rb_conn(int node_ip) {
    struct rcm *rcm = get_dsm_module_state()->rcm;
    struct rb_root *root;
    struct rb_node *node;
    struct conn_element *this = 0;
    unsigned long seq;

    do {
        seq = read_seqbegin(&rcm->conn_lock);
        root = &rcm->root_conn;
        for (node = root->rb_node; node; this = 0) {
            this = rb_entry(node, struct conn_element, rb_node);

            if (node_ip < this->remote_node_ip)
                node = node->rb_left;
            else if (node_ip > this->remote_node_ip)
                node = node->rb_right;
            else
                break;
        }
    } while (read_seqretry(&rcm->conn_lock, seq));

    return this;
}
EXPORT_SYMBOL(search_rb_conn);

void erase_rb_conn(struct conn_element *ele) {
    struct rcm *rcm = get_dsm_module_state()->rcm;

    write_seqlock(&rcm->conn_lock);
    rb_erase(&ele->rb_node, &rcm->root_conn);
    write_sequnlock(&rcm->conn_lock);
}
EXPORT_SYMBOL(erase_rb_conn);

void insert_mr(struct dsm *dsm, struct memory_region *mr) {
    struct rb_root *root = &dsm->mr_tree_root;
    struct rb_node **new = &root->rb_node;
    struct rb_node *parent = NULL;
    struct memory_region *this;
    write_seqlock(&dsm->mr_seq_lock);
    while (*new) {
        this = rb_entry(*new, struct memory_region, rb_node);
        parent = *new;
        if (mr->addr < this->addr)
            new = &((*new)->rb_left);
        else if (mr->addr > this->addr)
            new = &((*new)->rb_right);
    }

    rb_link_node(&mr->rb_node, parent, new);
    rb_insert_color(&mr->rb_node, root);
    write_sequnlock(&dsm->mr_seq_lock);
}
EXPORT_SYMBOL(insert_mr);

// Return NULL if no element contained within tree.
struct memory_region *search_mr(struct dsm *dsm, unsigned long addr) {
    struct rb_root *root = &dsm->mr_tree_root;
    struct rb_node *node = root->rb_node;
    struct memory_region *this = NULL;
    unsigned long seq;
    do {
        seq = read_seqbegin(&dsm->mr_seq_lock);
        while (node) {
            this = rb_entry(node, struct memory_region, rb_node);

            if (addr < this->addr)
                node = node->rb_left;
            else if (addr > this->addr)
                if (addr < (this->addr + this->sz))
                    break;
                else
                    node = node->rb_right;
            else
                break;

        }
    } while (read_seqretry(&dsm->mr_seq_lock, seq));
    return this;

}
EXPORT_SYMBOL(search_mr);

int destroy_mrs(struct dsm *dsm, int force) {
    struct rb_root *root = &dsm->mr_tree_root;
    struct rb_node *node;
    struct memory_region *mr;
    int ret = 0, i;
    struct svm_list svms;

    write_seqlock(&dsm->mr_seq_lock);
    for (node = rb_first(root); node; node = rb_next(node)) {
        mr = rb_entry(node, struct memory_region, rb_node);
        svms = dsm_descriptor_to_svms(mr->descriptor);
        if (!force) {
            for (i = 0; i < svms.num; i++) {
                if (svms.pp[i])
                    goto next;
            }
        }

        printk("[destroy_mrs] [%lu, %lu)\n", mr->addr, mr->addr + mr->sz);
        rb_erase(&mr->rb_node, root);
        kfree(mr);
        ret++;

        next: continue;
    }
    write_sequnlock(&dsm->mr_seq_lock);

    return ret;
}
EXPORT_SYMBOL(destroy_mrs);

/* svm_descriptors */
static struct svm_list *sdsc;
static u32 sdsc_max;
static struct mutex sdsc_lock;

static void dsm_descriptors_realloc(void)
{
    struct svm_list *new_sdsc;
    u32 new_sdsc_max;

    might_sleep();
    new_sdsc_max = sdsc_max + 256;
    new_sdsc = kzalloc(sizeof(struct svm_list) * new_sdsc_max, GFP_KERNEL);
    BUG_ON(!new_sdsc);
    if (sdsc) {
        memcpy(new_sdsc, sdsc, sizeof(struct svm_list) * sdsc_max);
        kfree(sdsc);
    }
    sdsc_max = new_sdsc_max;
    rcu_assign_pointer(sdsc, new_sdsc);
    synchronize_rcu();
}

void dsm_init_descriptors(void) {
    mutex_init(&sdsc_lock);
    mutex_lock(&sdsc_lock);
    dsm_descriptors_realloc();
    mutex_unlock(&sdsc_lock);
}
EXPORT_SYMBOL(dsm_init_descriptors);

void dsm_destroy_descriptors(void) {
    int i;

    for (i = 0; i < sdsc_max; i++)
        if (sdsc[i].pp)
            kfree(sdsc[i].pp);
    kfree(sdsc);
}
EXPORT_SYMBOL(dsm_destroy_descriptors);

static inline void dsm_add_descriptor(struct dsm *dsm, u32 i, u32 *svm_ids) {
    u32 j;

    might_sleep();
    for (j = 0; svm_ids[j]; j++)
        ;
    sdsc[i].num = j;
    sdsc[i].pp = kmalloc(sizeof(struct subvirtual_machine *) * j, GFP_KERNEL);
    for (j = 0; svm_ids[j]; j++)
        sdsc[i].pp[j] = find_svm(dsm, svm_ids[j]);
}

/*
 * svm_ids should be NULL terminated
 *
 */
u32 dsm_get_descriptor(struct dsm *dsm, u32 *svm_ids) {
    u32 i, j;

    mutex_lock(&sdsc_lock);
    for (i = 0; i < sdsc_max && sdsc[i].num; i++) {
        for (j = 0;
                j < sdsc[i].num && sdsc[i].pp[j] && svm_ids[j] && sdsc[i].pp[j]->svm_id == svm_ids[j];
                j++)
            ;
        if (j == sdsc[i].num && !svm_ids[j])
            break;
    }

    if (i + 1 == sdsc_max)
        dsm_descriptors_realloc();

    if (!sdsc[i].num)
        dsm_add_descriptor(dsm, i, svm_ids);

    mutex_unlock(&sdsc_lock);
    return i;
}
;
EXPORT_SYMBOL(dsm_get_descriptor);

inline swp_entry_t dsm_descriptor_to_swp_entry(u32 dsc, u32 flags) {
    u64 val = dsc;
    return val_to_dsm_entry((val << 24) | flags);
}

struct svm_list dsm_descriptor_to_svms(u32 dsc) {
    struct svm_list svms;

    rcu_read_lock();
    svms = rcu_dereference(sdsc)[dsc];
    rcu_read_unlock();
    return svms;
}
EXPORT_SYMBOL(dsm_descriptor_to_svms);

inline struct dsm_swp_data swp_entry_to_dsm_data(swp_entry_t entry) {
    struct dsm_swp_data dsd;
    u64 val = dsm_entry_to_val(entry);
    int i;

    dsd.flags = val & 0xFFFFFF;
    dsd.svms = dsm_descriptor_to_svms(val >> 24);
    for (i = 0; i < dsd.svms.num; i++) {
        if (dsd.svms.pp[i]) {
            dsd.dsm = dsd.svms.pp[i]->dsm;
            goto out;
        }
    }
    dsd.dsm = NULL;

    out: return dsd;
}

inline int dsm_swp_entry_same(swp_entry_t entry, swp_entry_t entry2) {

    u64 val = dsm_entry_to_val(entry) >> 24;
    u64 val2 = dsm_entry_to_val(entry2) >> 24;

    if (val == val2)
        return 1;
    return 0;

}

void clear_dsm_swp_entry_flag(struct mm_struct *mm, unsigned long addr,
        pte_t * pte, int pos) {
    u64 val;
    u32 flags;
    swp_entry_t entry;
    pte_t tmp_pte;

    tmp_pte = *pte;
    entry = pte_to_swp_entry(tmp_pte);
    val = dsm_entry_to_val(entry);
    flags = val & 0xFFFFFF;
    clear_bit(pos, (volatile long unsigned int *) &flags);
    val = val >> 24;
    set_pte_at(mm, addr, pte,
            swp_entry_to_pte(dsm_descriptor_to_swp_entry(val, flags)));

}
EXPORT_SYMBOL(clear_dsm_swp_entry_flag);
