/*
 * Test transport for native_sim, which has no Bluetooth: the same packets
 * the GATT characteristics carry, each prefixed with a big-endian u16 length
 * on a TCP connection. A Python test plays the Muse app on the other end.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include "setup.h"
#include "setup_transport.h"

LOG_MODULE_REGISTER(setup_tcp, LOG_LEVEL_INF);

static int s_client = -1;
static volatile bool s_open;
static K_MUTEX_DEFINE(s_send_lock);
static struct k_work_delayable s_close_work;

static int tcp_send(const uint8_t *pkt, size_t len)
{
	k_mutex_lock(&s_send_lock, K_FOREVER);
	int fd = s_client;
	uint8_t hdr[2] = {len >> 8, len & 0xFF};
	int rc = fd < 0 ? -ENOTCONN : 0;
	if (!rc && (zsock_send(fd, hdr, 2, 0) != 2 || zsock_send(fd, pkt, len, 0) != (ssize_t)len)) {
		rc = -EIO;
	}
	k_mutex_unlock(&s_send_lock);
	return rc;
}

static uint16_t tcp_mtu(void)
{
	return 185;
}

static volatile bool s_drop;

/* The listener notices within one poll interval and closes the client. */
static void close_work(struct k_work *work)
{
	s_drop = true;
}

static void tcp_disconnect(int delay_ms)
{
	k_work_reschedule(&s_close_work, K_MSEC(delay_ms));
}

static const struct setup_transport TRANSPORT = {
	.send = tcp_send,
	.mtu = tcp_mtu,
	.disconnect = tcp_disconnect,
};

static bool read_full(int fd, uint8_t *buf, size_t len)
{
	size_t got = 0;
	while (got < len) {
		if (s_drop) {
			return false;
		}
		/* Poll, then read without blocking: a blocking recv on native_sim's
		 * offloaded sockets can stall the whole simulator. */
		struct zsock_pollfd p = {.fd = fd, .events = ZSOCK_POLLIN};
		if (zsock_poll(&p, 1, 100) == 0) {
			continue;
		}
		ssize_t n = zsock_recv(fd, buf + got, len - got, ZSOCK_MSG_DONTWAIT);
		if (n == 0 || (n < 0 && errno != EAGAIN)) {
			return false;
		}
		if (n > 0) {
			got += n;
		}
	}
	return true;
}

static void listen_thread(void *a, void *b, void *c)
{
	int srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int one = 1;
	zsock_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_MUSE_SETUP_TCP_PORT),
	};
	if (zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || zsock_listen(srv, 1) < 0) {
		LOG_ERR("setup listener failed: %d", errno);
		return;
	}
	for (;;) {
		int fd = zsock_accept(srv, NULL, NULL);
		if (fd < 0) {
			continue;
		}
		if (!s_open) {
			zsock_close(fd);
			continue;
		}
		k_mutex_lock(&s_send_lock, K_FOREVER);
		s_client = fd;
		s_drop = false;
		k_mutex_unlock(&s_send_lock);
		LOG_INF("setup client connected");
		static uint8_t pkt[512];
		uint8_t hdr[2];
		while (read_full(fd, hdr, 2)) {
			size_t len = (hdr[0] << 8) | hdr[1];
			if (len > sizeof(pkt) || !read_full(fd, pkt, len)) {
				break;
			}
			setup_on_write(pkt, len);
		}
		k_mutex_lock(&s_send_lock, K_FOREVER);
		s_client = -1;
		zsock_close(fd);
		k_mutex_unlock(&s_send_lock);
		LOG_INF("setup client disconnected");
		setup_on_disconnect();
	}
}

K_THREAD_DEFINE(setup_tcp, 4096, listen_thread, NULL, NULL, NULL, 9, 0, 0);

int setup_transport_start(const char *name)
{
	k_work_init_delayable(&s_close_work, close_work);
	setup_init(&TRANSPORT, NULL);
	s_open = true;
	LOG_INF("setup open on TCP port %d as %s", CONFIG_MUSE_SETUP_TCP_PORT, name);
	return 0;
}

void setup_transport_stop(int delay_ms)
{
	s_open = false;
	tcp_disconnect(delay_ms);
}
