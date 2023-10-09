/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2023 Xilinx, Inc. */

/**
 * @file   sfptpd_main.c
 * @brief  Entry point fot the sfptpd application
 */

#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <pthread.h>
#include <unistd.h>

#include "sfptpd_app.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_logging.h"
#include "sfptpd_engine.h"
#include "sfptpd_clock.h"
#include "sfptpd_interface.h"
#include "sfptpd_constants.h"
#include "sfptpd_message.h"
#include "sfptpd_thread.h"
#include "sfptpd_control.h"
#include "sfptpd_link.h"
#include "sfptpd_netlink.h"
#include "sfptpd_statistics.h"
#include "sfptpd_multicast.h"

#ifdef HAVE_CAPS
#include <sys/capability.h>
#endif


/****************************************************************************
 * Types and Defines
 ****************************************************************************/

#define  ARRAY_SIZE(a)   (sizeof (a) / sizeof (a [0]))


/****************************************************************************
 * Local Data
 ****************************************************************************/

static const char *lock_filename = "/var/run/kernel_clock";
static const mode_t lock_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

#ifdef HAVE_CAPS
static const cap_value_t caps_essential[] = {
	CAP_SYS_TIME,
	CAP_NET_BIND_SERVICE,
	CAP_NET_ADMIN,
	CAP_NET_RAW,
};

static const cap_value_t caps_for_root[] = {
	CAP_SYS_TIME,
	CAP_NET_BIND_SERVICE,
	CAP_NET_ADMIN,
	CAP_NET_RAW,
	CAP_DAC_OVERRIDE, // Access devices
};
#endif

static struct sfptpd_config *config = NULL;
static struct sfptpd_engine *engine = NULL;
static struct sfptpd_nl_state *netlink = NULL;
const static struct sfptpd_link_table *initial_link_table = NULL;

/* The hardware state lock protects data structures that shadow
   the state of the hardware so that they are internally consistent.
   The clock and interface modules use this in their public APIs. It is
   'recursive' so it can be acquired by its current owner, meaning the public
   API is re-entrant.
 */
static pthread_mutex_t hardware_state_lock;


/****************************************************************************
 * Local Functions
 ****************************************************************************/


#ifdef HAVE_CAPS
static int claim_drop_privilege(struct sfptpd_config *config, uid_t user)
{
	const cap_value_t *required;
	int num_caps;
	char *cap_str = NULL;
	cap_t caps = NULL;
	int ret = EACCES;
	int rc;
	int i;
	sfptpd_config_general_t *gconf;

	assert(config != NULL);

	gconf = sfptpd_general_config_get(config);

	/* If running as root we expect to be able to access devices owned
	   by any user. */
	if (gconf->uid == 0 && user == 0) {
		required = caps_for_root;
		num_caps = ARRAY_SIZE(caps_for_root);
	} else {
		required = caps_essential;
		num_caps = ARRAY_SIZE(caps_essential);
	}

	caps = cap_init();
	if (caps == NULL) {
		CRITICAL("could not allocate capabilities object: %s\n",
			 strerror(errno));
		return EACCES;
	}

	for (i = 0; i < num_caps; i++) {
		rc = cap_set_flag(caps, CAP_EFFECTIVE, 1, &required[i], CAP_SET);
		if (rc == -1) {
			CRITICAL("could not set effective capability %d flag: %s\n",
				 required[i], strerror(errno));
			goto finish;
		}
		rc = cap_set_flag(caps, CAP_PERMITTED, 1, &required[i], CAP_SET);
		if (rc == -1) {
			CRITICAL("could not set permitted capability %d flag: %s\n",
				 required[i], strerror(errno));
			goto finish;
		}
	}

	cap_str = cap_to_text(caps, NULL);
	rc = cap_set_proc(caps);

	if (rc == -1) {
		CRITICAL("could not acquire necessary capabilities %s%s %s\n",
			 cap_str ? cap_str : "?",
			 user != 0 ? ". Try running sfptpd as root:" : ":",
			 strerror(errno));
	} else {
		ret = 0;
		TRACE_L3("%s capabilities %s\n",
			 user == 0 ? "retained" : "acquired",
			 cap_str ? cap_str : "?");
	}

finish:
	if (cap_str != NULL)
		cap_free(cap_str);

	if (caps != NULL)
		cap_free(caps);

	return ret;
}


