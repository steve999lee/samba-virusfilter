/*
   Samba Anti-Virus VFS modules
   Copyright (C) 2010 SATOH Fumiyasu @ OSS Technology, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "sav-common.h"
#include "sav-utils.h"

#include <poll.h>

#define SAV_ENV_SIZE_CHUNK 32

/* ====================================================================== */

char *sav_string_sub(TALLOC_CTX *mem_ctx, connection_struct *conn, const char *str)
{
	return talloc_sub_advanced(mem_ctx, lp_servicename(SNUM(conn)),
					conn->user,
					conn->connectpath, conn->gid,
					get_current_username(),
					current_user_info.domain,
					str);
}

/* Line-based socket I/O
 * ====================================================================== */

sav_io_handle *sav_io_new(TALLOC_CTX *mem_ctx, int connect_timeout, int timeout)
{
	sav_io_handle *io_h = TALLOC_ZERO_P(mem_ctx, sav_io_handle);

	if (!io_h) {
		return NULL;
	}

	io_h->socket = -1;
	io_h->eol = '\n';
	/* timeout <= 0 means infinite */
	io_h->connect_timeout = (connect_timeout > 0) ? connect_timeout : -1;
	io_h->timeout = (timeout > 0) ? timeout : -1;

	return io_h;
}

int sav_io_set_eol(sav_io_handle *io_h, int eol)
{
	int eol_old = io_h->eol;

	io_h->eol = eol;

	return eol_old;
}

sav_result sav_io_connect_path(sav_io_handle *io_h, const char *path)
{
	struct sockaddr_un addr;

	ZERO_STRUCT(addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path));

	io_h->socket = open_socket_out(SOCK_STREAM,
		(struct sockaddr_storage *)&addr, 0, io_h->connect_timeout);
	if (io_h->socket == -1) {
		return SAV_RESULT_ERROR;
	}

	return SAV_RESULT_OK;
}

sav_result sav_io_disconnect(sav_io_handle *io_h)
{
	if (io_h->socket != -1) {
		close(io_h->socket);
		io_h->socket = -1;
	}

	io_h->w_size = io_h->r_size = io_h->r_rest_size = 0;
	io_h->r_rest_buffer = NULL;

	return SAV_RESULT_OK;
}

sav_result sav_io_write(sav_io_handle *io_h)
{
	char *buffer = io_h->w_buffer;
	ssize_t buffer_size = io_h->w_size;
	struct pollfd pollfd;
	ssize_t wrote_size;

	if (buffer_size == 0) {
		return SAV_RESULT_OK;
	}

	if (buffer[buffer_size-1] != io_h->eol) {
		buffer[buffer_size] = io_h->eol;
		buffer_size++;
	}

	pollfd.fd = io_h->socket;
	pollfd.events = POLLOUT;

	while (buffer_size > 0) {
		switch (poll(&pollfd, 1, io_h->timeout)) {
		case -1:
			if (errno == EINTR) {
				errno = 0;
				continue;
			}
			return SAV_RESULT_ERROR;
		case 0:
			errno = ETIMEDOUT;
			return SAV_RESULT_ERROR;
		}

		wrote_size = write(io_h->socket, buffer, buffer_size);
		if (wrote_size == -1) {
			if (errno == EINTR) {
				errno = 0;
				continue;
			}
			return SAV_RESULT_ERROR;
		}

		buffer += wrote_size;
		buffer_size -= wrote_size;
	}

	return SAV_RESULT_OK;
}

