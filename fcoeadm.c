/*
 * Copyright(c) 2010 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Maintained at www.Open-FCoE.org
 */

#include <libgen.h>
#include <paths.h>
#include <net/if.h>
#include <sys/un.h>

#include "fcoe_utils_version.h"
#include "fcoeadm.h"
#include "fcoe_clif.h"

static struct option fcoeadm_opts[] = {
	{"create", 1, 0, 'c'},
	{"destroy", 1, 0, 'd'},
	{"reset", 1, 0, 'r'},
	{"interface", 1, 0, 'i'},
	{"target", 1, 0, 't'},
	{"lun", 2, 0, 'l'},
	{"stats", 1, 0, 's'},
	{"help", 0, 0, 'h'},
	{"version", 0, 0, 'v'},
	{0, 0, 0, 0}
};

struct opt_info _opt_info, *opt_info = &_opt_info;
char progname[20];

static void fcoeadm_help(void)
{
	printf("Version %s\n", FCOE_UTILS_VERSION);
	printf("Usage: %s\n"
	       "\t [-c|--create] <ethX>\n"
	       "\t [-d|--destroy] <ethX>\n"
	       "\t [-r|--reset] <ethX>\n"
	       "\t [-i|--interface] [<ethX>]\n"
	       "\t [-t|--target] [<ethX>]\n"
	       "\t [-l|--lun] [<target port_id> [<lun_id>]]\n"
	       "\t [-s|--stats] <ethX> [-n <interval>]\n"
	       "\t [-v|--version]\n"
	       "\t [-h|--help]\n\n", progname);
}

/*
 * TODO - check this ifname before performing any action
 */
static int fcoeadm_check(char *ifname)
{
	char path[256];
	int fd;
	int status = 0;

	/* check if we have sysfs */
	if (fcoe_checkdir(SYSFS_MOUNT)) {
		fprintf(stderr,
			"%s: Sysfs mount point %s not found\n",
			progname, SYSFS_MOUNT);
		status = -EINVAL;
	}

	/* check target interface */
	if (valid_ifname(ifname)) {
		fprintf(stderr, "%s: Invalid interface name\n", progname);
		status = -EINVAL;
	}
	sprintf(path, "%s/%s", SYSFS_NET, ifname);
	if (fcoe_checkdir(path)) {
		fprintf(stderr,
			"%s: Interface %s not found\n", progname, ifname);
		status = -EINVAL;
	}

	fd = open(CLIF_PID_FILE, O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		fprintf(stderr,
			"%s: fcoemon was not running\n", progname);
		status = -EINVAL;
	}

	return status;
}

static int fcoeadm_clif_request(struct clif_sock_info *clif_info,
				const struct clif_data *cmd, size_t cmd_len,
				char *reply, size_t *reply_len)
{
	struct timeval tv;
	int ret;
	fd_set rfds;

	if (send(clif_info->socket_fd, cmd, cmd_len, 0) < 0)
		return -1;

	for (;;) {
		tv.tv_sec = CLIF_CMD_RESPONSE_TIMEOUT;
		tv.tv_usec = 0;
		FD_ZERO(&rfds);
		FD_SET(clif_info->socket_fd, &rfds);
		ret = select(clif_info->socket_fd + 1, &rfds, NULL, NULL, &tv);
		if (FD_ISSET(clif_info->socket_fd, &rfds)) {
			ret = recv(clif_info->socket_fd, reply, *reply_len, 0);
			if (ret < 0)
				return ret;
			*reply_len = ret;
			break;
		} else {
			return -2;
		}
	}

	return 0;
}

/*
 * TODO: What is this returning? A 'enum clif_status'?
 */
static int fcoeadm_request(struct clif_sock_info *clif_info,
			   struct clif_data *data)
{
	char rbuf[MAX_MSGBUF];
	size_t len;
	int ret;

	len = sizeof(rbuf)-1;

