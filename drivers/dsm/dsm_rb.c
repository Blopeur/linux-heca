/*
 * rb.c
 *
 *  Created on: 7 Jul 2011
 *      Author: john
 */

#include <dsm/dsm_rb.h>
#include <dsm/dsm_def.h>
#include <dsm/dsm_core.h>

void insert_rb_conn(struct rcm *rcm, struct conn_element *ele) {
    struct rb_root *root = &rcm->root_conn;
    struct rb_node **new = &root->rb_node;
    struct rb_node *parent = NULL;
    struct conn_element *this;

    while (*new) {
        this = rb_entry(*new, struct conn_element, rb_node);

        parent = *new;

        if (ele->remote_node_ip < this->remote_node_ip) {
            new = &((*new)->rb_left);
        } else if (ele->remote_node_ip > this->remote_node_ip) {
            new = &((*new)->rb_right);
        }
    }

    rb_link_node(&ele->rb_node, parent, new);
    rb_insert_color(&ele->rb_node, root);

}

// Return NULL if no element contained within tree.
struct conn_element* search_rb_conn(struct rcm *rcm, int node_ip) {
    struct rb_root *root = &rcm->root_conn;
    struct rb_node *node = root->rb_node;
    struct conn_element *this = 0;

    while (node) {
        this = rb_entry(node, struct conn_element, rb_node);

        if (node_ip < this->remote_node_ip) {
            node = node->rb_left;

        } else if (node_ip > this->remote_node_ip) {
            node = node->rb_right;

        } else
            break;

    }

    return this;

}

// Function will free the element
void erase_rb_conn(struct rb_root *root, struct conn_element *ele) {
    BUG_ON(!ele);

    rb_erase(&ele->rb_node, root);

    kfree(ele);
}

////////////////////////////////////////////////////////////////////////////////////////////
// TO BE REMOVED
//void insert_rb_route(struct rcm *rcm, struct subvirtual_machine *rele) {
//    struct rb_root *root = &rcm->root_route;
//    struct rb_node **new = &root->rb_node;
//    struct rb_node *parent = NULL;
//    struct subvirtual_machine *this;
//    u32 rb_val;
//    u32 val = dsm_vm_id_to_u32(&rele->id);
//
//    while (*new) {
//        this = rb_entry(*new, struct subvirtual_machine, rb_node);
//
//        rb_val = dsm_vm_id_to_u32(&this->id);
//
//        parent = *new;
//
//        if (val < rb_val) {
//            new = &((*new)->rb_left);
//
//        } else if (val > rb_val) {
//            new = &((*new)->rb_right);
//
//        }
//
//    }
//
//    rb_link_node(&rele->rb_node, parent, new);
//    rb_insert_color(&rele->rb_node, root);
//
//}
//
//// Return NULL if no element contained within tree.
//struct subvirtual_machine* search_rb_route(struct rcm *rcm, struct dsm_vm_id *id) {
//    struct rb_root *root = &rcm->root_route;
//    struct rb_node *node = root->rb_node;
//    u32 rb_val;
//    u32 val = dsm_vm_id_to_u32(id);
//    struct subvirtual_machine *this = NULL;
//
//    while (node) {
//        this = rb_entry(node, struct subvirtual_machine, rb_node);
//
//        rb_val = dsm_vm_id_to_u32(&this->id);
//
//        if (val < rb_val) {
//            node = node->rb_left;
//
//        } else if (val > rb_val) {
//            node = node->rb_right;
//
//        } else {
//            return this;
//        }
//
//    }
//
//    return NULL;
//
//}
//
//// Function will free the element
//void erase_rb_route(struct rb_root *root, struct subvirtual_machine *rele) {
//    BUG_ON(!rele);
//
//    rb_erase(&rele->rb_node, root);
//    kfree(rele);
//
//}
// TO BE REMOVED
////////////////////////////////////////////////////////////////////////////////////////////


/*
 * page swap RB_TREE
 */
static void __insert_rb_swap(struct rb_root *root, struct swp_element *ele)
{
    struct rb_node **new = &root->rb_node;
    struct rb_node *parent = NULL;
    struct swp_element *this;

    while (*new) {
        this = rb_entry(*new, struct swp_element, rb);

        parent = *new;

        if (ele->addr < this->addr) {
            new = &((*new)->rb_left);

        } else if (ele->addr > this->addr) {
            new = &((*new)->rb_right);

        }

        // DSM1  - need tests to ensure there no double entries!

    }

    rb_link_node(&ele->rb, parent, new);
    rb_insert_color(&ele->rb, root);

}

struct swp_element * insert_rb_swap(struct rb_root *root, unsigned long addr, struct dsm_vm_id *id)
{
    struct swp_element *ele = kmalloc(sizeof(*ele), GFP_KERNEL);

    if (!ele)
        return NULL;

    ele->addr = addr;
    ele->id.dsm_id = id->dsm_id;
    ele->id.svm_id = id->svm_id;

    __insert_rb_swap(root, ele);

    return ele;

}

struct swp_element* search_rb_swap(struct rb_root *root, unsigned long addr) {
    struct rb_node *node = root->rb_node;
    struct swp_element *this;

    while (node) {
        this = rb_entry(node, struct swp_element, rb);

        if (addr < this->addr)
            node = node->rb_left;
        else if (addr > this->addr)
            node = node->rb_right;
        else
            return this;

    }

    return NULL;

}

void erase_rb_swap(struct rb_root *root, struct swp_element *ele) {
    BUG_ON(!ele);

    rb_erase(&ele->rb, root);

    kfree(ele);

}
