/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#include <unistd.h>
#include <uv.h>

#include <isc/atomic.h>
#include <isc/buffer.h>
#include <isc/condition.h>
#include <isc/magic.h>
#include <isc/mem.h>
#include <isc/netmgr.h>
#include <isc/random.h>
#include <isc/refcount.h>
#include <isc/region.h>
#include <isc/result.h>
#include <isc/sockaddr.h>
#include <isc/thread.h>
#include <isc/util.h>

#include "uv-compat.h"
#include "netmgr-int.h"

static isc_result_t
udp_send_direct(isc_nmsocket_t *sock, isc__nm_uvreq_t *req,
		isc_sockaddr_t *peer);

static void
udp_recv_cb(uv_udp_t *handle, ssize_t nrecv, const uv_buf_t *buf,
	    const struct sockaddr *addr, unsigned flags);

static void
udp_send_cb(uv_udp_send_t *req, int status);

isc_result_t
isc_nm_listenudp(isc_nm_t *mgr, isc_nmiface_t *iface,
		 isc_nm_recv_cb_t cb, void *cbarg,
		 size_t extrahandlesize, isc_nmsocket_t **sockp)
{
	isc_nmsocket_t *nsock = NULL;

	REQUIRE(VALID_NM(mgr));

	/*
	 * We are creating mgr->nworkers duplicated sockets, one
	 * socket for each worker thread.
	 */
	nsock = isc_mem_get(mgr->mctx, sizeof(isc_nmsocket_t));
	isc__nmsocket_init(nsock, mgr, isc_nm_udplistener, iface);
	nsock->nchildren = mgr->nworkers;
	atomic_init(&nsock->rchildren, mgr->nworkers);
	nsock->children = isc_mem_get(mgr->mctx,
				      mgr->nworkers * sizeof(*nsock));
	memset(nsock->children, 0, mgr->nworkers * sizeof(*nsock));

	INSIST(nsock->rcb.recv == NULL && nsock->rcbarg == NULL);
	nsock->rcb.recv = cb;
	nsock->rcbarg = cbarg;
	nsock->extrahandlesize = extrahandlesize;

	for (size_t i = 0; i < mgr->nworkers; i++) {
		uint16_t family = iface->addr.type.sa.sa_family;
		int res;

		isc__netievent_udplisten_t *ievent = NULL;
		isc_nmsocket_t *csock = &nsock->children[i];

		isc__nmsocket_init(csock, mgr, isc_nm_udpsocket, iface);
		csock->parent = nsock;
		csock->tid = i;
		csock->extrahandlesize = extrahandlesize;

		INSIST(csock->rcb.recv == NULL && csock->rcbarg == NULL);
		csock->rcb.recv = cb;
		csock->rcbarg = cbarg;
		csock->fd = socket(family, SOCK_DGRAM, 0);
		INSIST(csock->fd >= 0);

		/*
		 * This is SO_REUSE**** hell:
		 * On Linux SO_REUSEPORT allows multiple sockets to bind to
		 * the same host:port pair.
		 * On Windows the same thing is achieved with SO_REUSEADDR
		 */
#ifdef WIN32
		res = setsockopt(csock->fd, SOL_SOCKET, SO_REUSEADDR,
				 &(int){1}, sizeof(int));
#else
		res = setsockopt(csock->fd, SOL_SOCKET, SO_REUSEPORT,
				 &(int){1}, sizeof(int));
#endif
		RUNTIME_CHECK(res == 0);

		ievent = isc__nm_get_ievent(mgr, netievent_udplisten);
		ievent->sock = csock;
		isc__nm_enqueue_ievent(&mgr->workers[i],
				       (isc__netievent_t *) ievent);
	}

	*sockp = nsock;
	return (ISC_R_SUCCESS);
}

/*
 * handle 'udplisten' async call - start listening on a socket.
 */
