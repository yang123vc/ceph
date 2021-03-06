// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 XSKY <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <poll.h>

#include "include/str_list.h"
#include "RDMAStack.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << "RDMAStack "

static Infiniband* global_infiniband;

RDMADispatcher::~RDMADispatcher()
{
  done = true;
  t.join();
  ldout(cct, 20) << __func__ << " ing..." << dendl;
  auto i = qp_conns.begin();
  while (i != qp_conns.end()) {
    delete i->second.first;
    ++i;
  }

  while (!dead_queue_pairs.empty()) {
    delete dead_queue_pairs.back();
    dead_queue_pairs.pop_back();
  }

  rx_cc->ack_events();
  delete rx_cq;
  delete rx_cc;
  delete async_handler;
}

RDMADispatcher::RDMADispatcher(CephContext* c, Infiniband* i, RDMAStack* s)
  : cct(c), ib(i), async_handler(new C_handle_cq_async(this)), lock("RDMADispatcher::lock"),
  w_lock("RDMADispatcher::for worker pending list"), qp_lock("for qp lock"), stack(s)
{
  rx_cc = ib->create_comp_channel(c);
  assert(rx_cc);
  rx_cq = ib->create_comp_queue(c, rx_cc);
  assert(rx_cq);
  t = std::thread(&RDMADispatcher::polling, this);
  cct->register_fork_watcher(this);
}

void RDMADispatcher::handle_async_event()
{
  ldout(cct, 20) << __func__ << dendl;
  while (1) {
    ibv_async_event async_event;
    if (ibv_get_async_event(ib->get_device()->ctxt, &async_event)) {
      if (errno != EAGAIN)
       lderr(cct) << __func__ << " ibv_get_async_event failed. (errno=" << errno
                  << " " << cpp_strerror(errno) << ")" << dendl;
      return;
    }
    // FIXME: Currently we must ensure no other factor make QP in ERROR state,
    // otherwise this qp can't be deleted in current cleanup flow.
    if (async_event.event_type == IBV_EVENT_QP_LAST_WQE_REACHED) {
      uint64_t qpn = async_event.element.qp->qp_num;
      ldout(cct, 10) << __func__ << " event associated qp=" << async_event.element.qp
                     << " evt: " << ibv_event_type_str(async_event.event_type) << dendl;
      RDMAConnectedSocketImpl *conn = get_conn_by_qp(qpn);
      if (!conn) {
        ldout(cct, 1) << __func__ << " missing qp_num=" << qpn << " discard event" << dendl;
      } else {
        ldout(cct, 1) << __func__ << " it's not forwardly stopped by us, reenable=" << conn << dendl;
        conn->fault();
        erase_qpn(qpn);
      }
    } else {
      ldout(cct, 1) << __func__ << " ibv_get_async_event: dev=" << ib->get_device()->ctxt
                    << " evt: " << ibv_event_type_str(async_event.event_type)
                    << dendl;
    }
    ibv_ack_async_event(&async_event);
  }
}

