#include <stdio.h>
#include <ctype.h>
#include <unistd.h>

#include <libubox/ustream.h>
#include <libubox/ustream-ssl.h>
#include <libubox/usock.h>
#include <libubox/blobmsg.h>

#include "uclient.h"
#include "uclient-utils.h"
#include "uclient-backend.h"

static struct ustream_ssl_ctx *ssl_ctx;

enum request_type {
	REQ_GET,
	REQ_HEAD,
	REQ_POST,
	__REQ_MAX
};

enum http_state {
	HTTP_STATE_INIT,
	HTTP_STATE_HEADERS_SENT,
	HTTP_STATE_REQUEST_DONE,
	HTTP_STATE_RECV_HEADERS,
	HTTP_STATE_RECV_DATA,
	HTTP_STATE_ERROR,
};

static const char * const request_types[__REQ_MAX] = {
	[REQ_GET] = "GET",
	[REQ_HEAD] = "HEAD",
	[REQ_POST] = "POST",
};

struct uclient_http {
	struct uclient uc;

	struct ustream *us;

	struct ustream_fd ufd;
	struct ustream_ssl ussl;

	bool ssl;
	bool eof;
	enum request_type req_type;
	enum http_state state;

	long read_chunked;

	struct blob_buf headers;
	struct blob_buf meta;
};

enum {
	PREFIX_HTTP,
	PREFIX_HTTPS,
	__PREFIX_MAX,
};

static const char * const uclient_http_prefix[] = {
	[PREFIX_HTTP] = "http://",
	[PREFIX_HTTPS] = "https://",
	[__PREFIX_MAX] = NULL
};

static int uclient_do_connect(struct uclient_http *uh, const char *port)
{
	int fd;

	if (uh->uc.url->port)
		port = uh->uc.url->port;

	fd = usock(USOCK_TCP | USOCK_NONBLOCK, uh->uc.url->host, port);
	if (fd < 0)
		return -1;

	ustream_fd_init(&uh->ufd, fd);
	return 0;
}

static void uclient_notify_eof(struct uclient_http *uh)
{
	struct ustream *us = uh->us;

	if (!uh->eof) {
		if (!us->eof && !us->write_error)
			return;

		if (ustream_pending_data(us, false))
			return;
	}

	uclient_backend_set_eof(&uh->uc);
}

static void uclient_http_process_headers(struct uclient_http *uh)
{
	enum {
		HTTP_HDR_TRANSFER_ENCODING,
		__HTTP_HDR_MAX,
	};
	static const struct blobmsg_policy hdr_policy[__HTTP_HDR_MAX] = {
#define hdr(_name) { .name = _name, .type = BLOBMSG_TYPE_STRING }
		[HTTP_HDR_TRANSFER_ENCODING] = hdr("transfer-encoding"),
#undef hdr
	};
	struct blob_attr *tb[__HTTP_HDR_MAX];
	struct blob_attr *cur;

	blobmsg_parse(hdr_policy, __HTTP_HDR_MAX, tb, blob_data(uh->meta.head), blob_len(uh->meta.head));

	cur = tb[HTTP_HDR_TRANSFER_ENCODING];
	if (cur && strstr(blobmsg_data(cur), "chunked"))
		uh->read_chunked = 0;
}

static void uclient_parse_http_line(struct uclient_http *uh, char *data)
{
	char *name;
	char *sep;

	if (uh->state == HTTP_STATE_REQUEST_DONE) {
		uh->state = HTTP_STATE_RECV_HEADERS;
		return;
	}

	if (!*data) {
		uh->state = HTTP_STATE_RECV_DATA;
		uh->uc.meta = uh->meta.head;
		uclient_http_process_headers(uh);
		if (uh->uc.cb->header_done)
			uh->uc.cb->header_done(&uh->uc);
		return;
	}

	sep = strchr(data, ':');
	if (!sep)
		return;

	*(sep++) = 0;

	for (name = data; *name; name++)
		*name = tolower(*name);

	name = data;
	while (isspace(*sep))
		sep++;

	blobmsg_add_string(&uh->meta, name, sep);
}