	ret = fcoeadm_clif_request(clif_info, data, sizeof(struct clif_data),
				   rbuf, &len);
	if (ret == -2) {
		fprintf(stderr, "Command timed out\n");
		goto fail;
	} else if (ret < 0) {
		fprintf(stderr, "Command failed\n");
		goto fail;
	}

	rbuf[len] = '\0';
	ret = atoi(rbuf);
	return ret;

fail:
	return -EINVAL;
}

static void fcoeadm_close_cli(struct clif_sock_info *clif_info)
{
	unlink(clif_info->local.sun_path);
	close(clif_info->socket_fd);
}

/*
 * Create fcoeadm client interface
 */
static int fcoeadm_open_cli(struct clif_sock_info *clif_info)
{
	int counter;
	int rc = 0;

	clif_info->socket_fd = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (clif_info->socket_fd < 0) {
		/* Error code is returned through errno */
		rc = errno;
		goto err;
	}

	clif_info->local.sun_family = AF_UNIX;
	snprintf(clif_info->local.sun_path, sizeof(clif_info->local.sun_path),
		 "/tmp/fcadm_clif_%d-%d", getpid(), counter++);

	if (bind(clif_info->socket_fd, (struct sockaddr *)&clif_info->local,
		 sizeof(clif_info->local)) < 0) {
		/* Error code is returned through errno */
		rc = errno;
		goto err_close;
	}

	clif_info->dest.sun_family = AF_UNIX;
	strncpy(clif_info->dest.sun_path, CLIF_SOCK_FILE,
		sizeof(clif_info->dest.sun_path));

	if (!connect(clif_info->socket_fd, (struct sockaddr *)&clif_info->dest,
		     sizeof(clif_info->dest)) < 0) {
		/* Error code is returned through errno */
		rc = errno;
		unlink(clif_info->local.sun_path);
		goto err_close;
	}

err:
	return rc;

err_close:
	close(clif_info->socket_fd);
	return rc;
}

/*
 * Send request to fcoemon
 */
/*
 * TODO: This is wrong. Which is this routine returning
 * 'enum clif_status' or an -ERROR?
 */
int fcoeadm_action(enum clif_action cmd, char *ifname)
{
	struct clif_data data;
	struct clif_sock_info clif_info;
	int rc = 0;

	strncpy(data.ifname, ifname, sizeof(data.ifname));
	data.cmd = cmd;

	rc = fcoeadm_open_cli(&clif_info);
	if (!rc) {
		rc = fcoeadm_request(&clif_info, &data);
		fcoeadm_close_cli(&clif_info);
	}

	return rc;
}

/*
 * Create FCoE instance for this ifname
 */
static int fcoeadm_create(char *ifname)
{
	if (fcoeadm_check(ifname)) {
		fprintf(stderr,
			"%s: Failed to create FCoE instance on %s\n",
			progname, ifname);
		return -EINVAL;
	}
	return fcoeadm_action(CLIF_CREATE_CMD, ifname);
}

/*
 * Remove FCoE instance for this ifname
 */
static int fcoeadm_destroy(char *ifname)
{
	if (fcoeadm_check(ifname)) {
		fprintf(stderr,
			"%s: Failed to destroy FCoE instance on %s\n",
			progname, ifname);
		return -EINVAL;
	}
	return fcoeadm_action(CLIF_DESTROY_CMD, ifname);
}

/*
 * Reset the fc_host that is associated w/ this ifname
 */
static int fcoeadm_reset(char *ifname)
{
	if (fcoeadm_check(ifname)) {
		fprintf(stderr,
			"%s: Failed to reset FCoE instance on %s\n",
			progname, ifname);
		return -EINVAL;
	}
	return fcoeadm_action(CLIF_RESET_CMD, ifname);
}

/*
 * Parse a user-entered hex field.
 * Format may be xx-xx-xx OR xxxxxx OR xx:xx:xx for len bytes (up to 8).
 * Leading zeros may be omitted.
 */