static int drop_user(struct sfptpd_config *config)
{
	int rc;
	sfptpd_config_general_t *gconf;

	assert(config != NULL);

	gconf = sfptpd_general_config_get(config);

	if (gconf->gid != 0 || gconf->uid != 0 ) {
		rc = prctl(PR_SET_KEEPCAPS, 1);
		if (rc == -1)
			CRITICAL("failed to keep capabilities via prctl: %s\n",
				 strerror(errno));
	}

	if (gconf->uid != 0) {
		TRACE_L4("joining %d groups\n", gconf->num_groups);
		rc = setgroups(gconf->num_groups, gconf->groups);
		if (rc != 0) {
			rc = errno;
			CRITICAL("could not set group list: %s\n", strerror(rc));
			return rc;
		}
	}

	if (gconf->gid != 0) {
		INFO("dropping to group %d\n", gconf->gid);
		rc = setresgid(gconf->gid, gconf->gid, gconf->gid);
		if (rc == -1) {
			rc = errno;
			CRITICAL("could not drop group to gid %d: %s\n",
				 gconf->gid, strerror(rc));
			return rc;
		}
	}

	if (gconf->uid != 0) {
		INFO("dropping to user %d\n", gconf->uid);
		NOTICE("for hotplugged network interfaces, udev rules must "
		       "give access to corresponding /dev/{ptp*,pps*} devices "
		       "for the user or group running sfptpd\n");
		rc = setresuid(gconf->uid, gconf->uid, gconf->uid);
		if (rc == -1) {
			rc = errno;
			CRITICAL("could not drop user to uid %d: %s\n",
				 gconf->uid, strerror(rc));
			return rc;
		}
	}

	return 0;
}
#endif


static int runtime_checks(struct sfptpd_config *config)
{
	FILE *clock_src;
	char source[10] = "";
	struct utsname name;
	int rc = 0;

	assert(config != NULL);

	rc = uname(&name);
#if defined(__i386__)
	/* If this is a 32bit binary, check that we are running on a 32bit kernel */
	if (rc == -1) {
		CRITICAL("could not determine system characteristics with uname: %s\n",
			 strerror(errno));
		return ENOEXEC;
	} else if (strcmp(name.machine, "i686") != 0) {
		CRITICAL("32-bit sfptpd not compatible with 64-bit kernel\n");
		return ENOEXEC;
	}
#endif

	/* sfptpd has to be run as root. */
	if (geteuid() != 0) {
#ifdef HAVE_CAPS
		WARNING("sfptpd normally needs to be launched as root. "
			"Attempting to run with available capabilities and "
			"permissions.\n");
#else
		CRITICAL("sfptpd must be run as root\n");
		return EACCES;
#endif
	}

	if (sfptpd_general_config_get(config)->lock) {
		struct sfptpd_prog competitors[] = {
			{ "ptpd*" },
			{ "sfptpd" },
			{ NULL }
		};

		if (sfptpd_find_running_programs(competitors) != 0) {
			struct sfptpd_prog *prog;

			for (prog = &competitors[0]; prog->pattern; prog++) {
				if (prog->matches > 0)
					CRITICAL("%s is already running (%d)\n",
						 prog->a_program, prog->a_pid);
			}

			return EBUSY;
		}
	}

	if (strcmp(name.machine, "x86_64") == 0) {
		/* If the amd64 kernel isn't using TSC, print a warning but don't abort. */
		clock_src = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
		if (clock_src == NULL ||
		    fgets(source, sizeof source, clock_src) == NULL ||
		    strcmp(source, "tsc\n") != 0) {
			*strchrnul(source, '\n') = '\0';
			WARNING("system clock source should be set to TSC for stability; %s: %s\n",
				source[0] ? "current source is" : "could not determine current source",
				source[0] ? source : strerror(errno));
		}
		if (clock_src != NULL)
			fclose(clock_src);
	}

	return rc;
}


