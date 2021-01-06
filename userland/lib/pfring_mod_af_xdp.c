/*
 *
 * (C) 2020-2021 - ntop.org
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lessed General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 *
 */

#include <unistd.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/ether.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>

#include <linux/if_ether.h>
#include <linux/if_xdp.h>
#include <linux/if_link.h>

#include <bpf/xsk.h>

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define _BPF_H_ /* Fix redefinition of struct bpf_insn from libpcap */

#include "pfring.h"
#include "pfring_priv.h"
#include "pfring_utils.h"
#include "pfring_hw_filtering.h"
#include "pfring_mod.h"
#include "pfring_mod_af_xdp.h"

#define AF_XDP_DEV_MAX_QUEUES      16
#define AF_XDP_DEV_NUM_BUFFERS     4096
#define AF_XDP_DEV_NUM_DESC        XSK_RING_CONS__DEFAULT_NUM_DESCS
#define AF_XDP_DEV_FRAME_SIZE      XSK_UMEM__DEFAULT_FRAME_SIZE
#define AF_XDP_DEV_DATA_HEADROOM   0
#define AF_XDP_DEV_RX_BATCH_SIZE   32
#define AF_XDP_DEV_TX_BATCH_SIZE   32

struct pf_xdp_buff_queue {
  u_int64_t num_items;
  u_int64_t head;
  u_int64_t tail;
  void **items;
};

struct pf_xdp_xsk_umem_info {
  struct xsk_ring_prod fq;
  struct xsk_ring_cons cq;
  struct xsk_umem *umem;
  struct pf_xdp_buff_queue *buff_q;
  void *buffer;
};

struct pf_xdp_rx_stats {
  u_int64_t rx_pkts;
  u_int64_t rx_bytes;
  u_int64_t rx_dropped;
};

struct pf_xdp_tx_stats {
  u_int64_t tx_pkts;
  u_int64_t err_pkts;
  u_int64_t tx_bytes;
};

struct pf_xdp_pkt {
  void *buf;
  int len;
};

struct pf_xdp_rx_queue {
  struct xsk_ring_cons rx;
  struct pf_xdp_xsk_umem_info *umem;
  struct xsk_socket *xsk;
  struct pf_xdp_rx_stats stats;
  u_int16_t queue_idx;
};

struct pf_xdp_tx_queue {
  struct xsk_ring_prod tx;
  struct pf_xdp_xsk_umem_info *umem;
  struct xsk_socket *xsk;
  struct pf_xdp_tx_stats stats;
  u_int16_t queue_idx;
};

struct pf_xdp_handle {
  int if_index;
  u_int16_t queue_idx;
  char if_name[IFNAMSIZ];
  struct ether_addr eth_addr;
  struct pf_xdp_xsk_umem_info *umem;

  struct pf_xdp_rx_queue rx_queue;
  struct pf_xdp_tx_queue tx_queue;
};

/* **************************************************** */

static struct pf_xdp_buff_queue *pf_xdp_buff_q_create(u_int64_t num_items) {
  struct pf_xdp_buff_queue *buff_q;

  buff_q = calloc(1, sizeof(struct pf_xdp_buff_queue));

  if (buff_q == NULL) 
    return NULL;

  buff_q->items = calloc(num_items, sizeof(void *));

  if (buff_q->items == NULL) {
    free(buff_q);
    return NULL;
  }

  buff_q->tail = num_items - 1;
  buff_q->head = 0;
  buff_q->num_items = num_items;

  return buff_q;
}

/* **************************************************** */

static void pf_xdp_buff_q_free(struct pf_xdp_buff_queue *buff_q) {
  free(buff_q->items);
  free(buff_q);
}

/* **************************************************** */

