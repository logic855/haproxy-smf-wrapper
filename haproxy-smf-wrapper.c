/*
 * Wrapper to make haproxy SMF-compliant.
 *
 * Copyright 2014 Maciej Małecki <me@mmalecki.com>
 *
 * Blatantly hacked from:
 *
 * Wrapper to make haproxy systemd-compliant.
 *
 * Copyright 2013 Marc-Antoine Perennou <Marc-Antoine@Perennou.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define REEXEC_FLAG "HAPROXY_SMF_REEXEC"
#define SD_DEBUG "<7>"
#define SD_NOTICE "<5>"

static char *pid_file = "/tmp/haproxy.pid";
static int wrapper_argc;
static char **wrapper_argv;

static void spawn_haproxy(char **pid_strv, int nb_pid)
{
	pid_t pid;
	int main_argc;
	char **main_argv;

	main_argc = wrapper_argc - 1;
	main_argv = wrapper_argv + 1;

	pid = fork();
	if (!pid) {
		/* 3 for "haproxy -Ds -sf" */
		char **argv = calloc(4 + main_argc + nb_pid + 1, sizeof(char *));
		int i;
		int argno = 0;
		for (i = 0; i < main_argc; ++i)
			argv[argno++] = main_argv[i];
		argv[argno++] = "-Ds";
		if (nb_pid > 0) {
			argv[argno++] = "-sf";
			for (i = 0; i < nb_pid; ++i)
				argv[argno++] = pid_strv[i];
		}
		argv[argno] = NULL;

		fprintf(stderr, SD_DEBUG "haproxy-smf-wrapper: executing ");
		for (i = 0; argv[i]; ++i)
			fprintf(stderr, "%s ", argv[i]);
		fprintf(stderr, "\n");

		execv(argv[0], argv);
		exit(0);
	}
}

static int read_pids(char ***pid_strv)
{
	FILE *f = fopen(pid_file, "r");
	int read = 0, allocated = 8;
	char pid_str[10];

	if (!f)
		return 0;

	*pid_strv = malloc(allocated * sizeof(char *));
	while (1 == fscanf(f, "%s\n", pid_str)) {
		if (read == allocated) {
			allocated *= 2;
			*pid_strv = realloc(*pid_strv, allocated * sizeof(char *));
		}
		(*pid_strv)[read++] = strdup(pid_str);
	}

	fclose(f);

	return read;
}

static void sighup_handler(int signum __attribute__((unused)))
{
	setenv(REEXEC_FLAG, "1", 1);
	fprintf(stderr, SD_NOTICE "haproxy-smf-wrapper: re-executing\n");

	execv(wrapper_argv[0], wrapper_argv);
}

static void sigint_handler(int signum __attribute__((unused)))
{
	int i, pid;
	char **pid_strv = NULL;
	int nb_pid = read_pids(&pid_strv);
	for (i = 0; i < nb_pid; ++i) {
		pid = atoi(pid_strv[i]);
		if (pid > 0) {
			fprintf(stderr, SD_DEBUG "haproxy-smf-wrapper: SIGINT -> %d\n", pid);
			kill(pid, SIGINT);
			free(pid_strv[i]);
		}
	}
	free(pid_strv);
}

static void init(int argc, char **argv)
{
	while (argc > 1) {
		if (**argv == '-') {
			char *flag = *argv + 1;
			--argc; ++argv;
			if (*flag == 'p')
				pid_file = *argv;
		}
		--argc; ++argv;
	}
}

int main(int argc, char **argv)
{
	int status;

	wrapper_argc = argc;
	wrapper_argv = argv;

	--argc; ++argv;
	init(argc, argv);

	signal(SIGINT, &sigint_handler);
	signal(SIGHUP, &sighup_handler);

	if (getenv(REEXEC_FLAG) != NULL) {
		/* We are being re-executed: restart HAProxy gracefully */
		int i;
		char **pid_strv = NULL;
		int nb_pid = read_pids(&pid_strv);
		sigset_t sigs;

		unsetenv(REEXEC_FLAG);
		spawn_haproxy(pid_strv, nb_pid);

		/* Unblock SIGHUP which was blocked by the signal handler
		 * before re-exec */
		sigprocmask(SIG_BLOCK, NULL, &sigs);
		sigdelset(&sigs, SIGHUP);
		sigprocmask(SIG_SETMASK, &sigs, NULL);

		for (i = 0; i < nb_pid; ++i)
			free(pid_strv[i]);
		free(pid_strv);
	}
	else {
		/* Start a fresh copy of HAProxy */
		spawn_haproxy(NULL, 0);
	}

	status = -1;
	while (-1 != wait(&status) || errno == EINTR)
		;

	fprintf(stderr, SD_NOTICE "haproxy-smf-wrapper: exit, haproxy RC=%d\n",
			status);
	return status;
}