static int lock_create(struct sfptpd_config *config, int *lock_fd)
{
	int fd;
	char pid[16];
	struct flock file_lock = {
		.l_type = F_WRLCK,
		.l_start = 0,
		.l_whence = SEEK_SET,
		.l_len = 0
	};
	sfptpd_config_general_t *gconf;

	assert(config != NULL);
	assert(lock_fd != NULL);
	*lock_fd = -1;
	gconf = sfptpd_general_config_get(config);

	/* If locking is disabled, return straight-away */
	if (!gconf->lock)
		return 0;

	fd = open(lock_filename, O_CREAT | O_RDWR, lock_mode);
	if (fd < 0) {
		CRITICAL("failed to open %s: %s\n", lock_filename, strerror(errno));
		return errno;
	}

	if (fcntl(fd, F_SETLK, &file_lock) < 0) {
		CRITICAL("failed to lock %s: %s\n", lock_filename, strerror(errno));
		close(fd);
		return errno;
	}

	/* Truncating the file does not set the file offset so if the file
	 * already existed (still not unlinked following during daemonize) then
	 * the file offset will not be zero. Explicitly set the seek position
	 * back to the start of the file. Ignore unlikely errors at this stage. */
	if (-1 == ftruncate(fd, 0) ||
	    -1 == lseek(fd, 0, SEEK_SET) ||
	    -1 == sprintf(pid, "%ld\n", (long)getpid()) ||
	    -1 == write(fd, pid, strlen(pid)+1)) {
		CRITICAL("failed to write to lock file: %s\n", strerror(errno));
		close(fd);
		return errno;
	}

	if (chown(lock_filename, gconf->uid, gconf->gid))
		WARNING("could not set lock file to uid/gid %d/%d, %s\n",
			gconf->uid, gconf->gid, strerror(errno));

	*lock_fd = fd;
	return 0;
}


static void lock_delete(int lock_fd)
{
	if (lock_fd != -1) {
		close(lock_fd);
		unlink(lock_filename);
	}
}


static int hardware_state_lock_init(void) {
	pthread_mutexattr_t attr;

	pthread_mutexattr_init (&attr);
	pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE_NP);
	int rc = pthread_mutex_init(&hardware_state_lock, &attr);

	if (rc != 0) {
		CRITICAL("could not create hardware state lock\n");
	}

	return rc;
}


static void hardware_state_lock_destroy(void) {
	int rc = pthread_mutex_destroy(&hardware_state_lock);

	if (rc != 0) {
		CRITICAL("could not destroy hardware state lock\n");
	}
}


static int netlink_start(void) {
	int rc;

	netlink = sfptpd_netlink_init();
	if (netlink == NULL) {
		CRITICAL("could not start netlink\n");
		return EINVAL;
	}

	rc = sfptpd_netlink_set_driver_stats(netlink,
					     sfptpd_stats_ethtool_names,
					     SFPTPD_DRVSTAT_MAX);
	if (rc != 0) {
		CRITICAL("registering link stats types, %s\n",
			 strerror(rc));
		return rc;
	}

	rc = sfptpd_netlink_scan(netlink);
	if (rc != 0) {
		CRITICAL("scanning with netlink, %s\n",
			 strerror(rc));
		return rc;
	}

	/* Wait 5 seconds for initial link table */
	initial_link_table = sfptpd_netlink_table_wait(netlink, 1, 5000);
	if (initial_link_table == NULL) {
		CRITICAL("could not get initial link table, %s\n",
			 strerror(errno));
		return errno;
	}

	return 0;
}


static int daemonize(struct sfptpd_config *config, int *lock_fd)
{
	assert(config != NULL);
	assert(lock_fd != NULL);

	/* If not configured to daemonize the app, just return */
	if (!sfptpd_general_config_get(config)->daemon)
		return 0;

	/* To avoid a race condition where the parent does not exit (and
	 * release the lock) before we try to retake the lock, release the
	 * lock before forking the child process */
	lock_delete(*lock_fd);

	if (daemon(0, 1) < 0) {
		CRITICAL("failed to daemonize sfptpd, %s\n", strerror(errno));
		return errno;
	}

	INFO("running as a daemon\n");

	/* If locking is enabled, recreate the lock file with our new PID */
	if (sfptpd_general_config_get(config)->lock)
		return lock_create(config, lock_fd);

	return 0;
}