static inline int pf_xdp_buff_q_dequeue(struct pf_xdp_buff_queue *buff_q, void **item) {
  u_int32_t next_tail;

  next_tail = (buff_q->tail + 1);
  if (next_tail == buff_q->num_items) 
    next_tail = 0;

  if (next_tail != buff_q->head) {
    *item = buff_q->items[next_tail];
    buff_q->tail = next_tail;
    return 0;
  }

  return -1;
}

/* **************************************************** */

static inline int pf_xdp_buff_q_enqueue(struct pf_xdp_buff_queue *buff_q, void *item) {
  u_int32_t next_head;

  next_head = (buff_q->head + 1);
  if (next_head == buff_q->num_items)
    next_head = 0;

  if (buff_q->tail != next_head) {
    buff_q->items[buff_q->head] = item;
    buff_q->head = next_head;
    return 0;
  }

  return -1;
}

/* **************************************************** */

static inline int pf_xdp_refill_queue(struct pf_xdp_xsk_umem_info *umem, u_int16_t reserve_size) {
  struct xsk_ring_prod *fq = &umem->fq;
  u_int32_t provided = 0;
  u_int32_t idx;
  u_int16_t i;

  if (unlikely(!xsk_ring_prod__reserve(fq, reserve_size, &idx))) {
    printf("Failed to reserve enough fq descs.\n");
    return -1;
  }

  for (i = 0; i < reserve_size; i++) {
    __u64 *fq_addr;
    void *addr;

    /* FIXX what if we fail dequeueing reserve_size buffers reserved with xsk_ring_prod__reserve? */
    if (pf_xdp_buff_q_dequeue(umem->buff_q, &addr) != 0)
      break;

    fq_addr = xsk_ring_prod__fill_addr(fq, idx++);
    *fq_addr = (u_int64_t) addr;

    provided++;
  }

  xsk_ring_prod__submit(fq, provided);

  return 0;
}

/* **************************************************** */

int pfring_mod_af_xdp_is_pkt_available(pfring *ring) {
  struct pf_xdp_handle *handle = (struct pf_xdp_handle *) ring->priv_data;
  struct xsk_ring_cons *rx = &handle->rx_queue.rx;

  return xsk_cons_nb_avail(rx, 1);
}

/* **************************************************** */

int pfring_mod_af_xdp_get_selectable_fd(pfring *ring) {
  struct pf_xdp_handle *handle = (struct pf_xdp_handle *) ring->priv_data;

  return xsk_socket__fd(handle->rx_queue.xsk);
}

/* **************************************************** */

int pfring_mod_af_xdp_poll(pfring *ring, u_int wait_duration) {
  struct pf_xdp_handle *handle = (struct pf_xdp_handle *) ring->priv_data;
  struct pollfd fds[1];
  int ret, timeout, nfds = 1;

  memset(fds, 0, sizeof(fds));

  fds[0].fd = xsk_socket__fd(handle->rx_queue.xsk);
  fds[0].events = POLLIN;
  timeout = wait_duration;

  ret = poll(fds, nfds, timeout);

  return ret;
}

/* **************************************************** */

static u_int16_t pfring_mod_af_xdp_recv_multi(void *queue, struct pf_xdp_pkt *pkts, u_int16_t nb_pkts, int wait) {
  struct pf_xdp_rx_queue *rxq = queue;
  struct xsk_ring_cons *rx = &rxq->rx;
  struct pf_xdp_xsk_umem_info *umem = rxq->umem;
  struct xsk_ring_prod *fq = &umem->fq;
  const struct xdp_desc *desc;
  u_int32_t free_thresh = fq->size >> 1;
  u_int32_t idx_rx = 0;
  u_int64_t rx_bytes = 0;
  int n, i;

  nb_pkts = min(nb_pkts, AF_XDP_DEV_RX_BATCH_SIZE);

  n = xsk_ring_cons__peek(rx, nb_pkts, &idx_rx);

  if (n == 0) 
    return 0;

  if (xsk_prod_nb_free(fq, free_thresh) >= free_thresh)
    pf_xdp_refill_queue(umem, AF_XDP_DEV_RX_BATCH_SIZE);

  for (i = 0; i < n; i++) {
    desc = xsk_ring_cons__rx_desc(rx, idx_rx++);

    pkts[i].buf = xsk_umem__get_data(rxq->umem->buffer, desc->addr);
    pkts[i].len = desc->len;

    rx_bytes += desc->len;

    pf_xdp_buff_q_enqueue(umem->buff_q, (void *) desc->addr);
  }

  xsk_ring_cons__release(rx, n);

  rxq->stats.rx_pkts += n;
  rxq->stats.rx_bytes += rx_bytes;

  return n;
}