static int parse_hex_ll(unsigned long long *hexp, const char *input, u_int len)
{
	int i;
	unsigned long long hex = 0;
	unsigned long long byte;
	char *endptr = "";
	int error = EINVAL;
	char sep = 0;

	for (i = 0; i < len; i++) {
		byte = strtoull(input, &endptr, 16);
		if (i == 0 && *endptr == '\0') {
			hex = byte;
			if (len == 8 || hex < (1ULL << (8 * len)))
				error = 0;
			break;
		}
		if (sep == 0 && (*endptr == ':' || *endptr == '-'))
			sep = *endptr;
		if ((*endptr == '\0' || *endptr == sep) && byte < 256)
			hex = (hex << 8) | byte;
		else
			break;
		input = endptr + 1;
	}
	if (i == len && *endptr == '\0')
		error = 0;
	if (error == 0)
		*hexp = hex;
	return error;
}

static int parse_fcid(HBA_UINT32 *fcid, const char *input)
{
	int rc;
	unsigned long long hex;

	rc = parse_hex_ll(&hex, input, 3);
	if (rc == 0)
		*fcid = (HBA_UINT32) hex;
	return rc;
}

static int fcoeadm_loadhba()
{
	if (HBA_STATUS_OK != HBA_LoadLibrary()) {
		fprintf(stderr, "Failed to load Linux HBAAPI library! Please "
			"verify the hba.conf file is set up correctly.\n");
		return -EINVAL;
	}
	return 0;
}


/*
 * Display adapter information
 */
static int fcoeadm_display_adapter_info(struct opt_info *opt_info)
{
	if (fcoeadm_loadhba())
		return -EINVAL;

	display_adapter_info(opt_info);

	HBA_FreeLibrary();
	return 0;
}

/*
 * Display target information
 */
static int fcoeadm_display_target_info(struct opt_info *opt_info)
{
	if (fcoeadm_loadhba())
		return -EINVAL;

	display_target_info(opt_info);

	HBA_FreeLibrary();
	return 0;
}

/*
 * Display port statistics
 */
static int fcoeadm_display_port_stats(struct opt_info *opt_info)
{
	if (!opt_info->s_flag)
		return -EINVAL;

	if (!opt_info->n_flag)
		opt_info->n_interval = DEFAULT_STATS_INTERVAL;

	if (fcoeadm_loadhba())
		return -EINVAL;

	display_port_stats(opt_info);

	HBA_FreeLibrary();
	return 0;
}

#define MAX_ARG_LEN 32

