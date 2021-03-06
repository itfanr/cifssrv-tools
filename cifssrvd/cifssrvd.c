/*
 *   cifssrv-tools/cifssrv/cifssrvd.c
 *
 *   Copyright (C) 2015 Samsung Electronics Co., Ltd.
 *   Copyright (C) 2016 Namjae Jeon <namjae.jeon@protocolfreedom.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include "cifssrv.h"

struct list_head cifssrv_share_list;
int cifssrv_num_shares;

char workgroup[MAX_SERVER_WRKGRP_LEN];
char server_string[MAX_SERVER_NAME_LEN];

void usage(void)
{
	fprintf(stderr,
		"Usage: cifssrvd [-h|--help] [-v|--version] [-d |--debug]\n"
		"       [-c smb.conf|--configure=smb.conf] [-i usrs-db|--import-users=cifspwd.db\n");
	exit(0);
}

/**
 * config_users() - function to configure cifssrv with user accounts from
 *		local database file. cifssrv should be live in kernel
 *		else this function fails and displays user message
 *		"cifssrv is not available"
 *
 * Return:	success: CIFS_SUCCESS; fail: CIFS_FAIL
 */
int config_users(char *dbpath)
{
	int eof = 0;
	char *lstr, *usr, *pwd, *construct = NULL;
	int len;
	int fd_usr, fd_db;

	fd_db = open(dbpath, O_RDONLY);
	if (!fd_db) {
		cifssrv_err("[%s] open failed\n", dbpath);
		perror("Error");
		return CIFS_FAIL;
	}

	fd_usr = open(PATH_CIFSSRV_USR, O_WRONLY);
	if (!fd_usr) {
		cifssrv_err("cifssrv is not available\n");
		return CIFS_FAIL;
	}

	while (!eof) {
		size_t c_len;

		len = get_entry(fd_db, &lstr, &eof);
		if (len < 0)
			goto out2;

		init_2_strings(lstr, &usr, &pwd, len);
		if (usr && pwd) {
			len = strlen(usr);
			c_len = len + CIFS_NTHASH_SIZE + 2;

			construct = (char *)malloc(c_len);
			if (!construct)
				goto out;

			memset(construct, 0, c_len);
			memcpy(construct, usr, len);
			memcpy(construct + len, ":", 1);
			memcpy(construct + len + 1, pwd, 16);

			if (write(fd_usr, construct,  c_len - 1) !=
					c_len - 1) {
				cifssrv_err("cifssrv is not available\n");
				goto out;
			}
			free(usr);
			free(pwd);
			free(construct);
		}
		free(lstr);
	}

	close(fd_usr);
	close(fd_db);
	return CIFS_SUCCESS;

out:
	free(lstr);
	free(usr);
	free(pwd);
	free(construct);
out2:
	close(fd_usr);
	close(fd_db);

	return CIFS_FAIL;
}

/**
 * alloc_new_share() - allocate new share
 *
 * Return:	success: allocated share; fail: NULL
 */
static struct cifssrv_share *alloc_new_share(void)
{
	struct cifssrv_share *share = NULL;
	share = (struct cifssrv_share *) calloc(1,
			sizeof(struct cifssrv_share));
	if (!share)
		return NULL;

	share->sharename = (char *) calloc(1, SHARE_MAX_NAME_LEN);
	if (!share->sharename) {
		free(share);
		return NULL;
	}

	share->config.comment = (char *) calloc(1, SHARE_MAX_COMMENT_LEN);
	if (!share->config.comment) {
		free(share);
		free(share->sharename);
		return NULL;
	}

	INIT_LIST_HEAD(&share->list);
	return share;
}

/**
 * add_new_share() - add newly allocated share in global share list
 * @sharename:	share name string
 * @comment:	comment decribing share
 */
static void add_new_share(char *sharename, char *comment)
{
	struct cifssrv_share *share;

	share = (struct cifssrv_share *)alloc_new_share();
	if (!share)
		return;

	if (sharename)
		memcpy(share->sharename, sharename, strlen(sharename));

	if (share->config.comment)
		memcpy(share->config.comment, comment, strlen(comment));

	list_add(&share->list, &cifssrv_share_list);
	cifssrv_num_shares++;
}