/* **************************************************** */

int pfring_mod_af_xdp_recv(pfring *ring, u_char** buffer, u_int buffer_len, struct pfring_pkthdr *hdr, u_int8_t wait_for_incoming_packet) {
  struct pf_xdp_handle *handle = (struct pf_xdp_handle *) ring->priv_data;
  struct pf_xdp_pkt p[1];
  u_int32_t duration = 1;

  if (unlikely(ring->reentrant)) pthread_rwlock_wrlock(&ring->rx_lock);

 redo_recv:
  if (likely(pfring_mod_af_xdp_recv_multi(&handle->rx_queue, p, 1, wait_for_incoming_packet) > 0)) {

    if (unlikely(ring->sampling_rate > 1)) {
      if (likely(ring->sampling_counter > 0)) {
        ring->sampling_counter--;
	goto redo_recv;
      } else {
        ring->sampling_counter = ring->sampling_rate-1;
      }
    }

    hdr->len = hdr->caplen = p[0].len;
    hdr->extended_hdr.pkt_hash = 0;
    hdr->extended_hdr.rx_direction = 1;
    hdr->extended_hdr.timestamp_ns = 0;

    if (unlikely(buffer_len || ring->force_timestamp)) {
      gettimeofday(&hdr->ts, NULL);
    } else {
      /* as speed is required, we are not setting the sw time */
      hdr->ts.tv_sec = 0;
      hdr->ts.tv_usec = 0;
    }

    if (likely(buffer_len == 0)) {
      *buffer = p[0].buf;
    } else {
      if (buffer_len < p[0].len)
        hdr->caplen = buffer_len;

      memcpy(*buffer, p[0].buf, hdr->caplen);
      memset(&hdr->extended_hdr.parsed_pkt, 0, sizeof(hdr->extended_hdr.parsed_pkt));
      pfring_parse_pkt(*buffer, hdr, 4, 0 /* ts */, 1 /* hash */);
    }

    hdr->caplen = min_val(hdr->caplen, ring->caplen);

    if (unlikely(ring->reentrant)) pthread_rwlock_unlock(&ring->rx_lock);
    
    return 1;
  }

  if (wait_for_incoming_packet) {

    if (unlikely(ring->break_recv_loop)) {
      if (unlikely(ring->reentrant)) pthread_rwlock_unlock(&ring->rx_lock);
      errno = EINTR;
      return 0;
    }
      
    if (unlikely(pfring_mod_af_xdp_poll(ring, duration) == -1 && errno != EINTR)) {
      if (unlikely(ring->reentrant)) pthread_rwlock_unlock(&ring->rx_lock);
      return -1;
    }

    if (duration < ring->poll_duration) {
      duration += 10;
      if (unlikely(duration > ring->poll_duration)) 
        duration = ring->poll_duration;
    }

    goto redo_recv;
  }

  if (unlikely(ring->reentrant)) pthread_rwlock_unlock(&ring->rx_lock);
  return 0;
}

/* **************************************************** */