void
isc__nm_async_udplisten(isc__networker_t *worker, isc__netievent_t *ev0) {
	isc__netievent_udplisten_t *ievent =
		(isc__netievent_udplisten_t *) ev0;
	isc_nmsocket_t *sock = ievent->sock;
	int r, flags = 0;

	REQUIRE(sock->type == isc_nm_udpsocket);
	REQUIRE(sock->iface != NULL);
	REQUIRE(sock->parent != NULL);

	uv_udp_init(&worker->loop, &sock->uv_handle.udp);
	uv_handle_set_data(&sock->uv_handle.handle, NULL);
	isc_nmsocket_attach(sock,
			    (isc_nmsocket_t **)&sock->uv_handle.udp.data);

	r = uv_udp_open(&sock->uv_handle.udp, sock->fd);
	if (r == 0) {
		isc__nm_incstats(sock->mgr, sock->statsindex[STATID_OPEN]);
	} else {
		isc__nm_incstats(sock->mgr,
				 sock->statsindex[STATID_OPENFAIL]);
	}

	if (sock->iface->addr.type.sa.sa_family == AF_INET6) {
		flags = UV_UDP_IPV6ONLY;
	}

	r = uv_udp_bind(&sock->uv_handle.udp,
			&sock->parent->iface->addr.type.sa, flags);
	if (r < 0) {
		isc__nm_incstats(sock->mgr,
				 sock->statsindex[STATID_BINDFAIL]);
	}

	uv_recv_buffer_size(&sock->uv_handle.handle,
			    &(int){16 * 1024 * 1024});
	uv_send_buffer_size(&sock->uv_handle.handle,
			    &(int){16 * 1024 * 1024});
	uv_udp_recv_start(&sock->uv_handle.udp, isc__nm_alloc_cb, udp_recv_cb);
}

static void
udp_close_cb(uv_handle_t *handle) {
	isc_nmsocket_t *sock = uv_handle_get_data(handle);
	atomic_store(&sock->closed, true);

	isc_nmsocket_detach((isc_nmsocket_t **)&sock->uv_handle.udp.data);
}

static void
stop_udp_child(isc_nmsocket_t *sock) {
	INSIST(sock->type == isc_nm_udpsocket);

	uv_udp_recv_stop(&sock->uv_handle.udp);
	uv_close((uv_handle_t *) &sock->uv_handle.udp, udp_close_cb);

	isc__nm_incstats(sock->mgr, sock->statsindex[STATID_CLOSE]);

	LOCK(&sock->parent->lock);
	atomic_fetch_sub(&sock->parent->rchildren, 1);
	UNLOCK(&sock->parent->lock);
	BROADCAST(&sock->parent->cond);
}

static void
stoplistening(isc_nmsocket_t *sock) {
	/*
	 * Socket is already closing; there's nothing to do.
	 */
	if (uv_is_closing((uv_handle_t *) &sock->uv_handle.udp)) {
		return;
	}

	INSIST(sock->type == isc_nm_udplistener);

	for (int i = 0; i < sock->nchildren; i++) {
		isc__netievent_udpstop_t *event = NULL;

		if (i == sock->tid) {
			stop_udp_child(&sock->children[i]);
			continue;
		}

		event = isc__nm_get_ievent(sock->mgr, netievent_udpstop);
		event->sock = &sock->children[i];
		isc__nm_enqueue_ievent(&sock->mgr->workers[i],
				       (isc__netievent_t *) event);
	}

	LOCK(&sock->lock);
	while (atomic_load_relaxed(&sock->rchildren) > 0) {
		WAIT(&sock->cond, &sock->lock);
	}
	atomic_store(&sock->closed, true);
	UNLOCK(&sock->lock);

	isc__nmsocket_prep_destroy(sock);
}

void
isc_nm_udp_stoplistening(isc_nmsocket_t *sock) {
	isc__netievent_udpstop_t *ievent = NULL;

	/* We can't be launched from network thread, we'd deadlock */
	REQUIRE(!isc__nm_in_netthread());
	REQUIRE(VALID_NMSOCK(sock));
	REQUIRE(sock->type == isc_nm_udplistener);

	/*
	 * If the manager is interlocked, re-enqueue this as an asynchronous
	 * event. Otherwise, go ahead and stop listening right away.
	 */
	if (!isc__nm_acquire_interlocked(sock->mgr)) {
		ievent = isc__nm_get_ievent(sock->mgr, netievent_udpstop);
		ievent->sock = sock;
		isc__nm_enqueue_ievent(&sock->mgr->workers[sock->tid],
				       (isc__netievent_t *) ievent);
	} else {
		stoplistening(sock);
		isc__nm_drop_interlocked(sock->mgr);
	}
}