sav_result sav_io_read(sav_io_handle *io_h)
{
	char *buffer;
	ssize_t buffer_size = SAV_IO_BUFFER_SIZE;
	struct pollfd pollfd;
	ssize_t read_size;
	char *eol;

	if (io_h->r_rest_buffer == NULL) {
		DEBUG(10,("Rest data not found in read buffer\n"));
		buffer = io_h->r_buffer = io_h->r_buffer_real;
		buffer_size = SAV_IO_BUFFER_SIZE;
	} else {
		DEBUG(10,("Rest data found in read buffer: %s, size=%ld\n",
			io_h->r_rest_buffer, (long)io_h->r_rest_size));
		eol = memchr(io_h->r_rest_buffer, io_h->eol, io_h->r_rest_size);
		if (eol) {
			*eol = '\0';
			io_h->r_buffer = io_h->r_rest_buffer;
			io_h->r_size = eol - io_h->r_rest_buffer;
			DEBUG(10,("Read line data from read buffer: %s\n", io_h->r_buffer));

			io_h->r_rest_size -= io_h->r_size + 1;
			io_h->r_rest_buffer = (io_h->r_rest_size > 0) ? (eol + 1) : NULL;

			return SAV_RESULT_OK;
		}

		io_h->r_buffer = io_h->r_buffer_real;
		memmove(io_h->r_buffer, io_h->r_rest_buffer, io_h->r_rest_size);

		buffer = io_h->r_buffer + io_h->r_size;
		buffer_size = SAV_IO_BUFFER_SIZE - io_h->r_rest_size;
	}

	io_h->r_rest_buffer = NULL;
	io_h->r_rest_size = 0;

	pollfd.fd = io_h->socket;
	pollfd.events = POLLIN;

	while (buffer_size > 0) {
		switch (poll(&pollfd, 1, io_h->timeout)) {
		case -1:
			if (errno == EINTR) {
				errno = 0;
				continue;
			}
			return SAV_RESULT_ERROR;
		case 0:
			errno = ETIMEDOUT;
			return SAV_RESULT_ERROR;
		}

		read_size = read(io_h->socket, buffer, buffer_size);
		if (read_size == -1) {
			if (errno == EINTR) {
				errno = 0;
				continue;
			}
			return SAV_RESULT_ERROR;
		}

		buffer[read_size] = '\0';

		if (read_size == 0) { /* EOF */
			return SAV_RESULT_OK;
		}

		io_h->r_size += read_size;

		eol = memchr(buffer, io_h->eol, read_size);
		if (eol) {
			*eol = '\0';
			DEBUG(10,("Read line data from socket: %s\n", io_h->r_buffer));
			io_h->r_size = eol - io_h->r_buffer;
			io_h->r_rest_size = read_size - (eol - buffer + 1);
			if (io_h->r_rest_size > 0) {
				io_h->r_rest_buffer = eol + 1;
				DEBUG(10,("Rest data in read buffer: %s, size=%ld\n",
					io_h->r_rest_buffer, (long)io_h->r_rest_size));
			}
			return SAV_RESULT_OK;
		}

		buffer += read_size;
		buffer_size -= read_size;
	}

	errno = E2BIG;

	return SAV_RESULT_ERROR;
}

sav_result sav_io_writeread(sav_io_handle *io_h, const char *fmt, ...)
{
	if (fmt) {
		va_list ap;

		va_start(ap, fmt);
		io_h->w_size = vsnprintf(io_h->w_buffer, SAV_IO_BUFFER_SIZE, fmt, ap);
		va_end(ap);

		DEBUG(10,("Write line data: %s\n", io_h->w_buffer));

		if (sav_io_write(io_h) != SAV_RESULT_OK) {
			return SAV_RESULT_ERROR;
		}
	}

	if (sav_io_read(io_h) != SAV_RESULT_OK) {
		return SAV_RESULT_ERROR;
	}
	if (io_h->r_size == 0) { /* EOF */
		return SAV_RESULT_ERROR; /* FIXME: SAV_RESULT_EOF? */
	}

	DEBUG(10,("Read line data: %s\n", io_h->r_buffer));

	return SAV_RESULT_OK;
}

/* Generic "stupid" cache
 * ====================================================================== */

sav_cache_handle *sav_cache_new(TALLOC_CTX *ctx, int entry_limit, time_t time_limit)
{
	sav_cache_handle *cache_h = TALLOC_ZERO_P(ctx, sav_cache_handle);
	if (!cache_h) {
		DEBUG(0,("TALLOC_ZERO_P failed\n"));
		return NULL;
	}
	cache_h->entry_limit = entry_limit;
	cache_h->time_limit = time_limit;

	return cache_h;
}

void sav_cache_purge(sav_cache_handle *cache_h)
{
	sav_cache_entry *cache_e;
	time_t time_now = time(NULL);

	DEBUG(10,("Purging cache entry\n"));

	for (cache_e = cache_h->end; cache_e; cache_e = cache_h->end) {
		time_t time_age = time_now - cache_e->time;
		DEBUG(10,("Checking cache entry: fname=%s, age=%ld\n", cache_e->fname, (long)time_age));
		if (cache_h->entry_num <= cache_h->entry_limit &&
		    time_age < cache_h->time_limit) {
			break;
		}

		DEBUG(10,("Purging cache entry: %s\n", cache_e->fname));
		cache_h->end = cache_e->prev;
		cache_h->entry_num--;
		DLIST_REMOVE(cache_h->list, cache_e);
		TALLOC_FREE(cache_e);
	}
}

sav_cache_entry *sav_cache_get(sav_cache_handle *cache_h, const char *fname, int fname_len)
{
	sav_cache_entry *cache_e;

	sav_cache_purge(cache_h);

	if (fname_len <= 0) {
		fname_len = strlen(fname);
	}

	DEBUG(10,("Searching cache entry: fname=%s\n", fname));

	for (cache_e = cache_h->list; cache_e; cache_e = cache_e->next) {
		DEBUG(10,("Checking cache entry: fname=%s\n", cache_e->fname));
		if (cache_e->fname_len == fname_len && str_eq(cache_e->fname, fname)) {
			break;
		}
	}

	return cache_e;
}