static void pf_xdp_cleanup_tx_cq(struct pf_xdp_xsk_umem_info *umem, int size) {
  struct xsk_ring_cons *cq = &umem->cq;
  u_int32_t idx_cq = 0;
  u_int64_t addr;
  int i, n;

  n = xsk_ring_cons__peek(cq, size, &idx_cq);

  for (i = 0; i < n; i++) {
    addr = *xsk_ring_cons__comp_addr(cq, idx_cq++);
    pf_xdp_buff_q_enqueue(umem->buff_q, (void *) addr);
  }

  xsk_ring_cons__release(cq, n);
}

/* **************************************************** */

static void pf_xdp_flush_tx_q(struct pf_xdp_tx_queue *txq) {
  struct pf_xdp_xsk_umem_info *umem = txq->umem;

  while (send(xsk_socket__fd(txq->xsk), NULL, 0, MSG_DONTWAIT) < 0) {

    if (errno != EBUSY && errno != EAGAIN && errno != EINTR)
      break;

    if (errno == EAGAIN)
      pf_xdp_cleanup_tx_cq(umem, AF_XDP_DEV_TX_BATCH_SIZE);
  }

  pf_xdp_cleanup_tx_cq(umem, AF_XDP_DEV_TX_BATCH_SIZE);
}

/* **************************************************** */

static u_int16_t pfring_mod_af_xdp_send_burst(void *queue, struct pf_xdp_pkt *pkts, u_int16_t nb_pkts) {
  struct pf_xdp_tx_queue *txq = queue;
  struct pf_xdp_xsk_umem_info *umem = txq->umem;
  struct pf_xdp_pkt *pkt_i;
  void *addrs[AF_XDP_DEV_TX_BATCH_SIZE];
  u_int64_t tx_bytes = 0;
  u_int32_t idx_tx;
  int i;

  nb_pkts = min(nb_pkts, AF_XDP_DEV_TX_BATCH_SIZE);

  pf_xdp_cleanup_tx_cq(umem, nb_pkts);

  for (i = 0; i < nb_pkts; i++) {
    void *addr;

    if (pf_xdp_buff_q_dequeue(umem->buff_q, &addr) != 0) {
      nb_pkts = i;
      break;
    }

    addrs[i] = addr;
  }

  if (nb_pkts == 0)
    return 0;

  if (xsk_ring_prod__reserve(&txq->tx, nb_pkts, &idx_tx) != nb_pkts) {
    pf_xdp_flush_tx_q(txq);
    for (i = 0; i < nb_pkts; i++)
      pf_xdp_buff_q_enqueue(umem->buff_q, addrs[i]);
    return 0;
  }

  for (i = 0; i < nb_pkts; i++) {
    struct xdp_desc *desc;
    void *pkt;

    desc = xsk_ring_prod__tx_desc(&txq->tx, idx_tx + i);
    pkt_i = &pkts[i];

    desc->addr = (u_int64_t)addrs[i];
    desc->len = pkt_i->len;
    pkt = xsk_umem__get_data(umem->buffer, desc->addr);
    memcpy(pkt, pkt_i->buf, desc->len);

    tx_bytes += pkt_i->len;
  }

  xsk_ring_prod__submit(&txq->tx, nb_pkts);

  pf_xdp_flush_tx_q(txq);

  txq->stats.tx_pkts += nb_pkts;
  txq->stats.tx_bytes += tx_bytes;

  return nb_pkts;
}

/* **************************************************** */

int pfring_mod_af_xdp_send(pfring *ring, char *pkt, u_int pkt_len, u_int8_t flush_packet) {
  struct pf_xdp_handle *handle = (struct pf_xdp_handle *) ring->priv_data;
  struct pf_xdp_pkt p[1];

  p[0].buf = pkt;
  p[0].len = pkt_len;

  if (pfring_mod_af_xdp_send_burst(&handle->tx_queue, p, 1) > 0) 
    return pkt_len;

  return -1;
}

/* **************************************************** */