static void __uclient_notify_read(struct uclient_http *uh)
{
	struct uclient *uc = &uh->uc;
	char *data;
	int len;

	if (uh->state < HTTP_STATE_REQUEST_DONE)
		return;

	data = ustream_get_read_buf(uh->us, &len);
	if (!data || !len)
		return;

	if (uh->state < HTTP_STATE_RECV_DATA) {
		char *sep;
		int cur_len;

		do {
			sep = strstr(data, "\r\n");
			if (!sep)
				break;

			/* Check for multi-line HTTP headers */
			if (sep > data) {
				if (!sep[2])
					return;

				if (isspace(sep[2]) && sep[2] != '\r') {
					sep[0] = ' ';
					sep[1] = ' ';
					continue;
				}
			}

			*sep = 0;
			cur_len = sep + 2 - data;
			uclient_parse_http_line(uh, data);
			ustream_consume(uh->us, cur_len);
			len -= cur_len;

			data = ustream_get_read_buf(uh->us, &len);
		} while (uh->state < HTTP_STATE_RECV_DATA);

		if (!len)
			return;
	}

	if (uh->state == HTTP_STATE_RECV_DATA && uc->cb->data_read)
		uc->cb->data_read(uc);
}

static void uclient_notify_read(struct ustream *us, int bytes)
{
	struct uclient_http *uh = container_of(us, struct uclient_http, ufd.stream);

	__uclient_notify_read(uh);
}

static void uclient_notify_state(struct ustream *us)
{
	struct uclient_http *uh = container_of(us, struct uclient_http, ufd.stream);

	uclient_notify_eof(uh);
}

static int uclient_setup_http(struct uclient_http *uh)
{
	struct ustream *us = &uh->ufd.stream;
	int ret;

	uh->us = us;
	us->string_data = true;
	us->notify_state = uclient_notify_state;
	us->notify_read = uclient_notify_read;

	ret = uclient_do_connect(uh, "80");
	if (ret)
		return ret;

	return 0;
}

static void uclient_ssl_notify_read(struct ustream *us, int bytes)
{
	struct uclient_http *uh = container_of(us, struct uclient_http, ussl.stream);

	__uclient_notify_read(uh);
}

static void uclient_ssl_notify_state(struct ustream *us)
{
	struct uclient_http *uh = container_of(us, struct uclient_http, ussl.stream);

	uclient_notify_eof(uh);
}

static int uclient_setup_https(struct uclient_http *uh)
{
	struct ustream *us = &uh->ussl.stream;
	int ret;

	uh->ssl = true;
	uh->us = us;

	ret = uclient_do_connect(uh, "443");
	if (ret)
		return ret;

	if (!ssl_ctx)
		ssl_ctx = ustream_ssl_context_new(false);

	us->string_data = true;
	us->notify_state = uclient_ssl_notify_state;
	us->notify_read = uclient_ssl_notify_read;
	ustream_ssl_init(&uh->ussl, &uh->ufd.stream, ssl_ctx, false);

	return 0;
}

static void uclient_http_disconnect(struct uclient_http *uh)
{
	uclient_backend_reset_state(&uh->uc);
	uh->read_chunked = -1;
	uh->eof = false;

	if (!uh->us)
		return;

	if (uh->ssl)
		ustream_free(&uh->ussl.stream);
	ustream_free(&uh->ufd.stream);
	close(uh->ufd.fd.fd);
	uh->us = NULL;
}

static int uclient_http_connect(struct uclient *cl)
{
	struct uclient_http *uh = container_of(cl, struct uclient_http, uc);

	uclient_http_disconnect(uh);
	blob_buf_init(&uh->meta, 0);

	uh->ssl = cl->url->prefix == PREFIX_HTTPS;
	uh->state = HTTP_STATE_INIT;

	if (uh->ssl)
		return uclient_setup_https(uh);
	else
		return uclient_setup_http(uh);
}

static struct uclient *uclient_http_alloc(void)
{
	struct uclient_http *uh;

	uh = calloc_a(sizeof(*uh));
	blob_buf_init(&uh->headers, 0);

	return &uh->uc;
}

static void uclient_http_free(struct uclient *cl)
{
	struct uclient_http *uh = container_of(cl, struct uclient_http, uc);

	uclient_http_disconnect(uh);
	blob_buf_free(&uh->headers);
	blob_buf_free(&uh->meta);
	free(uh);
}

int
uclient_http_set_request_type(struct uclient *cl, const char *type)
{
	struct uclient_http *uh = container_of(cl, struct uclient_http, uc);
	int i;

	if (cl->backend != &uclient_backend_http)
		return -1;

	if (uh->state > HTTP_STATE_INIT)
		return -1;

	for (i = 0; i < ARRAY_SIZE(request_types); i++) {
		if (strcmp(request_types[i], type) != 0)
			continue;

		uh->req_type = i;
		return 0;
	}

	return -1;
}