/**
 * exit_share_config() - destroy share list
 */
static void exit_share_config(void)
{
	struct cifssrv_share *share;
	struct list_head *tmp, *t;

	list_for_each_safe(tmp, t, &cifssrv_share_list) {
		share = list_entry(tmp, struct cifssrv_share, list);
		list_del(&share->list);
		cifssrv_num_shares--;
		free(share->config.comment);
		free(share->sharename);
		free(share);
	}
}

/**
 * init_share_config() - initialize global share list head and
 *			add IPC$ share
 */
static void init_share_config(void)
{
	INIT_LIST_HEAD(&cifssrv_share_list);
	add_new_share(STR_IPC, "IPC$ share");
	strncpy(workgroup, STR_WRKGRP, strlen(STR_WRKGRP));
	strncpy(server_string, STR_SRV_NAME, strlen(STR_SRV_NAME));
}

/**
 * parse_global_config() - parse global share config
 *
 * @src:	source string to be scanned
 */
static void parse_global_config(char *src)
{
	char *tmp;
	char *dup;
	char *conf;
	char *val;
	char *sstring = NULL;
	char *workgrp = NULL;

	if (!src)
		return;

	tmp = dup = strdup(src);
	conf = strtok(dup, "<");
	if (!conf)
		goto out;

	do {
		if (!strncasecmp("server string =", conf, 15)) {
			val = strchr(conf, '=');
			if (val)
				sstring = val + 2;
		}
		else if (!strncasecmp("workgroup =", conf, 11)) {
			val = strchr(conf, '=');
			if (val)
				workgrp = val + 2;
		}
	}while((conf = strtok(NULL, "<")));

	if (sstring)
		strncpy(server_string, sstring, MAX_SERVER_NAME_LEN - 1);

	if (workgrp)
		strncpy(workgroup, workgrp, MAX_SERVER_WRKGRP_LEN - 1);

out:
	free(tmp);
}

/**
 * parse_share_config() - parse share config entry for sharename and
 *			comment for dcerpc
 *
 * @src:	source string to be scanned
 */
static void parse_share_config(char *src)
{
	char *tmp;
	char *dup;
	char *conf;
	char *val;
	char *sharename = NULL;
	char *comment = NULL;

	if (!src)
		return;

	cifssrv_debug("%s\n\n", src);

	if (strcasestr(src, "sharename = global")) {
		parse_global_config(src);
		return;
	}

	tmp = dup = strdup(src);
	conf = strtok(dup, "<");
	if (!conf)
		goto out;

	do {
		if (!strncasecmp("sharename =", conf, 11)) {
			val = strchr(conf, '=');
			if (val)
				sharename = val + 2;
		}
		else if (!strncasecmp("comment =", conf, 9)) {
			val = strchr(conf, '=');
			if (val)
				comment = val + 2;
		}
	}while((conf = strtok(NULL, "<")));

	if (sharename)
		add_new_share(sharename, comment);

out:
	free(tmp);
}

/**
 * prefix_share_name() - add prefix to share name for simple parsing
 * @src:	source string to be scanned
 * @srclen:	lenth of share name
 */
void prefix_share_name(char *src, int *srclen)
{
	char *share_cfg = "sharename = ";
	char share_name[PAGE_SZ];
	int i, j;

	/* remove [ and ] from share name */
	for (i = 0, j = 0; i < *srclen; i++) {
		if (!(src[i] == '[' || src[i] == ']'))
			share_name[j++] = src[i];
	}
	share_name[j] = '\0';

	strncpy(src, share_cfg,  strlen(share_cfg));
	strncat(src, share_name, strlen(share_name));
	*srclen = strlen(src);
}

/**
 * validate_share_path() - check if share path exist or not
 * @path:	share path name string
 * @sname:	share name string
 *
 * Return:	0 on success ortherwise error
 */
int validate_share_path(char *path, char *sname)
{
	struct stat st;

	if (stat(path, &st) == -1) {
		fprintf(stderr, "Failed to add SMB %s \t", sname);
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -errno;
	}

	return 0;
}