int pfring_mod_af_xdp_stats(pfring *ring, pfring_stat *stats) {
  struct pf_xdp_handle *handle = (struct pf_xdp_handle *) ring->priv_data;
  struct xdp_statistics xdp_stats;
  struct pf_xdp_rx_queue *rxq;
  socklen_t slen;
  int ret;

  memset(stats, 0, sizeof(*stats));

  slen = sizeof(struct xdp_statistics);
  rxq = &handle->rx_queue;
  stats->recv += handle->rx_queue.stats.rx_pkts;
  stats->drop += handle->rx_queue.stats.rx_dropped;

  ret = getsockopt(xsk_socket__fd(rxq->xsk), SOL_XDP, XDP_STATISTICS, &xdp_stats, &slen);

  if (ret == 0)
    stats->drop += xdp_stats.rx_dropped;

  /* Other available stats: 
  handle->rx_queue.stats.rx_bytes;
  handle->tx_queue.stats.tx_pkts;
  handle->tx_queue.stats.tx_bytes;
  handle->tx_queue.stats.err_pkts;
  */

  return 0;
}

/* **************************************************** */

static void pf_xdp_remove_xdp_program(struct pf_xdp_handle *handle) {
  u_int32_t curr_prog_id = 0;

  if (bpf_get_link_xdp_id(handle->if_index, &curr_prog_id, XDP_FLAGS_UPDATE_IF_NOEXIST)) {
    fprintf(stderr, "Failure in bpf_get_link_xdp_id\n");
    return;
  }

  bpf_set_link_xdp_fd(handle->if_index, -1, XDP_FLAGS_UPDATE_IF_NOEXIST);
}

/* **************************************************** */

static void pf_xdp_umem_destroy(struct pf_xdp_xsk_umem_info *umem) {
  pf_xdp_buff_q_free(umem->buff_q);

  free(umem->buffer);
  free(umem);
}

/* **************************************************** */

static void pf_xdp_dev_close(struct pf_xdp_handle *handle) {
  xsk_socket__delete(handle->rx_queue.xsk);

  (void)xsk_umem__delete(handle->umem->umem);
  pf_xdp_umem_destroy(handle->umem);

  pf_xdp_remove_xdp_program(handle);
}

/* **************************************************** */

static struct pf_xdp_xsk_umem_info *pf_xdp_umem_configure(struct pf_xdp_handle *handle) {
  struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
  struct pf_xdp_xsk_umem_info *umem;
  struct xsk_umem_config usr_config = {
    .fill_size = AF_XDP_DEV_NUM_DESC,
    .comp_size = AF_XDP_DEV_NUM_DESC,
    .frame_size = AF_XDP_DEV_FRAME_SIZE,
    .frame_headroom = AF_XDP_DEV_DATA_HEADROOM };
  int ret;
  u_int64_t i;

  umem = calloc(1, sizeof(*umem));

  if (umem == NULL) {
    fprintf(stderr, "Failed to allocate umem info");
    return NULL;
  }

  umem->buff_q = pf_xdp_buff_q_create(AF_XDP_DEV_NUM_BUFFERS);

  if (umem->buff_q == NULL) {
    fprintf(stderr, "Failed to create buffers queue\n");
    goto err;
  }

  if (setrlimit(RLIMIT_MEMLOCK, &r)) {
    fprintf(stderr, "Error in setrlimit(RLIMIT_MEMLOCK): %s\n",
      strerror(errno));
    goto err;
  }

  ret = posix_memalign(&umem->buffer, getpagesize(), /* PAGE_SIZE aligned */
           AF_XDP_DEV_NUM_BUFFERS * AF_XDP_DEV_FRAME_SIZE);

  if (ret) {
    fprintf(stderr, "Error allocating memory: %s\n",
      strerror(errno));
    goto err;
  }

  for (i = 0; i < AF_XDP_DEV_NUM_BUFFERS; i++)
    pf_xdp_buff_q_enqueue(umem->buff_q, (void *)(i * AF_XDP_DEV_FRAME_SIZE + AF_XDP_DEV_DATA_HEADROOM));

