/*
 * dsm_sr.h
 *
 *  Created on: 26 Jul 2011
 *      Author: john
 */

#ifndef DSM_SR_H_
#define DSM_SR_H_

#include <rdma/ib_cm.h>
#include <rdma/ib_verbs.h>
#include <dsm/dsm_def.h>
#include <dsm/dsm_op.h>

void init_kmem_request_cache(void);

void destroy_kmem_request_cache(void);
/*
 * processing message in recv handler
 */
int process_response(struct conn_element *, struct tx_buf_ele *);
int rx_tx_message_transfer(struct conn_element *, struct rx_buf_ele *);
/*
 * Step 0 : Exchange rdma info
 */

int exchange_info(struct conn_element *, int);
int dsm_send_info(struct conn_element *);
int dsm_recv_info(struct conn_element *);

/*
 * Step 1 : ask to send a message
 */

int request_dsm_page(struct page *, struct subvirtual_machine *,
        struct subvirtual_machine *, uint64_t, void(*func)(struct tx_buf_ele *),
        int);

int dsm_request_page_pull(struct mm_struct *, struct subvirtual_machine *,
        struct subvirtual_machine *, unsigned long);
/*
 * Step 2 : sending message
 */
int tx_dsm_send(struct conn_element *, struct tx_buf_ele *);

#endif /* DSM_SR_H_ */