void sav_cache_add(sav_cache_handle *cache_h, sav_cache_entry *cache_e)
{
	cache_e->fname_len = strlen(cache_e->fname);
	cache_e->time = time(NULL);

	DLIST_ADD(cache_h->list, cache_e);

	cache_h->entry_num++;
	if (!cache_h->end) {
		cache_h->end = cache_e;
	}

	sav_cache_purge(cache_h);
}

/* Environment variable handling for execle(2)
 * ====================================================================== */

sav_env_struct *sav_env_new(TALLOC_CTX *ctx)
{
	sav_env_struct *env_h = TALLOC_ZERO_P(ctx, sav_env_struct);
	if (!env_h) {
		DEBUG(0, ("TALLOC_ZERO_P failed\n"));
		goto sav_env_init_failed;
	}

	env_h->env_num = 0;
	env_h->env_size = SAV_ENV_SIZE_CHUNK;
	env_h->env_list = TALLOC_ARRAY(env_h, char *, env_h->env_size);
	if (!env_h->env_list) {
		DEBUG(0, ("TALLOC_ARRAY failed\n"));
		goto sav_env_init_failed;
	}

	env_h->env_list[0] = NULL;

	return env_h;

sav_env_init_failed:
	TALLOC_FREE(env_h);
	return NULL;
}

char * const *sav_env_list(sav_env_struct *env_h)
{
	return env_h->env_list;
}

int sav_env_set(sav_env_struct *env_h, const char *name, const char *value)
{
	size_t name_len = strlen(name);
	size_t env_len = name_len + 1 + strlen(value); /* strlen("name=value") */
	char **env_p;

	/* Named env value already exists? */
	for (env_p = env_h->env_list; *env_p != NULL; env_p++) {
		if ((*env_p)[name_len] == '=' && strn_eq(*env_p, name, name_len)) {
			break;
		}
	}

	if (!*env_p) {
		/* Not exist. Adding a new env entry */
		char *env_new;

		if (env_h->env_size == env_h->env_num + 1) {
			/* Enlarge env_h->env_list */
			size_t env_size_new = env_h->env_size + SAV_ENV_SIZE_CHUNK;
			char **env_list_new = TALLOC_REALLOC_ARRAY(
				env_h, env_h->env_list, char *, env_size_new);
			if (!env_list_new) {
				DEBUG(0,("TALLOC_REALLOC_ARRAY failed\n"));
				return -1;
			}
			env_h->env_list = env_list_new;
			env_h->env_size = env_size_new;
		}

		env_new = talloc_asprintf(env_h, "%s=%s", name, value);
		if (!env_new) {
			DEBUG(0,("talloc_asprintf failed\n"));
			return -1;
		}
		*env_p = env_new;
		env_h->env_num++;
		env_h->env_list[env_h->env_num] = NULL;

		return 0;
	}

	if (strlen(*env_p) < env_len) {
		/* Exist, but buffer is too short */
		char *env_new = talloc_asprintf(env_h, "%s=%s", name, value);
		if (!env_new) {
			DEBUG(0,("talloc_asprintf failed\n"));
			return -1;
		}
		TALLOC_FREE(*env_p);
		*env_p = env_new;

		return 0;
	}

	/* Exist and buffer is enough to overwrite */
	snprintf(*env_p, env_len + 1, "%s=%s", name, value);

	return 0;
}

/* Shell scripting
 * ====================================================================== */

/* sav_env version Samba's *_sub_advanced() in substitute.c */
int sav_shell_set_conn_env(sav_env_struct *env_h, connection_struct *conn)
{
	int snum = SNUM(conn);
	char addr[INET6_ADDRSTRLEN];
	const char *local_machine_name = get_local_machine_name();
	fstring pidstr;

	if (!local_machine_name || !*local_machine_name) {
		local_machine_name = global_myname();
	}

	client_socket_addr(get_client_fd(), addr, sizeof(addr));
	sav_env_set(env_h, "SAV_COMMAND_SERVER_IP", addr + (strnequal(addr,"::ffff:",7) ? 7 : 0));
	sav_env_set(env_h, "SAV_COMMAND_SERVER_NAME", myhostname());
	sav_env_set(env_h, "SAV_COMMAND_SERVER_NETBIOS_NAME", local_machine_name);
	slprintf(pidstr,sizeof(pidstr)-1, "%ld", (long)sys_getpid());
	sav_env_set(env_h, "SAV_COMMAND_SERVER_PID", pidstr);

	sav_env_set(env_h, "SAV_COMMAND_SERVICE_NAME", lp_servicename(snum));
	sav_env_set(env_h, "SAV_COMMAND_SERVICE_PATH", conn->connectpath);

	client_addr(get_client_fd(), addr, sizeof(addr));
	sav_env_set(env_h, "SAV_COMMAND_CLIENT_IP", addr + (strnequal(addr,"::ffff:",7) ? 7 : 0));
	sav_env_set(env_h, "SAV_COMMAND_CLIENT_NAME", client_name(get_client_fd()));
	sav_env_set(env_h, "SAV_COMMAND_CLIENT_NETBIOS_NAME", get_remote_machine_name());

	sav_env_set(env_h, "SAV_COMMAND_USER_NAME", get_current_username());
	sav_env_set(env_h, "SAV_COMMAND_USER_DOMAIN", current_user_info.domain);

	return 0;
}