  ret = xsk_umem__create(&umem->umem, umem->buffer,
             AF_XDP_DEV_NUM_BUFFERS * AF_XDP_DEV_FRAME_SIZE,
             &umem->fq, &umem->cq,
             &usr_config);

  if (ret) {
    fprintf(stderr, "Failed to create umem\n");
    goto err;
  }

  return umem;

err:
  pf_xdp_umem_destroy(umem);
  return NULL;
}

/* **************************************************** */

static int pf_xdp_xsk_configure(struct pf_xdp_handle *handle, struct pf_xdp_rx_queue *rxq, struct pf_xdp_tx_queue *txq, int ring_size) {
  struct xsk_socket_config cfg;
  int ret = 0;
  int reserve_size;

  rxq->umem = pf_xdp_umem_configure(handle);

  if (rxq->umem == NULL)
    return -ENOMEM;

  cfg.rx_size = ring_size;
  cfg.tx_size = ring_size;
  cfg.libbpf_flags = 0;
  cfg.xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST;
  cfg.bind_flags = 0;

  //fprintf(stderr, "Creating xsk socket on dev %s queue %u\n", 
  //  handle->if_name, handle->queue_idx);

  ret = xsk_socket__create(&rxq->xsk, handle->if_name, handle->queue_idx, rxq->umem->umem, &rxq->rx, &txq->tx, &cfg);

  if (ret) {
    fprintf(stderr, "Failed to create xsk socket on dev %s queue %u: %s (%d)\n", 
      handle->if_name, handle->queue_idx, strerror(errno), ret);
    goto err;
  }

  txq->umem = rxq->umem;
  txq->xsk = rxq->xsk;

  reserve_size = AF_XDP_DEV_NUM_DESC/2;

  ret = pf_xdp_refill_queue(rxq->umem, reserve_size);

  if (ret) {
    xsk_socket__delete(rxq->xsk);
    fprintf(stderr, "Failed to refill queue\n");
    goto err;
  }

  return 0;

err:
  pf_xdp_umem_destroy(rxq->umem);

  return ret;
}

/* **************************************************** */

static void pf_xdp_queue_reset(struct pf_xdp_handle *handle, u_int16_t queue_idx) {
  memset(&handle->rx_queue, 0, sizeof(struct pf_xdp_rx_queue));
  memset(&handle->rx_queue, 0, sizeof(struct pf_xdp_tx_queue));
  handle->rx_queue.queue_idx = queue_idx;
  handle->tx_queue.queue_idx = queue_idx;
}

/* **************************************************** */

static int pf_xdp_eth_rx_queue_setup(struct pf_xdp_handle *handle, u_int16_t queue_id, u_int16_t nb_rx_desc) {
  int ret;

  /* Cleanup XDP in case we didn't shutdown gracefully.. 
   * Note: doing this for the first queue only (this assumes
   * that the application is opening queues in order) */
  if (queue_id == 0) {
    pf_xdp_remove_xdp_program(handle);
  }

  pf_xdp_queue_reset(handle, queue_id);

  if (pf_xdp_xsk_configure(handle, &handle->rx_queue, &handle->tx_queue, nb_rx_desc)) {
    fprintf(stderr, "Failed to configure xdp socket\n");
    ret = -EINVAL;
    goto err;
  }

  handle->umem = handle->rx_queue.umem;

  return 0;

err:
  pf_xdp_queue_reset(handle, queue_id);
  return ret;
}

/* **************************************************** */

static void pf_xdp_dev_change_flags(char *if_name, u_int32_t flags, u_int32_t mask) {
  struct ifreq ifr;
  int s;

  s = socket(PF_INET, SOCK_DGRAM, 0);

  if (s < 0)
    return;

  strncpy(ifr.ifr_name, if_name, IFNAMSIZ);

  if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0)
    return;

  ifr.ifr_flags &= mask;
  ifr.ifr_flags |= flags;

  ioctl(s, SIOCSIFFLAGS, &ifr);

  close(s);
}

