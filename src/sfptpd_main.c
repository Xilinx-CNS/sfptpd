/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

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
#include <sys/stat.h>
#include <sys/utsname.h>
#include <pthread.h>
#include <unistd.h>

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


/****************************************************************************
 * Local Data
 ****************************************************************************/

static const char *lock_filename = "/var/run/kernel_clock";
static const mode_t lock_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

static struct sfptpd_config *config = NULL;
static struct sfptpd_engine *engine = NULL;

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
		CRITICAL("sfptpd must be run as root\n");
		return EACCES;
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
			WARNING("system clock source should be set to TSC for stability; %s: %s\n",
				source[0] ? "current source is" : "could not determine current source",
				source[0] ? source : strerror(errno));
			rc = ENOEXEC;
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

	assert(config != NULL);
	assert(lock_fd != NULL);
	*lock_fd = -1;

	/* If locking is disabled, return straight-away */
	if (!sfptpd_general_config_get(config)->lock)
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
	} else {
		*lock_fd = fd;
		return 0;
	}
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


static int main_on_startup(void *not_used)
{
	int rc;
	int control_fd;

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
	rc = sfptpd_engine_create(config, &engine);

	return rc; 
}


static void main_on_shutdown(void *not_used)
{
	/* If we get here we've shutdown due to a terminate or kill signal.
	 * Clean up and exit. */
	if (engine != NULL)
		sfptpd_engine_destroy(engine);
	engine = NULL;
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
	struct sfptpd_netlink_event intf_event;

	/* We only register a single user file descriptor in this thread. */
	assert(num_fds == 1);
	assert(fds[0] == sfptpd_control_socket_get_fd());

	action = sfptpd_control_socket_get_action(&param);
	switch (action) {
	case CONTROL_NOP:
		NOTICE("unrecognised control command\n");
		break;
	case CONTROL_ERROR:
		NOTICE("error receiving control command\n");
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
	case CONTROL_INTERFACEEVENT:
		/* Notify the engine that something has happened to an interface */
		NOTICE("received 'interface%s' control command for interface %s\n",
			   param.interface_event.insert? "insert": "remove",
			   param.interface_event.if_name);

		if (sfptpd_general_config_get(config)->hotplug_detection &
		    SFPTPD_HOTPLUG_DETECTION_MANUAL) {
			intf_event.if_index = -1;
			sfptpd_strncpy(intf_event.if_name, param.interface_event.if_name,
					       sizeof(intf_event.if_name));
			intf_event.insert = param.interface_event.insert;
			sfptpd_engine_interface_events(engine, &intf_event, 1);
		} else {
			ERROR("ignoring interface control command received when not "
			      "in manual hotplug detection mode\n");
		}
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

	/* Set up the hardware state lock */
	rc = hardware_state_lock_init();
	if (rc != 0)
		goto exit;

	/* Start clock management */
	rc = sfptpd_clock_initialise(config, &hardware_state_lock);
	if (rc != 0)
		goto exit;

	/* Start interface management */
	rc = sfptpd_interface_initialise(config, &hardware_state_lock);
	if (rc != 0)
		goto exit;

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
					 SFPTPD_SIZE_GLOBAL_MSGS);
	if (rc != 0)
		goto exit;

	/* Enter the main loop. This only returns when the application exits */
	rc = sfptpd_thread_main(&main_thread_ops, &signal_set, main_on_signal, NULL);

exit:
	sfptpd_threading_shutdown();
	sfptpd_clock_shutdown();
	sfptpd_interface_shutdown(config);
	hardware_state_lock_destroy();
	sfptpd_control_socket_close();
	sfptpd_log_close();
	lock_delete(lock_fd);
fail:
	sfptpd_log_config_abandon();
	sfptpd_config_destroy(config);
	return rc;
}


/* fin */