void RDMADispatcher::polling()
{
  static int MAX_COMPLETIONS = 32;
  ibv_wc wc[MAX_COMPLETIONS];

  std::map<RDMAConnectedSocketImpl*, std::vector<ibv_wc> > polled;
  std::vector<ibv_wc> tx_cqe;
  RDMAWorker* worker;
  ldout(cct, 20) << __func__ << " going to poll rx cq:" << rx_cq << dendl;
  RDMAConnectedSocketImpl *conn = nullptr;
  utime_t last_inactive = ceph_clock_now();
  bool rearmed = false;

  while (true) {
    int n = rx_cq->poll_cq(MAX_COMPLETIONS, wc);
    if (!n) {
      // NOTE: Has TX just transitioned to idle? We should do it when idle!
      // It's now safe to delete queue pairs (see comment by declaration
      // for dead_queue_pairs).
      // Additionally, don't delete qp while outstanding_buffers isn't empty,
      // because we need to check qp's state before sending
      if (!inflight.load()) {
        Mutex::Locker l(lock); // FIXME reuse dead qp because creating one qp costs 1 ms
        while (!dead_queue_pairs.empty()) {
          ldout(cct, 10) << __func__ << " finally delete qp=" << dead_queue_pairs.back() << dendl;
          delete dead_queue_pairs.back();
          dead_queue_pairs.pop_back();
        }
      }
      // handle_async_event();
      if (done)
        break;

      if ((ceph_clock_now() - last_inactive).to_nsec() / 1000 > cct->_conf->ms_async_rdma_polling_us) {
        if (!rearmed) {
          // Clean up cq events after rearm notify ensure no new incoming event
          // arrived between polling and rearm
          rx_cq->rearm_notify();
          rearmed = true;
          continue;
        }

        struct pollfd channel_poll;
        channel_poll.fd = rx_cc->get_fd();
        channel_poll.events = POLLIN | POLLERR | POLLNVAL | POLLHUP;
        channel_poll.revents = 0;
        int r = 0;
        while (!done && r == 0) {
          r = poll(&channel_poll, 1, 1);
          if (r < 0) {
            r = -errno;
            lderr(cct) << __func__ << " poll failed " << r << dendl;
            ceph_abort();
          }
        }
        if (r > 0 && rx_cc->get_cq_event())
          ldout(cct, 20) << __func__ << " got cq event." << dendl;
        last_inactive = ceph_clock_now();
        rearmed = false;
      }
      continue;
    }

    ldout(cct, 20) << __func__ << " pool completion queue got " << n
                   << " responses."<< dendl;
    Mutex::Locker l(lock);//make sure connected socket alive when pass wc
    for (int i = 0; i < n; ++i) {
      ibv_wc* response = &wc[i];
      Chunk* chunk = reinterpret_cast<Chunk *>(response->wr_id);

      if (response->status != IBV_WC_SUCCESS) {
        ldout(cct, 1) << __func__ << " work request returned error for buffer(" << chunk
                      << ") status(" << response->status << ":"
                      << ib->wc_status_to_string(response->status) << dendl;
        ib->recall_chunk(chunk);
        conn = get_conn_lockless(response->qp_num);
        if (conn && conn->is_connected())
          conn->fault();
        notify_pending_workers();
        continue;
      }

      if (wc[i].opcode == IBV_WC_SEND) {
        tx_cqe.push_back(wc[i]);
        ldout(cct, 25) << " got a tx cqe, bytes:" << wc[i].byte_len << dendl; 
        continue;
      }
      ldout(cct, 25) << __func__ << " got chunk=" << chunk << " bytes:" << response->byte_len << " opcode:" << response->opcode << dendl;
      conn = get_conn_lockless(response->qp_num);
      if (!conn) {
        int ret = ib->recall_chunk(chunk);
        ldout(cct, 1) << __func__ << " csi with qpn " << response->qp_num << " may be dead. chunk " << chunk << " will be back ? " << ret << dendl;
        continue;
      }
      polled[conn].push_back(*response);
    }
    for (auto &&i : polled)
      i.first->pass_wc(std::move(i.second));
    polled.clear();
    if (!tx_cqe.empty()) {
      worker = get_worker_from_list();
      if (worker == nullptr)
        worker = dynamic_cast<RDMAWorker*>(stack->get_worker());
      worker->pass_wc(std::move(tx_cqe));
      tx_cqe.clear();
    }
  }
}

void RDMADispatcher::notify_pending_workers() {
    Mutex::Locker l(w_lock);
    if (pending_workers.empty())
      return ;
    pending_workers.front()->pass_wc(std::move(vector<ibv_wc>()));
    pending_workers.pop_front();
}

int RDMADispatcher::register_qp(QueuePair *qp, RDMAConnectedSocketImpl* csi)
{
  int fd = eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK);
  assert(fd >= 0);
  Mutex::Locker l(lock);
  assert(!qp_conns.count(qp->get_local_qp_number()));
  qp_conns[qp->get_local_qp_number()] = std::make_pair(qp, csi);
  return fd;
}

int RDMADispatcher::register_worker(RDMAWorker* w)
{
  int fd = eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK);
  assert(fd >= 0);
  Mutex::Locker l(w_lock);
  workers[w] = fd;
  return fd;
}

void RDMADispatcher::pending_buffers(RDMAWorker* w)
{
  Mutex::Locker l(w_lock);
  pending_workers.push_back(w);
}

RDMAWorker* RDMADispatcher::get_worker_from_list()
{
  Mutex::Locker l(w_lock);
  if (pending_workers.empty())
    return nullptr;
  else {
    RDMAWorker* w = pending_workers.front();
    pending_workers.pop_front();
    return w;
  }
}

RDMAConnectedSocketImpl* RDMADispatcher::get_conn_by_qp(uint32_t qp)
{
  Mutex::Locker l(lock);
  auto it = qp_conns.find(qp);
  if (it == qp_conns.end())
    return nullptr;
  if (it->second.first->is_dead())
    return nullptr;
  return it->second.second;
}