/* **************************************************** */

static void af_xdp_dev_promiscuous_enable(char *if_name) {
  pf_xdp_dev_change_flags(if_name, IFF_PROMISC, ~0);
}

/* **************************************************** */

static void pf_xdp_dev_promiscuous_disable(char *if_name) {
  pf_xdp_dev_change_flags(if_name, 0, ~IFF_PROMISC);
}

/* **************************************************** */

int pfring_mod_af_xdp_get_bound_device_address(pfring *ring, u_char mac_address[6]) {
  struct pf_xdp_handle *handle = (struct pf_xdp_handle *) ring->priv_data;

  memcpy(mac_address, &handle->eth_addr, ETHER_ADDR_LEN);

  return 0;
}

/* **************************************************** */

int pfring_mod_af_xdp_get_bound_device_ifindex(pfring *ring, int *if_index) {
  struct pf_xdp_handle *handle = (struct pf_xdp_handle *) ring->priv_data;

  *if_index = handle->if_index;

  return 0;
}

/* **************************************************** */

u_int8_t pfring_mod_af_xdp_get_num_rx_channels(pfring *ring) {
  char path[256];
  FILE *proc_net_pfr;
  u_int8_t n = 1;

  snprintf(path, sizeof(path), "/proc/net/pf_ring/dev/%s/info", ring->device_name);
  proc_net_pfr = fopen(path, "r");
  if (proc_net_pfr != NULL) {
    while(fgets(path, sizeof(path), proc_net_pfr) != NULL) {
      char *p = &path[0];
      const char *str_rx_queues = "RX Queues:";
      if (!strncmp(p, str_rx_queues, strlen(str_rx_queues))) {
        p += strlen(str_rx_queues);
        while (*p == ' ' && *p != '0') p++;
        n = atoi(p);
        break;
      }
    }
    fclose(proc_net_pfr);
  }

  return n;
}

/* **************************************************** */

int pfring_mod_af_xdp_set_direction(pfring *ring, packet_direction direction) {
  if (direction != rx_only_direction)
    return -1;

  return pfring_mod_set_direction(ring, direction);
}

/* **************************************************** */

int pfring_mod_af_xdp_enable_ring(pfring *ring) {
  int rc = -1;

  rc = pfring_mod_enable_ring(ring);

  if (rc < 0)
    goto error;

  if (ring->mode != send_only_mode) {
    // RX initialization
  }

  if (ring->mode != recv_only_mode) {
    // TX initialization
  }

  return 0;

 error:
  return rc;
}

/* **************************************************** */

void pfring_mod_af_xdp_close(pfring *ring) {
  struct pf_xdp_handle *handle = (struct pf_xdp_handle *) ring->priv_data;

  if (handle) {
    if (ring->promisc)
      pf_xdp_dev_promiscuous_disable(handle->if_name);
    pf_xdp_dev_close(handle);
    free(handle);
    ring->priv_data = NULL;
  }

  close(ring->fd);
}

/* **************************************************** */

