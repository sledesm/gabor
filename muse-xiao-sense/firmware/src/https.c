#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include "https.h"
#include "net.h"

LOG_MODULE_REGISTER(https, LOG_LEVEL_INF);

#define TIMEOUT_MS 15000

int https_split_url(const char *url, char *host, size_t host_size, unsigned short *port,
		    const char **path, int *tls)
{
	const char *p;
	if (strncmp(url, "https://", 8) == 0) {
		*tls = 1;
		p = url + 8;
	} else if (IS_ENABLED(CONFIG_MUSE_ALLOW_PLAINTEXT) && strncmp(url, "http://", 7) == 0) {
		*tls = 0;
		p = url + 7;
	} else {
		return -EINVAL;
	}
	size_t n = strcspn(p, ":/");
	if (n == 0 || n >= host_size) {
		return -EINVAL;
	}
	memcpy(host, p, n);
	host[n] = '\0';
	p += n;
	*port = *tls ? 443 : 80;
	if (*p == ':') {
		*port = (unsigned short)strtoul(p + 1, (char **)&p, 10);
	}
	*path = *p ? p : "/";
	return 0;
}

struct collect {
	char *buf;
	size_t cap;
	size_t len;
	int status;
};

static int on_response(struct http_response *rsp, enum http_final_call final, void *user_data)
{
	struct collect *c = user_data;
	if (rsp->http_status_code) {
		c->status = rsp->http_status_code;
	}
	if (rsp->body_frag_start && rsp->body_frag_len) {
		size_t n = MIN(rsp->body_frag_len, c->cap - 1 - c->len);
		memcpy(c->buf + c->len, rsp->body_frag_start, n);
		c->len += n;
	}
	return 0;
}

int https_request(const char *method, const char *url, const char *const *headers,
		  const char *body, char *resp, size_t resp_cap)
{
	char host[96];
	unsigned short port;
	const char *path;
	int tls;
	if (https_split_url(url, host, sizeof(host), &port, &path, &tls) != 0) {
		LOG_ERR("refusing URL %s", url);
		return -EINVAL;
	}
	int fd = net_open(host, port, tls, TIMEOUT_MS);
	if (fd < 0) {
		return fd;
	}
	static uint8_t recv_buf[1024];
	struct collect c = {.buf = resp, .cap = resp_cap, .len = 0, .status = 0};
	struct http_request req = {0};
	req.method = strcmp(method, "POST") == 0 ? HTTP_POST : HTTP_GET;
	req.url = path;
	req.host = host;
	req.protocol = "HTTP/1.1";
	req.response = on_response;
	req.recv_buf = recv_buf;
	req.recv_buf_len = sizeof(recv_buf);
	req.header_fields = (const char **)headers;
	if (body) {
		req.content_type_value = "application/json";
		req.payload = body;
		req.payload_len = strlen(body);
	}
	resp[0] = '\0';
	int rc = http_client_req(fd, &req, TIMEOUT_MS, &c);
	zsock_close(fd);
	resp[c.len] = '\0';
	if (rc < 0 || c.status == 0) {
		LOG_WRN("%s %s failed: %d", method, path, rc);
		return rc < 0 ? rc : -EIO;
	}
	return c.status;
}
