//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"
#include "platform/posix/posix_aio.h"
#include "platform/posix/posix_pollq.h"

#ifdef PLATFORM_POSIX_PIPEDESC

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>


// nni_posix_pipedesc is a descriptor kept one per transport pipe (i.e. open
// file descriptor for TCP socket, etc.)  This contains the list of pending
// aios for that underlying socket, as well as the socket itself.
struct nni_posix_pipedesc {
	nni_posix_pollq *	pq;
	int			fd;
	nni_list		readq;
	nni_list		writeq;
	nni_posix_pollq_node	node;
	nni_mtx			mtx;
};


static void
nni_posix_pipedesc_finish(nni_aio *aio, int rv)
{
	nni_aio_list_remove(aio);
	nni_aio_finish(aio, rv, aio->a_count);
}


static void
nni_posix_pipedesc_dowrite(nni_posix_pipedesc *pd)
{
	int n;
	int rv;
	int i;
	struct iovec iovec[4];
	struct iovec *iovp;
	nni_aio *aio;

	while ((aio = nni_list_first(&pd->writeq)) != NULL) {
		for (i = 0; i < aio->a_niov; i++) {
			iovec[i].iov_len = aio->a_iov[i].iov_len;
			iovec[i].iov_base = aio->a_iov[i].iov_buf;
		}
		iovp = &iovec[0];
		rv = 0;

		n = writev(pd->fd, iovp, aio->a_niov);
		if (n < 0) {
			if ((errno == EAGAIN) || (errno == EINTR)) {
				// Can't write more right now.  We're done
				// on this fd for now.
				return;
			}
			rv = nni_plat_errno(errno);

			nni_posix_pipedesc_finish(aio, rv);
			return;
		}

		aio->a_count += n;

		while (n > 0) {
			// If we didn't write the first full iov,
			// then we're done for now.  Record progress
			// and return to caller.
			if (n < aio->a_iov[0].iov_len) {
				aio->a_iov[0].iov_buf += n;
				aio->a_iov[0].iov_len -= n;
				return;
			}

			// We consumed the full iovec, so just move the
			// remaininng ones up, and decrement count handled.
			n -= aio->a_iov[0].iov_len;
			for (i = 1; i < aio->a_niov; i++) {
				aio->a_iov[i-1] = aio->a_iov[i];
			}
			NNI_ASSERT(aio->a_niov > 0);
			aio->a_niov--;
		}

		// We completed the entire operation on this aioq.
		nni_posix_pipedesc_finish(aio, 0);

		// Go back to start of loop to see if there is another
		// aioq ready for us to process.
	}
}


static void
nni_posix_pipedesc_doread(nni_posix_pipedesc *pd)
{
	int n;
	int rv;
	int i;
	struct iovec iovec[4];
	struct iovec *iovp;
	nni_aio *aio;

	while ((aio = nni_list_first(&pd->readq)) != NULL) {
		for (i = 0; i < aio->a_niov; i++) {
			iovec[i].iov_len = aio->a_iov[i].iov_len;
			iovec[i].iov_base = aio->a_iov[i].iov_buf;
		}
		iovp = &iovec[0];
		rv = 0;

		n = readv(pd->fd, iovp, aio->a_niov);
		if (n < 0) {
			if ((errno == EAGAIN) || (errno == EINTR)) {
				// Can't write more right now.  We're done
				// on this fd for now.
				return;
			}
			rv = nni_plat_errno(errno);

			nni_posix_pipedesc_finish(aio, rv);
			return;
		}

		if (n == 0) {
			// No bytes indicates a closed descriptor.
			nni_posix_pipedesc_finish(aio, NNG_ECLOSED);
			return;
		}

		aio->a_count += n;

		while (n > 0) {
			// If we didn't write the first full iov,
			// then we're done for now.  Record progress
			// and return to caller.
			if (n < aio->a_iov[0].iov_len) {
				aio->a_iov[0].iov_buf += n;
				aio->a_iov[0].iov_len -= n;
				return;
			}

			// We consumed the full iovec, so just move the
			// remaininng ones up, and decrement count handled.
			n -= aio->a_iov[0].iov_len;
			for (i = 1; i < aio->a_niov; i++) {
				aio->a_iov[i-1] = aio->a_iov[i];
			}
			NNI_ASSERT(aio->a_niov > 0);
			aio->a_niov--;
		}

		// We completed the entire operation on this aioq.
		nni_posix_pipedesc_finish(aio, 0);

		// Go back to start of loop to see if there is another
		// aioq ready for us to process.
	}
}


static void
nni_posix_pipedesc_doclose(nni_posix_pipedesc *pd)
{
	nni_aio *aio;

	if (pd->fd != -1) {
		// Let any peer know we are closing.
		(void) shutdown(pd->fd, SHUT_RDWR);
	}
	while ((aio = nni_list_first(&pd->readq)) != NULL) {
		nni_posix_pipedesc_finish(aio, NNG_ECLOSED);
	}
	while ((aio = nni_list_first(&pd->writeq)) != NULL) {
		nni_posix_pipedesc_finish(aio, NNG_ECLOSED);
	}
}