int
uclient_http_reset_headers(struct uclient *cl, const char *name, const char *value)
{
	struct uclient_http *uh = container_of(cl, struct uclient_http, uc);

	blob_buf_init(&uh->headers, 0);

	return 0;
}

int
uclient_http_set_header(struct uclient *cl, const char *name, const char *value)
{
	struct uclient_http *uh = container_of(cl, struct uclient_http, uc);

	if (cl->backend != &uclient_backend_http)
		return -1;

	if (uh->state > HTTP_STATE_INIT)
		return -1;

	blobmsg_add_string(&uh->headers, name, value);
	return 0;
}

#define ustream_printf(us, ...) do { \
	fprintf(stderr, "send: " __VA_ARGS__); \
	ustream_printf(us, __VA_ARGS__); \
} while (0)


static void
uclient_http_send_headers(struct uclient_http *uh)
{
	struct uclient_url *url = uh->uc.url;
	struct blob_attr *cur;
	int rem;

	if (uh->state >= HTTP_STATE_HEADERS_SENT)
		return;

	ustream_printf(uh->us,
		"%s /%s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Connection: close\r\n",
		request_types[uh->req_type],
		url->location, url->host);

	blobmsg_for_each_attr(cur, uh->headers.head, rem)
		ustream_printf(uh->us, "%s: %s\n", blobmsg_name(cur), (char *) blobmsg_data(cur));

	if (url->auth) {
		int auth_len = strlen(url->auth);
		char *auth_buf;

		if (auth_len > 512)
			return;

		auth_buf = alloca(base64_len(auth_len) + 1);
		base64_encode(url->auth, auth_len, auth_buf);
		ustream_printf(uh->us, "Authorization: Basic %s\r\n", auth_buf);
	}

	if (uh->req_type == REQ_POST)
		ustream_printf(uh->us, "Transfer-Encoding: chunked\r\n");

	ustream_printf(uh->us, "\r\n");
}

static int
uclient_http_send_data(struct uclient *cl, char *buf, unsigned int len)
{
	struct uclient_http *uh = container_of(cl, struct uclient_http, uc);

	if (uh->state >= HTTP_STATE_REQUEST_DONE)
		return -1;

	uclient_http_send_headers(uh);

	ustream_printf(uh->us, "%X\r\n", len);
	ustream_write(uh->us, buf, len, false);
	ustream_printf(uh->us, "\r\n");

	return len;
}

static int
uclient_http_request_done(struct uclient *cl)
{
	struct uclient_http *uh = container_of(cl, struct uclient_http, uc);

	if (uh->state >= HTTP_STATE_REQUEST_DONE)
		return -1;

	uclient_http_send_headers(uh);
	uh->state = HTTP_STATE_REQUEST_DONE;

	return 0;
}

static int
uclient_http_read(struct uclient *cl, char *buf, unsigned int len)
{
	struct uclient_http *uh = container_of(cl, struct uclient_http, uc);
	int read_len = 0;
	char *data, *data_end;

	if (uh->state < HTTP_STATE_RECV_DATA)
		return 0;

	data = ustream_get_read_buf(uh->us, &read_len);
	if (!data || !read_len)
		return 0;

	data_end = data + read_len;
	read_len = 0;

	if (uh->read_chunked == 0) {
		char *sep;

		if (data[0] == '\r' && data[1] == '\n') {
			data += 2;
			read_len += 2;
		}

		sep = strstr(data, "\r\n");
		if (!sep)
			return 0;

		*sep = 0;
		uh->read_chunked = strtoul(data, NULL, 16);

		read_len += sep + 2 - data;
		data = sep + 2;

		if (!uh->read_chunked)
			uh->eof = true;
	}

	if (len > data_end - data)
		len = data_end - data;

	if (uh->read_chunked >= 0) {
		if (len > uh->read_chunked)
			len = uh->read_chunked;

		uh->read_chunked -= len;
	}

	if (len > 0) {
		read_len += len;
		memcpy(buf, data, len);
	}

	if (read_len > 0)
		ustream_consume(uh->us, read_len);

	uclient_notify_eof(uh);

	return len;
}

const struct uclient_backend uclient_backend_http __hidden = {
	.prefix = uclient_http_prefix,

	.alloc = uclient_http_alloc,
	.free = uclient_http_free,
	.connect = uclient_http_connect,

	.read = uclient_http_read,
	.write = uclient_http_send_data,
	.request = uclient_http_request_done,
};