RDMAConnectedSocketImpl* RDMADispatcher::get_conn_lockless(uint32_t qp)
{
  auto it = qp_conns.find(qp);
  if (it == qp_conns.end())
    return nullptr;
  if (it->second.first->is_dead())
    return nullptr;
  return it->second.second;
}

void RDMADispatcher::erase_qpn(uint32_t qpn)
{
  Mutex::Locker l(lock);
  auto it = qp_conns.find(qpn);
  if (it == qp_conns.end())
    return ;
  dead_queue_pairs.push_back(it->second.first);
  qp_conns.erase(it);
}

void RDMADispatcher::handle_pre_fork()
{
  done = true;
  t.join();
  done = false;
}

void RDMADispatcher::handle_post_fork()
{
  t = std::thread(&RDMADispatcher::polling, this);
}


RDMAWorker::RDMAWorker(CephContext *c, unsigned i)
  : Worker(c, i), stack(nullptr), infiniband(NULL),
    tx_handler(new C_handle_cq_tx(this)), memory_manager(NULL), lock("RDMAWorker::lock"), pended(false)
{
}

RDMAWorker::~RDMAWorker()
{
  delete tx_handler;
  if (notify_fd >= 0)
    ::close(notify_fd);
}

void RDMAWorker::initialize()
{
  if (!dispatcher) {
    dispatcher = stack->get_dispatcher();
    notify_fd = dispatcher->register_worker(this);
    center.create_file_event(notify_fd, EVENT_READABLE, tx_handler);
    memory_manager = infiniband->get_memory_manager();
  }
}

void RDMAWorker::notify()
{
  uint64_t i = 1;
  assert(write(notify_fd, &i, sizeof(i)) == sizeof(i));
}

void RDMAWorker::pass_wc(std::vector<ibv_wc> &&v)
{
  Mutex::Locker l(lock);
  if (wc.empty())
    wc = std::move(v);
  else
    wc.insert(wc.end(), v.begin(), v.end());
  notify();
}

void RDMAWorker::add_pending_conn(RDMAConnectedSocketImpl* o)
{
  pending_sent_conns.push_back(o);
  if (!pended) {
    dispatcher->pending_buffers(this);
    pended = true;
  }
}

void RDMAWorker::get_wc(std::vector<ibv_wc> &w)
{
  Mutex::Locker l(lock);
  if (wc.empty())
    return ;
  w.swap(wc);
}

int RDMAWorker::listen(entity_addr_t &sa, const SocketOptions &opt,ServerSocket *sock)
{
  auto p = new RDMAServerSocketImpl(cct, infiniband, get_stack()->get_dispatcher(), this, sa);
  int r = p->listen(sa, opt);
  if (r < 0) {
    delete p;
    return r;
  }

  *sock = ServerSocket(std::unique_ptr<ServerSocketImpl>(p));
  return 0;
}

int RDMAWorker::connect(const entity_addr_t &addr, const SocketOptions &opts, ConnectedSocket *socket)
{
  RDMAConnectedSocketImpl* p = new RDMAConnectedSocketImpl(cct, infiniband, get_stack()->get_dispatcher(), this);
  int r = p->try_connect(addr, opts);

  if (r < 0) {
    ldout(cct, 1) << __func__ << " try connecting failed." << dendl;
    return r;
  }
  std::unique_ptr<RDMAConnectedSocketImpl> csi(p);
  *socket = ConnectedSocket(std::move(csi));
  return 0;
}

int RDMAWorker::reserve_message_buffer(RDMAConnectedSocketImpl *o, std::vector<Chunk*> &c, size_t bytes)
{
  int r = infiniband->get_tx_buffers(c, bytes);
  if (r > 0) {
    stack->get_dispatcher()->inflight += c.size();
    ldout(cct, 30) << __func__ << " reserve " << c.size() << " chunks, inflight " << stack->get_dispatcher()->inflight << dendl;
    return r;
  }
  assert(r == 0);

  if (pending_sent_conns.back() != o)
    pending_sent_conns.push_back(o);
  dispatcher->pending_buffers(this);
  return r;
}

/**
 * Add the given Chunks to the given free queue.
 *
 * \param[in] chunks
 *      The Chunks to enqueue.
 * \return
 *      0 if success or -1 for failure
 */