static void
nni_posix_pipedesc_cb(void *arg)
{
	nni_posix_pipedesc *pd = arg;

	nni_mtx_lock(&pd->mtx);
	if (pd->node.revents & POLLIN) {
		nni_posix_pipedesc_doread(pd);
	}
	if (pd->node.revents & POLLOUT) {
		nni_posix_pipedesc_dowrite(pd);
	}
	if (pd->node.revents & (POLLHUP|POLLERR|POLLNVAL)) {
		nni_posix_pipedesc_doclose(pd);
	}

	pd->node.revents = 0;
	pd->node.events = 0;

	if (!nni_list_empty(&pd->writeq)) {
		pd->node.events |= POLLOUT;
	}
	if (!nni_list_empty(&pd->readq)) {
		pd->node.events |= POLLIN;
	}

	// If we still have uncompleted operations, resubmit us.
	if (pd->node.events != 0) {
		nni_posix_pollq_submit(pd->pq, &pd->node);
	}
	nni_mtx_unlock(&pd->mtx);
}


void
nni_posix_pipedesc_close(nni_posix_pipedesc *pd)
{
	nni_posix_pollq_cancel(pd->pq, &pd->node);

	nni_mtx_lock(&pd->mtx);
	nni_posix_pipedesc_doclose(pd);
	nni_mtx_unlock(&pd->mtx);
}


static void
nni_posix_pipedesc_cancel(nni_aio *aio)
{
	nni_posix_pipedesc *pd = aio->a_prov_data;

	nni_mtx_lock(&pd->mtx);
	nni_aio_list_remove(aio);
	nni_mtx_unlock(&pd->mtx);
}


void
nni_posix_pipedesc_recv(nni_posix_pipedesc *pd, nni_aio *aio)
{
	int rv;

	nni_mtx_lock(&pd->mtx);
	if ((rv = nni_aio_start(aio, nni_posix_pipedesc_cancel, pd)) != 0) {
		nni_mtx_unlock(&pd->mtx);
		return;
	}
	if (pd->fd < 0) {
		nni_posix_pipedesc_finish(aio, NNG_ECLOSED);
		nni_mtx_unlock(&pd->mtx);
		return;
	}

	nni_aio_list_append(&pd->readq, aio);
	if ((pd->node.events & POLLIN) == 0) {
		pd->node.events |= POLLIN;
		rv = nni_posix_pollq_submit(pd->pq, &pd->node);
		if (rv != 0) {
			nni_posix_pipedesc_finish(aio, rv);
			nni_mtx_unlock(&pd->mtx);
			return;
		}
	}
	nni_mtx_unlock(&pd->mtx);
}


void
nni_posix_pipedesc_send(nni_posix_pipedesc *pd, nni_aio *aio)
{
	int rv;

	nni_mtx_lock(&pd->mtx);
	if ((rv = nni_aio_start(aio, nni_posix_pipedesc_cancel, pd)) != 0) {
		nni_mtx_unlock(&pd->mtx);
		return;
	}
	if (pd->fd < 0) {
		nni_posix_pipedesc_finish(aio, NNG_ECLOSED);
		nni_mtx_unlock(&pd->mtx);
		return;
	}

	nni_aio_list_append(&pd->writeq, aio);
	if ((pd->node.events & POLLOUT) == 0) {
		pd->node.events |= POLLOUT;
		rv = nni_posix_pollq_submit(pd->pq, &pd->node);
		if (rv != 0) {
			nni_posix_pipedesc_finish(aio, rv);
			nni_mtx_unlock(&pd->mtx);
			return;
		}
	}
	nni_mtx_unlock(&pd->mtx);
}


int
nni_posix_pipedesc_init(nni_posix_pipedesc **pdp, int fd)
{
	nni_posix_pipedesc *pd;
	int rv;

	if ((pd = NNI_ALLOC_STRUCT(pd)) == NULL) {
		return (NNG_ENOMEM);
	}
	memset(pd, 0, sizeof (*pd));

	// We could randomly choose a different pollq, or for efficiencies
	// sake we could take a modulo of the file desc number to choose
	// one.  For now we just have a global pollq.  Note that by tying
	// the pd to a single pollq we may get some kind of cache warmth.

	if ((rv = nni_mtx_init(&pd->mtx)) != 0) {
		NNI_FREE_STRUCT(pd);
		return (rv);
	}
	pd->pq = nni_posix_pollq_get(fd);
	pd->fd = fd;
	pd->node.fd = fd;
	pd->node.cb = nni_posix_pipedesc_cb;
	pd->node.data = pd;

	(void) fcntl(pd->fd, F_SETFL, O_NONBLOCK);

	nni_aio_list_init(&pd->readq);
	nni_aio_list_init(&pd->writeq);

	*pdp = pd;
	return (0);
}


void
nni_posix_pipedesc_fini(nni_posix_pipedesc *pd)
{
	// Make sure no other polling activity is pending.
	nni_posix_pipedesc_close(pd);

	nni_mtx_fini(&pd->mtx);

	NNI_FREE_STRUCT(pd);
}


#else

// Suppress empty symbols warnings in ranlib.
int nni_posix_pipedesc_not_used = 0;

#endif // PLATFORM_POSIX_PIPEDESC