int main(int argc, char *argv[])
{
	char *s;
	int opt, rc = -1;

	strncpy(progname, basename(argv[0]), sizeof(progname));
	memset(opt_info, 0, sizeof(*opt_info));

	while ((opt = getopt_long(argc, argv, "c:d:r:itls:n:hv",
				  fcoeadm_opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			if ((argc < 2 || argc > 3) ||
			    strnlen(optarg, MAX_ARG_LEN) > (IFNAMSIZ - 1) ||
			    ((argc == 3) && strnlen(argv[1], MAX_ARG_LEN) > 2 &&
			     argv[1][1] != '-'))
				goto error;
			rc = fcoeadm_create(optarg);
			goto done;
		case 'd':
			if ((argc < 2 || argc > 3) ||
			    strnlen(optarg, MAX_ARG_LEN) > (IFNAMSIZ - 1) ||
			    ((argc == 3) && strnlen(argv[1], MAX_ARG_LEN) > 2 &&
			     argv[1][1] != '-'))
				goto error;
			rc = fcoeadm_destroy(optarg);
			goto done;
		case 'r':
			if ((argc < 2 || argc > 3) ||
			    strnlen(optarg, MAX_ARG_LEN) > (IFNAMSIZ - 1) ||
			    ((argc == 3) && strnlen(argv[1], MAX_ARG_LEN) > 2 &&
			     argv[1][1] != '-'))
				goto error;
			rc = fcoeadm_reset(optarg);
			goto done;
		case 'i':
			if (argc < 2 || argc > 3 ||
			    (argc == 3 && strnlen(argv[1], MAX_ARG_LEN) > 2 &&
			     (argv[1][1] != '-' || strchr(argv[1], '=')
			      != NULL)))
				goto error;
			s = NULL;
			if (argc == 2) {
				if (argv[1][1] == '-')
					s = strchr(argv[1], '=')+1;
				else
					s = argv[1]+2;
			} else
				s = argv[2];

			if (s) {
				if (strnlen(s, MAX_ARG_LEN) > (IFNAMSIZ - 1))
					goto error;
				strncpy(opt_info->ifname, s,
					sizeof(opt_info->ifname));
			}
			if (strnlen(opt_info->ifname, IFNAMSIZ - 1)) {
				if (fcoe_validate_interface(opt_info->ifname))
					goto done;
			}
			opt_info->a_flag = 1;
			rc = fcoeadm_display_adapter_info(opt_info);
			goto done;
		case 't':
			if (argc < 2 || argc > 3 ||
			    (argc == 3 && strnlen(argv[1], MAX_ARG_LEN) > 2 &&
			     (argv[1][1] != '-' || strchr(argv[1], '=')
			      != NULL)))
				goto error;
			s = NULL;
			if (argc == 2) {
				if (argv[1][1] == '-')
					s = strchr(argv[1], '=')+1;
				else
					s = argv[1]+2;
			} else {
				s = argv[2];
			}
			if (s) {
				if (strnlen(s, MAX_ARG_LEN) > (IFNAMSIZ - 1))
					goto error;
				strncpy(opt_info->ifname, s,
					sizeof(opt_info->ifname));
			}
			if (strnlen(opt_info->ifname, IFNAMSIZ - 1)) {
				if (fcoe_validate_interface(opt_info->ifname))
					goto done;
			}
			opt_info->t_flag = 1;
			rc = fcoeadm_display_target_info(opt_info);
			goto done;
		case 'l':
			if (argc < 2 || argc > 4)
				goto error;
			if (optarg) {
				if (parse_fcid(&opt_info->l_fcid, optarg))
					goto error;
				opt_info->l_fcid_present = 1;
				if (argv[optind]) {
					opt_info->l_lun_id = atoi(argv[optind]);
					opt_info->l_lun_id_present = 1;
				}
			}
			opt_info->l_flag = 1;
			rc = fcoeadm_display_target_info(opt_info);
			goto done;
		case 's':
			if ((argc < 2 || argc > 5) ||
			    strnlen(optarg, MAX_ARG_LEN) > (IFNAMSIZ - 1))
				goto error;
			if (optarg)
				strncpy(opt_info->ifname, optarg,
					sizeof(opt_info->ifname));
			if (strnlen(opt_info->ifname, IFNAMSIZ - 1)) {
				if (fcoe_validate_interface(opt_info->ifname))
					goto done;
			}
			opt_info->s_flag = 1;
			if (argv[optind] && !strncmp(argv[optind], "-n", 2))
				break;
			goto stats;
		case 'n':
			if (!opt_info->s_flag)
				goto error;
			opt_info->n_interval = atoi(optarg);
			if (opt_info->n_interval <= 0)
				goto error;
			if (argv[optind] &&
			    strnlen(argv[optind], MAX_ARG_LEN<<1) > MAX_ARG_LEN)
				goto error;
			opt_info->n_flag = 1;
			goto stats;
		case 'v':
			if (argc != 2)
				goto error;
			printf("%s\n", FCOE_UTILS_VERSION);
			goto done;
		case 'h':
		default:
			if (argc != 2)
				goto error;
			fcoeadm_help();
			exit(-EINVAL);
		}
	}
	goto error;

stats:
	if (!fcoeadm_display_port_stats(opt_info))
		goto done;

error:
	fprintf(stderr, "%s: Invalid command options\n", progname);
	fcoeadm_help();
	exit(-EINVAL);

done:
	return rc;
}