/**
 * get_share_path() - get share path for a share
 *
 * @dst:	destination buffer for share path
 * @src:	source string to be scanned
 * @sharename:	share name string
 * Return:	lenth of share path on success otherwise zero or error
 */
int get_share_path(char *dst, char *src, char *sharename)
{
	char *pname;
	char *tmp, *dup;
	int len;
	int ret;

	if (!src || !dst)
		return 0;

	/* global config does not have share path */
	if (strcasestr(sharename, "sharename = global"))
		return 0;

	tmp = dup = strdup(src);
	if (strcasestr(dup, "path = "))
	{
		pname = strtok(dup, "= ");
		pname = strtok(NULL, "= ");
		if (pname)
		{
			len = strlen(pname);
			strncpy(dst, pname, len);
			dst[len] = '\0';
			free(tmp);
			ret = validate_share_path(dst, sharename);
			if (ret < 0)
				return ret;
			else
				return len;
		}
	}

	free(tmp);
	return 0;
}

/**
 * getfchar() - helper function to locate valid starting character
 *              and copies characters till i/p LINE length.
 *              Here valid data means:
 *              i) not commented line (starting with ';' or '#')
 *              ii) ascii values between- a-z || A-Z || 0-9
 * @sz: current LINE length
 * @c:          first valid character
 * @dst:        initialize destination string with LINE data starting from 'c'
 * @ssz:        total length of copied destination data
 */
void getfchar(char *LINE, int sz, char *c, char *dst, int *ssz)
{
	int cnt = 0;
	int i = 0;
	int len = 0;

	while ((cnt < sz) && ((LINE[cnt] != ';') &&
	       (LINE[cnt] != '#') && (LINE[cnt] != '[') &&
	       !(LINE[cnt] >= 'A' && LINE[cnt] <= 'Z') &&
	       !(LINE[cnt] >= 'a' && LINE[cnt] <= 'z') &&
	       !(LINE[cnt] >= '0' && LINE[cnt] <= '9')))
		cnt++;

	cnt == sz ? (*c = ' ') : (*c = LINE[cnt]);

	if ((LINE[cnt] != ';') && (LINE[cnt] != '#')) {
		while ((cnt < sz) &&
				(LINE[cnt] != ';') &&
				(LINE[cnt] != '#')) {
			dst[i++] = LINE[cnt++];
			len++;
		}
	}

	*ssz = len;
}

/**
 * config_shares() - function to initialize cifssrv with share settings.
 *		     This function parses local configuration file and
 *		     initializes cifssrv with [share] settings
 *
 * Return:	success: CIFS_SUCCESS; fail: CIFS_FAIL
 */
