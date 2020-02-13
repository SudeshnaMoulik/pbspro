/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <errno.h>
#ifdef WIN32
#if defined(FD_SET_IN_SYS_SELECT_H)
#  include <sys/select.h>
#endif
#include <io.h>
#else
#include <unistd.h>
#include <poll.h>
#include "libsec.h"
#endif
#include "libpbs.h"
#include "dis.h"
#include "auth.h"

volatile int reply_timedout = 0; /* for reply_send.c -- was alarm handler called? */

static int tcp_recv(int, void *, int);
static int tcp_send(int, void *, int);

/**
 * @brief
 *	Get the user buffer associated with the tcp channel. If no buffer has
 *	been set, then allocate a pbs_tcp_chan_t structure and associate with
 *	the given tcp channel
 *
 * @param[in] - fd - tcp channel to which to get/associate a user buffer
 *
 * @retval	NULL - Failure
 * @retval	!NULL - Buffer associated with the tcp channel
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: No
 *
 */
static pbs_tcp_chan_t *
tcp_get_chan(int fd)
{
	pbs_tcp_chan_t *chan = get_conn_chan(fd);
	if (chan == NULL) {
		if (errno != ENOTCONN) {
			dis_setup_chan(fd, get_conn_chan);
			chan = get_conn_chan(fd);
		}
	}
	return chan;
}

void
tcp_chan_free_extra(void *extra)
{
	if (extra != NULL && pbs_auth_destroy_ctx) {
		pbs_auth_destroy_ctx(extra);
	}
}

/**
 * @brief
 * 	tcp_recv - receive data from tcp stream
 *
 * @param[in] fd - socket descriptor
 * @param[out] data - data from tcp stream
 * @param[in] len - bytes to receive from tcp stream
 *
 * @return	int
 * @retval	0 	if success
 * @retval	-1 	if error
 * @retval	-2 	if EOF (stream closed)
 */
static int
tcp_recv(int fd, void *data, int len)
{
	int i;
#ifdef WIN32
	fd_set readset;
	struct timeval timeout;
#else
	struct pollfd pollfds[1];
	int timeout;
#endif

	/*
	 * we don't want to be locked out by an attack on the port to
	 * deny service, so we time out the read, the network had better
	 * deliver promptly
	 */
	do {
#ifdef WIN32
		timeout.tv_sec = (long) pbs_tcp_timeout;
		timeout.tv_usec = 0;
		FD_ZERO(&readset);
		FD_SET((unsigned int)fd, &readset);
		i = select(FD_SETSIZE, &readset, NULL, NULL, &timeout);
#else
		timeout = pbs_tcp_timeout;
		pollfds[0].fd = fd;
		pollfds[0].events = POLLIN;
		pollfds[0].revents = 0;
		i = poll(pollfds, 1, timeout * 1000);
#endif
		if (pbs_tcp_interrupt)
			break;
#ifdef WIN32
	} while ((i == -1) && (errno == WSAEINTR));
#else
	} while ((i == -1) && (errno == EINTR));
#endif

	if ((i == 0) || (i < 0))
		return i;

#ifdef WIN32
	while ((i = recv(fd, (char *)data, len, 0)) == -1) {
		errno = WSAGetLastError();
		if (errno != WSAEINTR) {
			if (errno == WSAECONNRESET) {
				i = 0;	/* treat like no data for winsock */
				/* will return this if remote */
				/* connection prematurely closed */
			}
			break;
		}
#else
	while ((i = CS_read(fd, (char *)data, (size_t)len)) == CS_IO_FAIL) {
		if (errno != EINTR)
			break;
#endif
	}
	return ((i == 0) ? -2 : i);
}

/**
 * @brief
 * 	tcp_send - send data to tcp stream
 *
 * @param[in] fd - socket descriptor
 * @param[out] data - data to send
 * @param[in] len - bytes to send
 *
 * @return	int
 * @retval	>0 	number of characters sent
 * @retval	0 	if EOD (no data currently avalable)
 * @retval	-1 	if error
 * @retval	-2 	if EOF (stream closed)
 */
static int
tcp_send(int fd, void *data, int len)
{
	size_t	ct = (size_t)len;
	int	i;
	int	j;
	char	*pb = (char *)data;
	struct	pollfd pollfds[1];

#ifdef WIN32
	while ((i = send(fd, pb, (int) ct, 0)) != (int)ct) {
		errno = WSAGetLastError();
		if (i == -1) {
			if (errno != WSAEINTR) {
				pbs_tcp_errno = errno;
				return (-1);
			} else
				continue;
		}
#else
	while ((i = CS_write(fd, pb, ct)) != ct) {
		if (i == CS_IO_FAIL) {
			if (errno == EINTR) {
				continue;
			}
			if (errno != EAGAIN) {
				/* fatal error on write, abort output */
				pbs_tcp_errno = errno;
				return (-1);
			}

			/* write would have blocked (EAGAIN returned) */
			/* poll for socket to be ready to accept, if  */
			/* not ready in TIMEOUT_SHORT seconds, fail   */
			/* redo the poll if EINTR		      */
			do {
				if (reply_timedout) {
					/* caught alarm - timeout spanning several writes for one reply */
					/* alarm set up in dis_reply_write() */
					/* treat identically to poll timeout */
					j = 0;
					reply_timedout = 0;
				} else {
					pollfds[0].fd = fd;
					pollfds[0].events = POLLOUT;
					pollfds[0].revents = 0;
					j = poll(pollfds, 1, pbs_tcp_timeout * 1000);
				}
			} while ((j == -1) && (errno == EINTR));

			if (j == 0) {
				/* never came ready, return error */
				/* pbs_tcp_errno will add to log message */
				pbs_tcp_errno = EAGAIN;
				return (-1);
			} else if (j == -1) {
				/* some other error - fatal */
				pbs_tcp_errno = errno;
				return (-1);
			}
			continue;	/* socket ready, retry write */
		}
#endif
		/* write succeeded, do more if needed */
		ct -= i;
		pb += i;
	}
	return len;
}

/**
 * @brief
 *	sets tcp related functions.
 *
 */
void
DIS_tcp_funcs()
{
	pfn_transport_get_chan = tcp_get_chan;
	pfn_transport_set_chan = set_conn_chan;
	pfn_transport_chan_free_extra = tcp_chan_free_extra;
	pfn_transport_recv = tcp_recv;
	pfn_transport_send = tcp_send;
}