static int notify_init(int retcode)
{
	const char *failure = "could not notify init supervisor: %s: %s\n";
	struct sockaddr_un addr;
	const char *path;
	FILE *stream;
	int fd;
	int rc;

	if ((path = getenv("NOTIFY_SOCKET")) == NULL)
		return 0;

	if (path[0] != '/' && path[0] != '@') {
		CRITICAL("init notify socket form not handled, change service configuration: %s\n",
			 path);
		return ENOTSUP;
	}

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd == -1) {
		CRITICAL(failure, "socket", strerror(errno));
		return errno;
	}

	addr.sun_family = AF_UNIX;
	if (path[0] == '@') {
		addr.sun_path[0] = '\0';
		sfptpd_strncpy(addr.sun_path + 1, path, sizeof addr.sun_path - 1);
	} else {
		sfptpd_strncpy(addr.sun_path, path, sizeof addr.sun_path);
	}

	rc = connect(fd, (const struct sockaddr *) &addr, sizeof(addr));
	if (rc == -1) {
		CRITICAL(failure, "connect", strerror(errno));
		goto fail;
	}

	stream = fdopen(fd, "w");
	if (stream == NULL) {
		CRITICAL(failure, "fdopen", strerror(errno));
		goto fail;
	}

	if (retcode == 0) {
		fprintf(stream, "READY=1\n");
	} else {
		fprintf(stream, "ERRNO=%d\n", rc);
	}

	fclose(stream);
	return 0;

fail:
	close(fd);
	return errno;
}


static int main_on_startup(void *not_used)
{
	int rc;
	int ret;
	int control_fd;

	rc = sfptpd_multicast_init();
	if (rc != 0) {
		CRITICAL("failed to initialise multicast messaging\n");
		return rc;
	}

	/* Try to open the sysfs PTP directory */
	if (access("/sys/class/ptp/", F_OK) == -1) {
		CRITICAL("failed to open sysfs ptp devices directory (Is your kernel built with PHC support?), "
			 "%s\n", strerror(errno));
		if (sfptpd_general_config_get(config)->ignore_critical[SFPTPD_CRITICAL_NO_PTP_SUBSYSTEM])
			NOTICE("ignoring lack of kernel PTP Hardware Clock subsystem by configuration\n");
		else {
			NOTICE("configure \"ignore_critical: no-ptp-subsystem\" to allow sfptpd to start in spite of this condition\n");
			return errno;
		}
	}

	sfptpd_multicast_publish(SFPTPD_SERVO_MSG_PID_ADJUST);
	sfptpd_multicast_publish(SFPTPD_APP_MSG_DUMP_TABLES);

	/* Configure control socket handling */
	control_fd = sfptpd_control_socket_get_fd();
	if (control_fd == -1) {
		CRITICAL("control: no file descriptor set for the control socket\n");
		return EINVAL;
	}

	rc = sfptpd_thread_user_fd_add(control_fd, true, false);
	if (rc != 0) {
		CRITICAL("control: failed to add control socket to thread epoll set, %s\n",
			 strerror(rc));
		return rc;
	}

	/* Create an instance of the sync-engine using the configuration */
	rc = sfptpd_engine_create(config, &engine, netlink, initial_link_table);

	/* Notify init supervisor */
	ret = notify_init(rc);
	if (rc == 0)
		rc = ret;

	return rc; 
}


static void main_on_shutdown(void *not_used)
{
	/* If we get here we've shutdown due to a terminate or kill signal.
	 * Clean up and exit. */
	if (engine != NULL)
		sfptpd_engine_destroy(engine);
	engine = NULL;

	sfptpd_multicast_unpublish(SFPTPD_APP_MSG_DUMP_TABLES);
	sfptpd_multicast_unpublish(SFPTPD_SERVO_MSG_PID_ADJUST);
	sfptpd_multicast_destroy();
}


static void main_on_signal(void *not_used, int signal_num)
{
	int test_id;

	switch (signal_num) {
	case SIGINT:
	case SIGTERM:
		/* Exit the application without an error */
		NOTICE("received exit signal\n");
		sfptpd_thread_exit(0);
		break;

	case SIGHUP:
		/* Rotate the stats log */
		NOTICE("received SIGHUP: rotating logs\n");
		sfptpd_engine_log_rotate(engine);
		break;

	case SIGUSR1:
		/* Step the clocks to the current offset */
		NOTICE("received SIGUSR1: stepping clocks to current offset\n");
		sfptpd_engine_step_clocks(engine);
		break;

	default:
		/* Handle the test signals. The real-time signal numbers are
		 * not constants so resort to if-else statements */
		test_id = signal_num - SIGRTMIN;
		if (test_id < SFPTPD_TEST_ID_MAX)
			sfptpd_engine_test_mode(engine, test_id, 0, 0, 0);
		break;
	}
}


