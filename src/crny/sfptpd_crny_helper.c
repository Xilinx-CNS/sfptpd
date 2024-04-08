/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2017-2024 Advanced Micro Devices, Inc. */

/* Chrony connection helper */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <regex.h>

#include "sfptpd_crny_helper.h"


/****************************************************************************
 * Macros
 ****************************************************************************/


/****************************************************************************
 * Types
 ****************************************************************************/


/****************************************************************************
 * Constants
 ****************************************************************************/


/****************************************************************************
 * Function prototypes
 ****************************************************************************/


/****************************************************************************
 * Configuration
 ****************************************************************************/

int sfptpd_crny_helper_connect(const char *client_path,
			       const char *server_path,
			       int *sock_ret,
			       const char **failing_step)
{
	struct sockaddr_un client_addr = {
		.sun_family = AF_UNIX
	};
	struct sockaddr_un server_addr = {
		.sun_family = AF_UNIX
	};
	int sock;
	int flags;
	int rc = EINVAL;

	assert(client_path);
	assert(server_path);
	assert(sock_ret);
	assert(failing_step);

	*sock_ret = -1;
	*failing_step = "unknown";

	if (snprintf(client_addr.sun_path,
		     sizeof client_addr.sun_path,
		     "%s", client_path) >=
	    sizeof client_addr.sun_path) {
		*failing_step = "snprintf1";
		return ENOMEM;
	}

	if (snprintf(server_addr.sun_path,
		     sizeof server_addr.sun_path,
		     "%s", server_path) >=
	    sizeof server_addr.sun_path) {
		*failing_step = "snprintf2";
		return ENOMEM;
	}

	sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (sock < 0) {
		*failing_step = "socket";
		return errno;
	}

	flags = fcntl(sock, F_GETFD);
	if (flags == -1) {
		*failing_step = "fcntl(F_GETFD)";
		rc = errno;
		goto cleanup;
	}

	flags |= FD_CLOEXEC;

	if (fcntl(sock, F_SETFD, flags) < 0) {
		*failing_step = "fcntl(F_SETFD)";
		rc = errno;
		goto cleanup;
	}

	if (fcntl(sock, F_SETFL, O_NONBLOCK)) {
		*failing_step = "fcntl(F_SETFL)";
		rc = errno;
		goto cleanup;
	}

	/* Bind the local socket to the path we just specified */
	/* Note: we need to unlink before bind, in case the socket wasn't cleaned up last time */
	unlink(client_path);
	if (bind(sock, &client_addr, sizeof client_addr) < 0) {
		*failing_step = "bind";
		rc = errno;
		goto cleanup2;
	}

	/* You need to chmod 0666 the socket otherwise pselect will time out. */
	if (chmod(client_path, 0666) < 0) {
		*failing_step = "chmod";
		rc = errno;
		goto cleanup2;
	}

	/* Connect the socket */
	if (connect(sock, &server_addr, sizeof server_addr) < 0) {
		*failing_step = "connect";
		rc = errno;
		if (rc != EINPROGRESS)
			goto cleanup2;
	} else {
		rc = 0;
	}

	*sock_ret = sock;
	*failing_step = "success";

	return rc;

cleanup2:
	unlink(client_path);
cleanup:
	close(sock);
	return rc;
}

struct env_style {
	char *path;
	char *default_key;
	char *option_pattern;
};

enum edit_state {
	IDLE,
	OURS,
};

const struct env_style env_files[] = {
	{ "/etc/sysconfig/chronyd", "OPTIONS", "^[[:space:]]*(OPTIONS)[[:space:]]*=[[:space:]]*\"?([^\"]*)\"?[[:space:]]*([;#].*)?$" },
	{ "/etc/default/chrony", "DAEMON_OPTS", "^[[:space:]]*(DAEMON_OPTS)[[:space:]]*=[[:space:]]*\"?([^\"]*)\"?[[:space:]]*([;#].*)?$" },
};

#define START_BLOCK "### BEGIN sfptpd ###"
#define END_BLOCK "### END sfptpd ###"

const char *our_lines_start_pattern = "^" START_BLOCK;
const char *our_lines_end_pattern = "^" END_BLOCK;