/*
 * handle 'udpstop' async call - stop listening on a socket.
 */
void
isc__nm_async_udpstop(isc__networker_t *worker, isc__netievent_t *ev0) {
	isc__netievent_udpstop_t *ievent = (isc__netievent_udpstop_t *) ev0;
	isc_nmsocket_t *sock = ievent->sock;

	REQUIRE(sock->iface != NULL);
	UNUSED(worker);

	/*
	 * If this is a child socket, stop listening and return.
	 */
	if (sock->parent != NULL) {
		stop_udp_child(sock);
		return;
	}

	/*
	 * If network manager is paused, re-enqueue the event for later.
	 */
	if (!isc__nm_acquire_interlocked(sock->mgr)) {
		isc__netievent_udplisten_t *event = NULL;

		event = isc__nm_get_ievent(sock->mgr, netievent_udpstop);
		event->sock = sock;
		isc__nm_enqueue_ievent(&sock->mgr->workers[sock->tid],
				       (isc__netievent_t *) event);
	} else {
		stoplistening(sock);
		isc__nm_drop_interlocked(sock->mgr);
	}
}

/*
 * udp_recv_cb handles incoming UDP packet from uv.  The buffer here is
 * reused for a series of packets, so we need to allocate a new one. This
 * new one can be reused to send the response then.
 */
static void
udp_recv_cb(uv_udp_t *handle, ssize_t nrecv, const uv_buf_t *buf,
	    const struct sockaddr *addr, unsigned flags)
{
	isc_result_t result;
	isc_nmhandle_t *nmhandle = NULL;
	isc_sockaddr_t sockaddr;
	isc_sockaddr_t localaddr;
	struct sockaddr_storage laddr;
	isc_nmsocket_t *sock = uv_handle_get_data((uv_handle_t *)handle);
	isc_region_t region;
	uint32_t maxudp;

	REQUIRE(VALID_NMSOCK(sock));

	/*
	 * We can ignore the flags; currently the only one in use by libuv
	 * is UV_UDP_PARTIAL, which only occurs if the receive buffer is
	 * too small, which can't happen here.
	 */
	UNUSED(flags);

	/*
	 * If addr == NULL that's the end of stream - we can
	 * free the buffer and bail.
	 */
	if (addr == NULL) {
		isc__nm_free_uvbuf(sock, buf);
		return;
	}

	/*
	 * Simulate a firewall blocking UDP packets bigger than
	 * 'maxudp' bytes.
	 */
	maxudp = atomic_load(&sock->mgr->maxudp);
	if (maxudp != 0 && (uint32_t)nrecv > maxudp) {
		return;
	}

	result = isc_sockaddr_fromsockaddr(&sockaddr, addr);
	RUNTIME_CHECK(result == ISC_R_SUCCESS);
	uv_udp_getsockname(handle, (struct sockaddr *) &laddr,
			   &(int){sizeof(struct sockaddr_storage)});
	result = isc_sockaddr_fromsockaddr(&localaddr,
					   (struct sockaddr *) &laddr);
	RUNTIME_CHECK(result == ISC_R_SUCCESS);

	nmhandle = isc__nmhandle_get(sock, &sockaddr, &localaddr);
	region.base = (unsigned char *) buf->base;
	region.length = nrecv;

	INSIST(sock->rcb.recv != NULL);
	sock->rcb.recv(nmhandle, &region, sock->rcbarg);
	isc__nm_free_uvbuf(sock, buf);

	/*
	 * If the recv callback wants to hold on to the handle,
	 * it needs to attach to it.
	 */
	isc_nmhandle_unref(nmhandle);
}

/*
 * isc__nm_udp_send sends buf to a peer on a socket.
 * It tries to find a proper sibling/child socket so that we won't have
 * to jump to other thread.
 */