static void main_on_message(void *not_used, struct sfptpd_msg_hdr *hdr)
{
	assert(hdr != NULL);
 
	switch (sfptpd_msg_get_id(hdr)) {
	case SFPTPD_MSG_ID_THREAD_EXIT_NOTIFY:
	{
		sfptpd_msg_thread_exit_notify_t *msg = (sfptpd_msg_thread_exit_notify_t *)hdr;
		TRACE_L1("sfptpd engine exited with code %d\n", msg->exit_code);
		sfptpd_thread_exit(msg->exit_code);
	}
		break;

	default:
		WARNING("main: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
	}
}


static void main_on_user_fds(void *not_used, unsigned int num_fds, int fds[])
{
	enum sfptpd_control_action action;
	union sfptpd_control_action_parameters param;
	union {
		sfptpd_app_msg_t app;
		sfptpd_servo_msg_t servo;
	} msg;

	/* We only register a single user file descriptor in this thread. */
	assert(num_fds == 1);
	assert(fds[0] == sfptpd_control_socket_get_fd());

	action = sfptpd_control_socket_get_action(&param);
	switch (action) {
	case CONTROL_NOP:
		NOTICE("unrecognised control command\n");
		break;
	case CONTROL_ERROR:
		ERROR("error receiving control command\n");
		break;
	case CONTROL_EXIT:
		NOTICE("received 'exit' control command: exiting application\n");
		sfptpd_thread_exit(0);
		break;
	case CONTROL_LOGROTATE:
		NOTICE("received 'logrotate' control command: rotating logs\n");
		sfptpd_engine_log_rotate(engine);
		break;
	case CONTROL_STEPCLOCKS:
		/* Step the clocks to the current offset */
		NOTICE("received 'stepclocks' control command: stepping clocks to current offset\n");
		sfptpd_engine_step_clocks(engine);
		break;
	case CONTROL_SELECTINSTANCE:
		/* Choose a particular sync instance */
		NOTICE("received 'selectinstance' control command: choosing instance\n");
		sfptpd_engine_select_instance(engine, param.selected_instance);
		break;
	case CONTROL_TESTMODE:
		/* Configurate a test mode */
		NOTICE("received 'testmode' control command: configuring test mode\n");
		sfptpd_engine_test_mode(engine,
					param.test_mode.id,
					param.test_mode.params[0],
					param.test_mode.params[1],
					param.test_mode.params[2]);
		break;
	case CONTROL_DUMPTABLES:
		/* Dump diagnostic tables */
		NOTICE("received 'dumptables' control command: outputing diagnostics\n");
		sfptpd_interface_diagnostics(0);
		sfptpd_clock_diagnostics(0);
		sfptpd_multicast_dump_state();
		SFPTPD_MSG_INIT(msg.app);
		SFPTPD_MULTICAST_SEND(&msg.app,
				      SFPTPD_APP_MSG_DUMP_TABLES,
				      SFPTPD_MSG_POOL_GLOBAL);
		break;
	case CONTROL_PID_ADJUST:
		/* Adjust PID controller coefficients */
		NOTICE("received 'pid_adjust' control command: (%g, %g, %g) @0%o%s\n",
			param.pid_adjust.kp,
			param.pid_adjust.ki,
			param.pid_adjust.kd,
			param.pid_adjust.servo_type_mask,
			param.pid_adjust.reset ? " reset": "");
		SFPTPD_MSG_INIT(msg.servo);
		msg.servo.u.pid_adjust.kp = param.pid_adjust.kp;
		msg.servo.u.pid_adjust.ki = param.pid_adjust.ki;
		msg.servo.u.pid_adjust.kd = param.pid_adjust.kd;
		msg.servo.u.pid_adjust.servo_type_mask = param.pid_adjust.servo_type_mask;
		msg.servo.u.pid_adjust.reset = param.pid_adjust.reset;
		SFPTPD_MULTICAST_SEND(&msg.servo,
				      SFPTPD_SERVO_MSG_PID_ADJUST,
				      SFPTPD_MSG_POOL_GLOBAL);
		break;
	}
}


static const struct sfptpd_thread_ops main_thread_ops = 
{
	main_on_startup,
	main_on_shutdown,
	main_on_message,
	main_on_user_fds
};


/****************************************************************************
 * Entry Point
 ****************************************************************************/

/* Make main a weak symbol so we can override it for unit testing */
#pragma weak main
int main(int argc, char **argv)
{
	int rc, i;
	sigset_t signal_set;
	int lock_fd = -1;
#ifdef HAVE_CAPS
	uid_t original_user;
#endif

	/* Ensure that both streams are line buffered before anything is
	   output on them so that they behave effectively if they are
	   directed to the same fd. Ignore any errors. */
	setvbuf(stdout, (char *) NULL, _IOLBF, 0);
	setvbuf(stderr, (char *) NULL, _IOLBF, 0);

	INFO("Solarflare Enhanced PTP Daemon, version %s\n",
	     SFPTPD_VERSION_TEXT);

	/* Initialise the configuration to the defaults */
	rc = sfptpd_config_create(&config);
	if (rc != 0)
		return rc;

	/* Parse the command line options to get the configuration file */
	rc = sfptpd_config_parse_command_line_pass1(config, argc, argv);
	if (rc != 0)
		goto fail;

	/* Parse the configuration file */
	rc = sfptpd_config_parse_file(config);
	if (rc != 0)
		goto fail;

	/* Parse the command line options and configuration file */
	rc = sfptpd_config_parse_command_line_pass2(config, argc, argv);
	if (rc != 0)
		goto fail;

	/* Perform some runtime checks */
	rc = runtime_checks(config);
	if (rc != 0)
		goto fail;

	/* Create a lock */
	rc = lock_create(config, &lock_fd);
	if (rc != 0)
		goto fail;

	/* Set up logging */
	rc = sfptpd_log_open(config);
	if (rc != 0)
		goto exit;

	/* Set up control interface */
	rc = sfptpd_control_socket_open(config);
	if (rc != 0)
		goto exit;

	/* Start netlink client */
	rc = netlink_start();
	if (rc != 0)
		goto exit;

	/* Set up the hardware state lock */
	rc = hardware_state_lock_init();
	if (rc != 0)
		goto exit;

	/* Start clock management */
	rc = sfptpd_clock_initialise(config, &hardware_state_lock);
	if (rc != 0)
		goto exit;

	/* Start interface management */
	rc = sfptpd_interface_initialise(config, &hardware_state_lock,
					 initial_link_table);
	if (rc != 0)
		goto exit;

#ifdef HAVE_CAPS
	/* Drop to non-root user/group if so configured */
	original_user = geteuid();
	rc = drop_user(config);
	if (rc != 0)
		goto exit;

	/* Ensure suitable system privilege is gained or dropped */
	rc = claim_drop_privilege(config, original_user);
	if (rc != 0)
		goto exit;
#endif

	/* If configured to do so, daemonize the application */
	rc = daemonize(config, &lock_fd);
	if (rc != 0)
		goto exit;

	/* Create the set of signals that the application handles */
	sigemptyset(&signal_set);
	sigaddset(&signal_set, SIGINT);
	sigaddset(&signal_set, SIGTERM);
	sigaddset(&signal_set, SIGHUP);
	sigaddset(&signal_set, SIGUSR1);
	for (i = SIGRTMIN; i < SIGRTMAX; i++)
		sigaddset(&signal_set, i);

	/* Initialise the threading library */
	rc = sfptpd_threading_initialise(SFPTPD_NUM_GLOBAL_MSGS,
					 SFPTPD_SIZE_GLOBAL_MSGS,
					 SFPTPD_THREAD_ZOMBIES_REAP_AT_EXIT);
	if (rc != 0)
		goto exit;

	/* Enter the main loop. This only returns when the application exits */
	rc = sfptpd_thread_main(&main_thread_ops, &signal_set, main_on_signal, NULL);

exit:
	sfptpd_threading_shutdown();
	sfptpd_clock_shutdown();
	sfptpd_interface_shutdown(config);
	hardware_state_lock_destroy();
	if (netlink != NULL)
		sfptpd_netlink_finish(netlink);
	sfptpd_control_socket_close();
	sfptpd_log_close();
	lock_delete(lock_fd);
fail:
	if (rc == ESHUTDOWN)
		rc = 0;
	sfptpd_log_config_abandon();
	sfptpd_config_destroy(config);

	return rc;
}


/* fin */
