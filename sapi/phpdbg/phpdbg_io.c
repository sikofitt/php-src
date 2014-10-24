/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2014 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Anatol Belski <ab@php.net>                                  |
   +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "phpdbg_io.h"

#ifdef PHP_WIN32
#undef UNICODE
#include "win32/inet.h"
#include <winsock2.h>
#include <windows.h>
#include <Ws2tcpip.h>
#include "win32/sockets.h"

#else

#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#endif

ZEND_EXTERN_MODULE_GLOBALS(phpdbg);

PHPDBG_API int phpdbg_consume_bytes(int sock, char *ptr, int len, int tmo TSRMLS_DC) {
	int got_now, i = len, j;
	char *p = ptr;
#ifndef PHP_WIN32
	struct pollfd pfd;

	if (tmo < 0) goto recv_once;
	pfd.fd = sock;
	pfd.events = POLLIN;

	j = poll(&pfd, 1, tmo);

	if (j == 0) {
#else
	struct fd_set readfds;
	struct timeval ttmo;

	if (tmo < 0) goto recv_once;
	FD_ZERO(&readfds);
	FD_SET(sock, &readfds);

	ttmo.tv_sec = 0;
	ttmo.tv_usec = tmo*1000;

	j = select(0, &readfds, NULL, NULL, &ttmo);

	if (j <= 0) {
#endif
		return -1;
	}

recv_once:
	while(i > 0) {
		if (tmo < 0) {
			/* There's something to read. Read what's available and proceed
			disregarding whether len could be exhausted or not.*/
			int can_read = recv(sock, p, i, MSG_PEEK);
#ifndef _WIN32
			if (can_read == -1 && errno == EINTR) {
				continue;
			}
#endif
			i = can_read;
		}

#ifdef _WIN32
		got_now = recv(sock, p, i, 0);
#else
		do {
			got_now = recv(sock, p, i, 0);
		} while (got_now == -1 && errno == EINTR);
#endif

		if (got_now == -1) {
			write(PHPDBG_G(io)[PHPDBG_STDERR].fd, ZEND_STRL("Read operation timed out!\n"));
			return -1;
		}
		i -= got_now;
		p += got_now;
	}

	return p - ptr;
}

PHPDBG_API int phpdbg_send_bytes(int sock, const char *ptr, int len) {
	int sent, i = len;
	const char *p = ptr;
/* XXX poll/select needed here? */
	while(i > 0) {
		sent = send(sock, p, i, 0);
		if (sent == -1) {
			return -1;
		}
		i -= sent;
		p += sent;
	}

	return len;
}


PHPDBG_API int phpdbg_mixed_read(int sock, char *ptr, int len, int tmo TSRMLS_DC) {
	if (PHPDBG_G(flags) & PHPDBG_IS_REMOTE) {
		return phpdbg_consume_bytes(sock, ptr, len, tmo TSRMLS_CC);
	}

	return read(sock, ptr, len);
}


PHPDBG_API int phpdbg_mixed_write(int sock, const char *ptr, int len TSRMLS_DC) {
	if (PHPDBG_G(flags) & PHPDBG_IS_REMOTE) {
		return phpdbg_send_bytes(sock, ptr, len);
	}

	return write(sock, ptr, len);
}


PHPDBG_API int phpdbg_open_socket(const char *interface, unsigned short port TSRMLS_DC) {
	struct addrinfo res;
	int fd = phpdbg_create_listenable_socket(interface, port, &res TSRMLS_CC);

	if (fd == -1) {
		return -1;
	}

	if (bind(fd, res.ai_addr, res.ai_addrlen) == -1) {
		phpdbg_close_socket(fd);
		return -4;
	}

	listen(fd, 5);

	return fd;
}


PHPDBG_API int phpdbg_create_listenable_socket(const char *addr, unsigned short port, struct addrinfo *addr_res TSRMLS_DC) {
	int sock = -1, rc;
	int reuse = 1;
	struct in6_addr serveraddr;
	struct addrinfo hints, *res = NULL;
	char port_buf[8];
	int8_t any_addr = *addr == '*';

	do {
		memset(&hints, 0, sizeof hints);
		if (any_addr) {
			hints.ai_flags = AI_PASSIVE;
		} else {
			hints.ai_flags = AI_NUMERICSERV;
		}
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		rc = inet_pton(AF_INET, addr, &serveraddr);
		if (1 == rc) {
			hints.ai_family = AF_INET;
			if (!any_addr) {
				hints.ai_flags |= AI_NUMERICHOST;
			}
		} else {
			rc = inet_pton(AF_INET6, addr, &serveraddr);
			if (1 == rc) {
				hints.ai_family = AF_INET6;
				if (!any_addr) {
					hints.ai_flags |= AI_NUMERICHOST;
				}
			} else {
				/* XXX get host by name ??? */
			}
		}

		snprintf(port_buf, 7, "%u", port);
		if (!any_addr) {
			rc = getaddrinfo(addr, port_buf, &hints, &res);
		} else {
			rc = getaddrinfo(NULL, port_buf, &hints, &res);
		}

		if (0 != rc) {
#ifndef PHP_WIN32
			if (rc == EAI_SYSTEM) {
				char buf[128];
				int wrote;

				wrote = snprintf(buf, 128, "Could not translate address '%s'", addr);
				buf[wrote] = '\0';
				write(PHPDBG_G(io)[PHPDBG_STDERR].fd, buf, strlen(buf));

				return sock;
			} else {
#endif
				char buf[256];
				int wrote;

				wrote = snprintf(buf, 256, "Host '%s' not found. %s", addr, estrdup(gai_strerror(rc)));
				buf[wrote] = '\0';
				write(PHPDBG_G(io)[PHPDBG_STDERR].fd, buf, strlen(buf));

				return sock;
#ifndef PHP_WIN32
			}
#endif
			return sock;
		}

		if((sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) == -1) {
			char buf[128];
			int wrote;

			wrote = sprintf(buf, "Unable to create socket");
			buf[wrote] = '\0';
			write(PHPDBG_G(io)[PHPDBG_STDERR].fd, buf, strlen(buf));

			return sock;
		} 

		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*) &reuse, sizeof(reuse)) == -1) {
			phpdbg_close_socket(sock);
			return sock;
		}


	} while (0);

	*addr_res = *res;

	return sock;
}

PHPDBG_API void phpdbg_close_socket(int sock) {
	if (socket >= 0) {
#ifdef _WIN32
		closesocket(sock);
#else
		shutdown(sock, SHUT_RDWR);
		close(sock);
#endif
	}
}