int RDMAWorker::post_tx_buffer(std::vector<Chunk*> &chunks)
{
  if (chunks.empty())
    return 0;

  stack->get_dispatcher()->inflight -= chunks.size();
  memory_manager->return_tx(chunks);
  ldout(cct, 30) << __func__ << " release " << chunks.size() << " chunks, inflight " << stack->get_dispatcher()->inflight << dendl;

  pended = false;
  std::set<RDMAConnectedSocketImpl*> done;
  while (!pending_sent_conns.empty()) {
    RDMAConnectedSocketImpl *o = pending_sent_conns.front();
    if (done.count(o) == 0) {
      done.insert(o);
    } else {
      pending_sent_conns.pop_front();
      continue;
    }
    ssize_t r = o->submit(false);
    ldout(cct, 20) << __func__ << " sent pending bl socket=" << o << " r=" << r << dendl;
    if (r < 0) {
      if (r == -EAGAIN)
        break;
      o->fault();
    }
    pending_sent_conns.pop_front();
  }
  return 0;
}

void RDMAWorker::handle_tx_event()
{
  std::vector<Chunk*> tx_chunks;
  std::vector<ibv_wc> cqe;
  get_wc(cqe);

  for (size_t i = 0; i < cqe.size(); ++i) {
    ibv_wc* response = &cqe[i];
    Chunk* chunk = reinterpret_cast<Chunk *>(response->wr_id);
    ldout(cct, 25) << __func__ << " QP: " << response->qp_num << " len: " << response->byte_len << " , addr:" << chunk << " " << infiniband->wc_status_to_string(response->status) << dendl;

    if (response->status != IBV_WC_SUCCESS) {
      if (response->status == IBV_WC_RETRY_EXC_ERR) {
        ldout(cct, 1) << __func__ << " connection between server and client not working. Disconnect this now" << dendl;
      } else if (response->status == IBV_WC_WR_FLUSH_ERR) {
        ldout(cct, 1) << __func__ << " Work Request Flushed Error: this connection's qp="
                      << response->qp_num << " should be down while this WR=" << response->wr_id
                      << " still in flight." << dendl;
      } else {
        ldout(cct, 1) << __func__ << " send work request returned error for buffer("
                      << response->wr_id << ") status(" << response->status << "): "
                      << infiniband->wc_status_to_string(response->status) << dendl;
      }
      RDMAConnectedSocketImpl *conn = stack->get_dispatcher()->get_conn_by_qp(response->qp_num);
      if (conn) {
        ldout(cct, 25) << __func__ << " qp state is : " << conn->get_qp_state() << dendl;//wangzhi
        conn->fault();
      } else {
        ldout(cct, 1) << __func__ << " missing qp_num=" << response->qp_num << " discard event" << dendl;
      }
    }

    //assert(memory_manager->is_tx_chunk(chunk));
    if (memory_manager->is_tx_chunk(chunk)) {
      tx_chunks.push_back(chunk);
    } else {
      ldout(cct, 1) << __func__ << " a outter chunk: " << chunk << dendl;//fin
    }
  }

  post_tx_buffer(tx_chunks);

  ldout(cct, 20) << __func__ << " give back " << tx_chunks.size() << " in Worker " << this << dendl;
  dispatcher->notify_pending_workers();
}


RDMAStack::RDMAStack(CephContext *cct, const string &t): NetworkStack(cct, t)
{
  if (!global_infiniband)
    global_infiniband = new Infiniband(
      cct, cct->_conf->ms_async_rdma_device_name, cct->_conf->ms_async_rdma_port_num);
  ldout(cct, 20) << __func__ << " constructing RDMAStack..." << dendl;
  dispatcher = new RDMADispatcher(cct, global_infiniband, this);
  unsigned num = get_num_worker();
  for (unsigned i = 0; i < num; ++i) {
    RDMAWorker* w = dynamic_cast<RDMAWorker*>(get_worker(i));
    w->set_ib(global_infiniband);
    w->set_stack(this);
  }
  ldout(cct, 20) << " creating RDMAStack:" << this << " with dispatcher:" << dispatcher << dendl;
}

RDMAStack::~RDMAStack()
{
  delete dispatcher;
}

void RDMAStack::spawn_worker(unsigned i, std::function<void ()> &&func)
{
  threads.resize(i+1);
  threads[i] = std::move(std::thread(func));
}

void RDMAStack::join_worker(unsigned i)
{
  assert(threads.size() > i && threads[i].joinable());
  threads[i].join();
}