/* Modified version of Samba's smbrun() in smbrun.c */
int sav_shell_run(
	const char *cmd,
	uid_t uid,
	gid_t gid,
	sav_env_struct *env_h,
	connection_struct *conn,
	bool sanitize)
{
	pid_t pid;

	if (!env_h) {
		env_h = sav_env_new(talloc_tos());
		if (!env_h) {
			return -1;
		}
	}

	if (conn && sav_shell_set_conn_env(env_h, conn) == -1) {
		return -1;
	}

#ifdef SAV_RUN_OUTFD_SUPPORT
	/* point our stdout at the file we want output to go into */
	if (outfd && ((*outfd = setup_out_fd()) == -1)) {
		return -1;
	}
#endif

	/* in this method we will exec /bin/sh with the correct
	   arguments, after first setting stdout to point at the file */

	/*
	 * We need to temporarily stop CatchChild from eating
	 * SIGCLD signals as it also eats the exit status code. JRA.
	 */

	CatchChildLeaveStatus();

	if ((pid=sys_fork()) < 0) {
		DEBUG(0,("sav_run: fork failed with error %s\n", strerror(errno)));
		CatchChild();
#ifdef SAV_RUN_OUTFD_SUPPORT
		if (outfd) {
			close(*outfd);
			*outfd = -1;
		}
#endif
		return errno;
	}

	if (pid) {
		/*
		 * Parent.
		 */
		int status=0;
		pid_t wpid;

		/* the parent just waits for the child to exit */
		while((wpid = sys_waitpid(pid,&status,0)) < 0) {
			if(errno == EINTR) {
				errno = 0;
				continue;
			}
			break;
		}

		CatchChild();

		if (wpid != pid) {
			DEBUG(2,("waitpid(%d) : %s\n",(int)pid,strerror(errno)));
#ifdef SAV_RUN_OUTFD_SUPPORT
			if (outfd) {
				close(*outfd);
				*outfd = -1;
			}
#endif
			return -1;
		}

#ifdef SAV_RUN_OUTFD_SUPPORT
		/* Reset the seek pointer. */
		if (outfd) {
			sys_lseek(*outfd, 0, SEEK_SET);
		}
#endif

#if defined(WIFEXITED) && defined(WEXITSTATUS)
		if (WIFEXITED(status)) {
			return WEXITSTATUS(status);
		}
#endif

		return status;
	}

	CatchChild();

	/* we are in the child. we exec /bin/sh to do the work for us. we
	   don't directly exec the command we want because it may be a
	   pipeline or anything else the config file specifies */

#ifdef SAV_RUN_OUTFD_SUPPORT
	/* point our stdout at the file we want output to go into */
	if (outfd) {
		close(1);
		if (dup2(*outfd,1) != 1) {
			DEBUG(2,("Failed to create stdout file descriptor\n"));
			close(*outfd);
			exit(80);
		}
	}
#endif

	/* now completely lose our privileges. This is a fairly paranoid
	   way of doing it, but it does work on all systems that I know of */

	become_user_permanently(uid, gid);

	if (!non_root_mode()) {
		if (getuid() != uid || geteuid() != uid ||
		    getgid() != gid || getegid() != gid) {
			/* we failed to lose our privileges - do not execute
			   the command */
			exit(81); /* we can't print stuff at this stage,
				     instead use exit codes for debugging */
		}
	}

#ifndef __INSURE__
	/* close all other file descriptors, leaving only 0, 1 and 2. 0 and
	   2 point to /dev/null from the startup code */
	{
	int fd;
	for (fd=3;fd<256;fd++) close(fd);
	}
#endif

	{
		char *newcmd = NULL;
		if (sanitize) {
			newcmd = escape_shell_string(cmd);
			if (!newcmd)
				exit(82);
		}

		execle("/bin/sh","sh","-c",
		    newcmd ? (const char *)newcmd : cmd, NULL, sav_env_list(env_h));

		SAFE_FREE(newcmd);
	}

	/* not reached */
	exit(83);
	return 1;
}