int config_shares(char *conf_path)
{
	char lshare[PAGE_SZ] = "";
	char sharepath[PAGE_SZ] = "";
	char tbuf[PAGE_SZ];
	int sharepath_len = 0;
	int cnt = 0, lssz = 0, limit = 0, eof = 0, sz;
	int fd_conf;
	FILE *fd_share;

	fd_share = fopen(conf_path, "r");
	if (!fd_share) {
		cifssrv_err("[%s] is not existing, installing, err %d\n",
				conf_path, errno);
		return CIFS_FAIL;
	}

	fd_conf = open(PATH_CIFSSRV_CONFIG, O_WRONLY);
	if (fd_conf < 0) {
		cifssrv_err("cifssrv is not available, err %d\n", errno);
		fclose(fd_share);
		return CIFS_FAIL;
	}

	memset(tbuf, 0, PAGE_SZ);

	while (!eof) {
		char ch;
		char stro[PAGE_SZ] = "";
		char str[PAGE_SZ] = "";
		char cont_str[PAGE_SZ] = "";
		int contsz = 0, ssz = 0;
		char *line;

		cnt = readline(fd_share, &line, &eof, 1);
		if (cnt < 0)
			goto out;
		else if (!cnt) {
			free(line);
			continue;
		}

		if (line[cnt - 1] == '\\') {
			do {
				strncat(cont_str, line, cnt - 1);
				free(line);
				cnt = readline(fd_share, &line, &eof, 1);
			} while ((cnt > 0) && (line[cnt - 1] == '\\'));

			if (cnt > 0)
				strncat(cont_str, line, cnt);
			free(line);

			contsz = strlen(cont_str);
			line = (char *)malloc(contsz + 1);
			memset(line, 0, contsz + 1);
			strncpy(line, cont_str, contsz);
			cnt = contsz;
		}

		getfchar(line, cnt, &ch, stro, &ssz);
		free(line);
		tlws(stro, str, &ssz);

		if ((ch == '[') || (ch >= 'A' && ch <= 'Z') ||
		    (ch >= 'a' && ch <= 'z')) {
			/* writeout previous export entry */
			if (ch == '[' && limit > 0) {
				if (sharepath_len >= 0) {
					tbuf[limit] = '\0';
					limit += 1;
					lseek(fd_conf, 0, SEEK_SET);
					sz = write(fd_conf, tbuf,
							limit);
					if (sz != limit)
						perror("config error");
					else
						parse_share_config(tbuf);
				}

				memset(tbuf, 0, PAGE_SZ);
				limit = 0;
				sharepath_len = 0;
			}

			if (ch == '[') {
				prefix_share_name(str, &ssz);
				memset(lshare, 0, PAGE_SZ);
				strncpy(lshare, str, ssz);
				lssz = ssz;
			}

			if (!sharepath_len)
				sharepath_len =
					get_share_path(sharepath, str, lshare);
again:
			if ((limit + ssz + 1) < PAGE_SZ) {
				strncat(tbuf+limit, "<", 1);
				strncat(tbuf+limit + 1, str, ssz);
				limit += ssz + 1;
			} else {
				if (sharepath_len >= 0) {
					tbuf[limit] = '\0';
					limit += 1;
					lseek(fd_conf, 0, SEEK_SET);
					sz = write(fd_conf, tbuf,
							limit);
					if (sz != limit)
						perror("config error");
				}

				memset(tbuf, 0, PAGE_SZ);

				if (ch != '[') {
					strncat(tbuf, "<", 1);
					strncat(tbuf + 1, lshare, lssz);
					limit = lssz + 1;
				} else {
					sharepath_len = 0;
					limit = 0;
				}

				goto again;
			}
		}
	}

out:
	if (sharepath_len >= 0 && limit > 0) {
		tbuf[limit] = '\0';
		limit += 1;

		lseek(fd_conf, 0, SEEK_SET);
		sz = write(fd_conf, tbuf, limit);
		if (sz != limit) {
			/* retry once again */
			sleep(1);
			lseek(fd_conf, 0, SEEK_SET);
			sz = write(fd_conf, tbuf, limit);
			if (sz != limit) {
				perror("write error");
				cifssrv_err(": <write=%d> <req=%d>\n", sz, limit);
			}
		}
		else
			parse_share_config(tbuf);

		sharepath_len = 0;
	}

	fclose(fd_share);
	close(fd_conf);

	return CIFS_SUCCESS;
}

int main(int argc, char**argv)
{
	char *cifspwd = PATH_PWDDB;
	char *cifsconf = PATH_SHARECONF;
	int c;
	int ret;

	/* Parse the command line options and arguments. */
	opterr = 0;
	while ((c = getopt(argc, argv, "c:i:vh")) != EOF)
		switch (c) {
		case 'c':
			cifsconf = strdup(optarg);
			break;
		case 'i':
			cifspwd = strdup(optarg);
			break;
		case 'v':
			if (argc <= 2) {
				printf("[option] needed with verbose\n");
				usage();
			}
			vflags |= F_VERBOSE;
			break;
		case '?':
		case 'h':
		default:
			usage();
	}

	init_share_config();

	/* import user account */
	ret = config_users(cifspwd);
	if (ret != CIFS_SUCCESS)
		goto out;

	/* import shares info */
	ret = config_shares(cifsconf);
	if (ret != CIFS_SUCCESS)
		goto out;

	//cifssrv_debug("cifssrvd version : %d\n", cifssrvd_version);

	/* netlink communication loop */
	cifssrvd_netlink_setup();

	exit_share_config();

out:
	cifssrv_debug("cifssrvd terminated\n");
	
	exit(1);
}