int pfring_mod_af_xdp_open(pfring *ring) {
  struct pf_xdp_handle *handle;
  int channel_id = 0;
  struct ifreq ifr;
  int sock, rc;
  char *at;

  ring->enable_ring = pfring_mod_af_xdp_enable_ring;
  ring->close = pfring_mod_af_xdp_close;
  ring->stats = pfring_mod_af_xdp_stats;
  ring->recv  = pfring_mod_af_xdp_recv;
  ring->poll = pfring_mod_af_xdp_poll;
  ring->is_pkt_available = pfring_mod_af_xdp_is_pkt_available;
  ring->send  = pfring_mod_af_xdp_send;
  ring->set_direction = pfring_mod_af_xdp_set_direction;
  ring->get_bound_device_address = pfring_mod_af_xdp_get_bound_device_address;
  ring->get_bound_device_ifindex = pfring_mod_af_xdp_get_bound_device_ifindex;
  ring->get_selectable_fd = pfring_mod_af_xdp_get_selectable_fd;
  ring->get_num_rx_channels = pfring_mod_af_xdp_get_num_rx_channels;

  ring->set_socket_mode = pfring_mod_set_socket_mode;
  ring->get_interface_speed = pfring_mod_get_interface_speed;
  ring->set_poll_duration = pfring_mod_set_poll_duration;
  ring->set_application_name = pfring_mod_set_application_name;
  ring->set_application_stats = pfring_mod_set_application_stats;
  ring->get_appl_stats_file_name = pfring_mod_get_appl_stats_file_name;
  ring->get_ring_id = pfring_mod_get_ring_id;
  ring->version = pfring_mod_version;
  ring->get_device_ifindex = pfring_mod_get_device_ifindex;
  ring->set_virtual_device = pfring_mod_set_virtual_device;
  ring->add_hw_rule = pfring_hw_ft_add_hw_rule;
  ring->remove_hw_rule = pfring_hw_ft_remove_hw_rule;
  ring->loopback_test = pfring_mod_loopback_test;
  ring->disable_ring = pfring_mod_disable_ring;
  ring->shutdown = pfring_mod_shutdown;

  ring->direction = rx_only_direction;
  ring->poll_duration = DEFAULT_POLL_DURATION;

  /* ***************************************** */

  ring->fd = socket(PF_RING, SOCK_RAW, htons(ETH_P_ALL));

  if (ring->fd < 0) {
    rc = ring->fd;
    goto error;
  }

  /* Syntax: ethX@1 */
  at = strchr(ring->device_name, '@');
  if (at != NULL) {
    at[0] = '\0';
    channel_id = atoi(&at[1]);
    if (channel_id >= AF_XDP_DEV_MAX_QUEUES) {
      rc = -1;
      goto close_fd;
    }
  }

  if ((handle = calloc(1, sizeof(struct pf_xdp_handle))) == NULL) {
    rc = -1;
    goto close_fd;
  }

  strncpy(handle->if_name, ring->device_name, IFNAMSIZ);
  handle->queue_idx = channel_id;

  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

  if (sock < 0) {
    fprintf(stderr, "Failed to get interface info\n");
    rc = -1;
    goto close_fd;
  }

  strncpy(ifr.ifr_name, handle->if_name, IFNAMSIZ);

  if (ioctl(sock, SIOCGIFINDEX, &ifr)) {
    fprintf(stderr, "Failed to get interface ifindex\n");
    rc = -1;
    close(sock);
    goto close_fd;
  }

  handle->if_index = ifr.ifr_ifindex;

  if (ioctl(sock, SIOCGIFHWADDR, &ifr)) {
    fprintf(stderr, "Failed to get interface address\n");
    rc = -1;
    close(sock);
    goto close_fd;
  }

  memcpy(&handle->eth_addr, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);

  close(sock);

  rc = pf_xdp_eth_rx_queue_setup(handle, handle->queue_idx, AF_XDP_DEV_NUM_BUFFERS);

  if (rc < 0) {
    fprintf(stderr, "Failed to setup queue\n");
    goto free_handle;
  }

  ring->priv_data = handle;

  pfring_enable_hw_timestamp(ring, ring->device_name, ring->hw_ts.enable_hw_timestamp ? 1 : 0, 0);

  pfring_set_filtering_mode(ring, hardware_only);
  pfring_hw_ft_init(ring);

  if (ring->promisc)
    af_xdp_dev_promiscuous_enable(handle->if_name);  

  errno = 0;

  return 0;

 free_handle:
  free(handle);
  ring->priv_data = NULL;

 close_fd:
  close(ring->fd);

 error:
  return rc;
}

/* **************************************************** */