static int edit_env(enum chrony_clock_control_op op)
{
	char errbuf[256];
	int rc = ENOENT;
	int i;
	unsigned int successes = 0;

	regex_t our_lines_start_re;
	regex_t our_lines_end_re;

	rc = regcomp(&our_lines_start_re, our_lines_start_pattern, REG_EXTENDED);
	if (rc != 0) {
		regerror(rc, &our_lines_start_re, errbuf, sizeof errbuf);
		fprintf(stderr, "crny-helper: regcomp: %s: %s\n", our_lines_start_pattern, errbuf);
		rc = EINVAL;
		goto fail0;
	}

	rc = regcomp(&our_lines_end_re, our_lines_end_pattern, REG_EXTENDED);
	if (rc != 0) {
		regerror(rc, &our_lines_end_re, errbuf, sizeof errbuf);
		fprintf(stderr, "crny-helper: regcomp: %s: %s\n", our_lines_end_pattern, errbuf);
		rc = EINVAL;
		goto fail1;
	}

	rc = ENOENT;
	for (i = 0; i < (sizeof env_files / sizeof env_files[0]); i++) {
		const struct env_style *style = env_files + i;
		regex_t option_re;
		enum edit_state state;
		int env_fd;
		FILE *env_stream = NULL;
		struct stat env_stat;
		off_t ours_start = -1;
		off_t ours_end = -1;
		off_t end = -1;
		off_t ptr = 0;
		char *options_key = NULL;
		char *options_value = NULL;
		int localrc;
		char *text = NULL;
		char *next_line;
		size_t map_sz;
		bool no_newline_at_eof = false;

		env_fd = open(style->path, O_RDWR);
		if (env_fd == -1) {
			rc = errno;
			continue;
		}

		localrc = fstat(env_fd, &env_stat);
		if (localrc == -1) {
			rc = errno;
			goto fail2;
		}

		map_sz = env_stat.st_size;
		end = map_sz;
		if (map_sz == 0)
			goto append_only;
		text = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE, env_fd, 0);
		if (text == MAP_FAILED) {
			rc = errno;
			goto fail2;
		}
		no_newline_at_eof = text[map_sz - 1] != '\n';

		rc = regcomp(&option_re, style->option_pattern, REG_EXTENDED);
		if (rc != 0) {
			regerror(rc, &option_re, errbuf, sizeof errbuf);
			fprintf(stderr, "crny-helper: regcomp: %s: %s\n", style->option_pattern, errbuf);
			rc = EINVAL;
			goto fail3;
		}

		state = IDLE;
		while (ptr < end) {
			off_t line_start;
			char *line;
			#define NMATCH 4
			regmatch_t match[NMATCH];

			line_start = ptr;
			line = next_line = text + ptr;
			if (no_newline_at_eof && strnlen(next_line, end - ptr) == end - ptr)
				next_line = text + end;
			else
				strsep(&next_line, "\n");
			ptr = next_line - text;
			switch (state) {
			case IDLE:
				if (!regexec(&our_lines_start_re, line, NMATCH, match, 0)) {
					state = OURS;
					ours_start = line_start;
				} else if (!regexec(&option_re, line, NMATCH, match, 0)) {
					free(options_key);
					free(options_value);
					if (match[1].rm_so != -1)
						options_key = strndup(line + match[1].rm_so,
								      match[1].rm_eo - match[1].rm_so);
					else
						options_key = NULL;
					if (match[2].rm_so != -1)
						options_value = strndup(line + match[2].rm_so,
								      match[2].rm_eo - match[2].rm_so);
					else
						options_value = NULL;
				}
				break;
			case OURS:
				if (!regexec(&our_lines_end_re, line, NMATCH, match, 0) || ptr == end) {
					off_t chunk;

					state = IDLE;
					ours_end = ptr;

					/* Delete our section */
					chunk = end - ours_end;
					memmove(text + ours_start, text + ours_end, chunk);
					ptr = ours_start;
					end -= ours_end - ours_start;
					text[end] = '\0';
				}
				break;
			}
		}

		regfree(&option_re);
	append_only:
		/* Append new settings */
		env_stream = fdopen(env_fd, "w");
		if (env_stream == NULL)
			goto fail3;
		rewind(env_stream);
		for (ptr = 0; ptr < end; ptr++)
			fputc(text[ptr] == '\0' ? '\n' : text[ptr], env_stream);
		if (no_newline_at_eof)
			fputc('\n', env_stream);

		switch (op) {
		case CRNY_CTRL_OP_DISABLE:
			fprintf(env_stream,
				"%s\n%s=\"%s%s%s\"\n%s\n",
				START_BLOCK,
				options_key ? options_key : style->default_key,
				options_value ? options_value : "",
				options_value ? " " : "",
				"-x",
				END_BLOCK);
			break;
		case CRNY_CTRL_OP_ENABLE:
		case CRNY_CTRL_OP_SAVE:
		case CRNY_CTRL_OP_RESTORE:
		case CRNY_CTRL_OP_RESTORENORESTART:
		case CRNY_CTRL_OP_NOP:
			break;
		}

		fflush(env_stream);
		rc = ftruncate(env_fd, ftell(env_stream));
		if (rc == 0)
			successes++;
	fail3:
		free(options_key);
		free(options_value);
		if (map_sz)
			munmap(text, map_sz);
	fail2:
		if (env_stream)
			fclose(env_stream);
		else
			close(env_fd);
	}

	regfree(&our_lines_end_re);
fail1:
	regfree(&our_lines_start_re);
fail0:
	if (rc != 0 && rc != ENOENT)
		perror("priv: edit_env");
	return successes != 0 ? 0 : rc;
}

int sfptpd_crny_helper_control(enum chrony_clock_control_op op)
{
	int rc;

	switch (op) {
	case CRNY_CTRL_OP_RESTORENORESTART:
		return edit_env(op);
	case CRNY_CTRL_OP_ENABLE:
	case CRNY_CTRL_OP_DISABLE:
	case CRNY_CTRL_OP_RESTORE:
		rc = edit_env(op);
		if (rc == 0) {
			rc = system("systemctl restart chronyd");
			if (rc == 127) {
				/* Non-systemd cases */
				rc = system("service chronyd restart");
				if (rc == 4) {
					/* Debianish case */
					rc = system("service chrony restart");
				}
			} else if (rc >= 4) {
				/* Debianish case */
				rc = system("systemctl restart chrony");
			}
		}
		return rc;
	case CRNY_CTRL_OP_SAVE:
	case CRNY_CTRL_OP_NOP:
	default:
		return 0;
	}
}
