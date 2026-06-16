/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "rcp_interface.h"
#include "rcp_interface_impl.h"
#include "rcp_bus.h"

#include <arpa/inet.h>
#include <sys/socket.h>

/*
	Simple udp client
*/
#include <stdio.h>  //printf
#include <string.h> //memset
#include <stdlib.h> //exit(0);

#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>

static int socket_fd = -1;
static pthread_t rx_thread;
static bool reading = false;
enum rcp_bus inst;
uint16_t to_read;

#define SERVER "127.0.0.1"
#define BUFLEN 64   // Max length of buffer
#define PORT   8887 // The port on which to send data

static enum rcp_bus_status socket_open(void);
static enum rcp_bus_status socket_write(uint8_t *tx, uint16_t len);
static enum rcp_bus_status socket_read(uint8_t *rx, uint16_t len, uint16_t *nread);
static enum rcp_bus_status socket_close(void);
static enum rcp_bus_status socket_get_caps(struct rcp_bus_caps *caps);
static uint16_t socket_get_rx_len(void);

static void die(char *s)
{
	perror(s);
	exit(1);
}

static struct sockaddr_in si_other;
static struct rcp_bus_api bus_api = {
	.open = socket_open,
	.write = socket_write,
	.read = socket_read,
	.close = socket_close,
	.get_caps = socket_get_caps,
	.get_rx_len = socket_get_rx_len,
};

void rcp_bus_socket_init(void)
{
	int s, i, slen = sizeof(si_other);
	char buf[BUFLEN];
	char message[BUFLEN];

	if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		die("socket");
	}

	memset((char *)&si_other, 0, sizeof(si_other));
	si_other.sin_family = AF_INET;
	si_other.sin_port = htons(PORT);

	if (inet_aton(SERVER, &si_other.sin_addr) == 0) {
		fprintf(stderr, "inet_aton() failed\n");
		exit(1);
	}
	if (bind(s, (struct sockaddr *)&si_other, sizeof(si_other)) == 0) {
		printf("Opened Socket: %s:%d\n", SERVER, PORT);
	}
	/* Imitiate SPI bus for demo purposes. */
	inst = RCP_BUS_SPI;
	rcp_bus_register(inst, &bus_api);

	socket_fd = s;
}

static void *rx_thread_handle(void *arg)
{
	struct pollfd fds = {
		.fd = socket_fd,
		.events = POLLIN,
	};

	while (socket_fd >= 0) {
		if (reading) {
			sched_yield();
			continue;
		}
		int ret = poll(&fds, 1, 1000);
		if (ret < 0) {
			perror("poll");
			break;
		} else if (ret == 0) {
			continue;
		}

		if (fds.revents & POLLIN) {
			reading = true;
			int nbytes = 0;
			if (ioctl(socket_fd, FIONREAD, &nbytes) == 0) {
				to_read = nbytes;
				/* TODO: Block further writes? */
				__rcp_bus_set_flags(inst, RCP_BUS_FLAG_RX_DATA, 0);
			}
		}
	}
}

// link_result_t wait_for_rx(int read_fd)
// {
//     fd_set rfds;
//     struct timeval tv;
//     int retval;

//     FD_ZERO(&rfds);
//     FD_SET(read_fd, &rfds);

//     tv.tv_sec = 10;
//     tv.tv_usec = 0;

//    retval = select(read_fd + 1, &rfds, NULL, NULL, &tv);

//    if (retval == -1)
//    {
//         perror("select()");
//         return RCP_LNK_NO_RX_DATA;
//    }
//     else if (retval)
//       return RCP_LNK_SUCCESS;
//     else
//       return RCP_LNK_TIMEOUT;
// }

// link_result_t wait_for_tx_ready(int write_fd)
// {
//     fd_set wfds;
//     struct timeval tv;
//     int retval;

//     FD_ZERO(&wfds);
//     FD_SET(write_fd, &wfds);

//     tv.tv_sec = 3;
//     tv.tv_usec = 0;

//    retval = select(write_fd + 1, NULL, &wfds, NULL, &tv);

//    if (retval == -1)
//    {
//         perror("select()");
//         return RCP_LNK_NO_RX_DATA;
//    }
//     else if (retval)
//       return RCP_LNK_SUCCESS;
//     else
//       return RCP_LNK_TIMEOUT;
// }

static enum rcp_bus_status socket_open(void)
{
	pthread_create(&rx_thread, NULL, rx_thread_handle, NULL);
	/* Notify TX ready. */
	__rcp_bus_set_flags(inst, RCP_BUS_FLAG_TX_RDY, 0);
	return RCP_BUS_OK;
}

static enum rcp_bus_status socket_write(uint8_t *tx, uint16_t len)
{
	__rcp_bus_set_flags(inst, 0, RCP_BUS_FLAG_TX_RDY);
	if (sendto(socket_fd, tx, len, 0, (struct sockaddr *)&si_other, sizeof(si_other)) == -1) {
		die("sendto()");
	}

	__rcp_bus_set_flags(inst, RCP_BUS_FLAG_TX_RDY, 0);
}

static enum rcp_bus_status socket_read(uint8_t *rx, uint16_t len, uint16_t *nread)
{
	int recv_len = 0;
	socklen_t slen = sizeof(si_other);

	/* Block TX. */
	__rcp_bus_set_flags(inst, 0, RCP_BUS_FLAG_TX_RDY);
	// wait_for_rx(sock_fd);
	if ((recv_len = recvfrom(socket_fd, rx, len, MSG_DONTWAIT, (struct sockaddr *)&si_other,
				 &slen)) == -1) {
		__rcp_bus_set_flags(inst, RCP_BUS_FLAG_TX_RDY, 0);
		if (errno == EAGAIN) {
			return RCP_BUS_ERR_EMPTY;
		}
		die("recvfrom()");
	}
	reading = false;
	*nread = recv_len;
	if (*nread == len) {
		/* No more data. */
		__rcp_bus_set_flags(inst, 0, RCP_BUS_FLAG_RX_DATA);
	} else {
		/* Notify again. */
		__rcp_bus_set_flags(inst, RCP_BUS_FLAG_RX_DATA, 0);
		to_read -= *nread;
	}
	__rcp_bus_set_flags(inst, RCP_BUS_FLAG_TX_RDY, 0);
	return RCP_BUS_OK;
}

static enum rcp_bus_status socket_close(void)
{
	/* Clear all flags. */
	__rcp_bus_set_flags(inst, 0, RCP_BUS_FLAG_RX_DATA | RCP_BUS_FLAG_TX_RDY);
	close(socket_fd);
	pthread_join(rx_thread, NULL);
	return RCP_BUS_OK;
}

uint16_t socket_get_rx_len(void)
{
	return to_read;
}

void rcp_bus_socket_close(void)
{
	(void)socket_close();
}

static enum rcp_bus_status socket_get_caps(struct rcp_bus_caps *caps)
{
	return RCP_BUS_OK;
}