isc_result_t
isc__nm_udp_send(isc_nmhandle_t *handle, isc_region_t *region,
		 isc_nm_cb_t cb, void *cbarg)
{
	isc_nmsocket_t *psock = NULL, *rsock = NULL;
	isc_nmsocket_t *sock = handle->sock;
	isc_sockaddr_t *peer = &handle->peer;
	isc__netievent_udpsend_t *ievent = NULL;
	isc__nm_uvreq_t *uvreq = NULL;
	int ntid;
	uint32_t maxudp = atomic_load(&sock->mgr->maxudp);

	/*
	 * Simulate a firewall blocking UDP packets bigger than
	 * 'maxudp' bytes.
	 */
	if (maxudp != 0 && region->length > maxudp) {
		isc_nmhandle_unref(handle);
		return (ISC_R_SUCCESS);
	}

	if (sock->type == isc_nm_udpsocket) {
		INSIST(sock->parent != NULL);
		psock = sock->parent;
	} else if (sock->type == isc_nm_udplistener) {
		psock = sock;
	} else {
		isc_nmhandle_unref(handle);
		return (ISC_R_UNEXPECTED);
	}

	if (isc__nm_in_netthread()) {
		ntid = isc_nm_tid();
	} else {
		ntid = (int) isc_random_uniform(sock->nchildren);
	}

	rsock = &psock->children[ntid];

	uvreq = isc__nm_uvreq_get(sock->mgr, sock);
	uvreq->uvbuf.base = (char *) region->base;
	uvreq->uvbuf.len = region->length;

	uvreq->handle = handle;
	isc_nmhandle_ref(uvreq->handle);

	uvreq->cb.send = cb;
	uvreq->cbarg = cbarg;

	if (isc_nm_tid() == rsock->tid) {
		/*
		 * If we're in the same thread as the socket we can send the
		 * data directly
		 */
		return (udp_send_direct(rsock, uvreq, peer));
	} else {
		/*
		 * We need to create an event and pass it using async channel
		 */
		ievent = isc__nm_get_ievent(sock->mgr, netievent_udpsend);
		ievent->sock = rsock;
		ievent->peer = *peer;
		ievent->req = uvreq;

		isc__nm_enqueue_ievent(&sock->mgr->workers[rsock->tid],
				       (isc__netievent_t *) ievent);
		return (ISC_R_SUCCESS);
	}
}


/*
 * handle 'udpsend' async event - send a packet on the socket
 */
void
isc__nm_async_udpsend(isc__networker_t *worker, isc__netievent_t *ev0) {
	isc__netievent_udpsend_t *ievent =
		(isc__netievent_udpsend_t *) ev0;

	REQUIRE(worker->id == ievent->sock->tid);

	if (atomic_load(&ievent->sock->active)) {
		udp_send_direct(ievent->sock, ievent->req, &ievent->peer);
	} else {
		ievent->req->cb.send(ievent->req->handle,
				     ISC_R_CANCELED, ievent->req->cbarg);
		isc__nm_uvreq_put(&ievent->req, ievent->req->sock);
	}
}

/*
 * udp_send_cb - callback
 */
static void
udp_send_cb(uv_udp_send_t *req, int status) {
	isc_result_t result = ISC_R_SUCCESS;
	isc__nm_uvreq_t *uvreq = (isc__nm_uvreq_t *)req->data;

	REQUIRE(VALID_UVREQ(uvreq));
	REQUIRE(VALID_NMHANDLE(uvreq->handle));

	if (status < 0) {
		result = isc__nm_uverr2result(status);
		isc__nm_incstats(uvreq->sock->mgr,
				 uvreq->sock->statsindex[STATID_SENDFAIL]);
	}

	uvreq->cb.send(uvreq->handle, result, uvreq->cbarg);
	isc_nmhandle_unref(uvreq->handle);
	isc__nm_uvreq_put(&uvreq, uvreq->sock);
}

/*
 * udp_send_direct sends buf to a peer on a socket. Sock has to be in
 * the same thread as the callee.
 */
static isc_result_t
udp_send_direct(isc_nmsocket_t *sock, isc__nm_uvreq_t *req,
		isc_sockaddr_t *peer)
{
	int rv;

	REQUIRE(sock->tid == isc_nm_tid());
	REQUIRE(sock->type == isc_nm_udpsocket);

	isc_nmhandle_ref(req->handle);
	rv = uv_udp_send(&req->uv_req.udp_send,
			 &sock->uv_handle.udp, &req->uvbuf, 1,
			 &peer->type.sa, udp_send_cb);
	if (rv < 0) {
		isc__nm_incstats(req->sock->mgr,
				 req->sock->statsindex[STATID_SENDFAIL]);
		return (isc__nm_uverr2result(rv));
	}

	return (ISC_R_SUCCESS);
}
