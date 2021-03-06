/*
 * Copyright (C) 2014-2016 Firejail Authors
 *
 * This file is part of firejail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "firejail.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <grp.h>

#define MAX_BUF 4096

int is_container(const char *str) {
	assert(str);
	if (strcmp(str, "lxc") == 0 ||
	     strcmp(str, "docker") == 0 ||
	     strcmp(str, "lxc-libvirt") == 0 ||
	     strcmp(str, "systemd-nspawn") == 0 ||
	     strcmp(str, "rkt") == 0)
		return 1;
	return 0;
}

// returns 1 if we are running under LXC
int check_namespace_virt(void) {
	EUID_ASSERT();
	
	// check container environment variable
	char *str = getenv("container");
	if (str && is_container(str))
		return 1;
	
	// check PID 1 container environment variable
	EUID_ROOT();
	FILE *fp = fopen("/proc/1/environ", "r");
	if (fp) {
		int c = 0;
		while (c != EOF) {
			// read one line
			char buf[MAX_BUF];
			int i = 0;
			while ((c = fgetc(fp)) != EOF) {
				if (c == 0)
					break;
				buf[i] = (char) c;
				if (++i == (MAX_BUF - 1))
					break;
			}
			buf[i] = '\0';
			
			// check env var name
			if (strncmp(buf, "container=", 10) == 0) {
				// found it
				if (is_container(buf + 10)) {
					fclose(fp);
					EUID_USER();
					return 1;
				}
			}
//			printf("i %d c %d, buf #%s#\n", i, c, buf);
		}
		
		fclose(fp);
	}
		
	EUID_USER();
	return 0;
}

// check process space for kernel processes
// return 1 if found, 0 if not found
int check_kernel_procs(void) {
	// we run this function with EUID set in order to detect grsecurity
	// only user processes are available in /proc when running grsecurity
	// EUID_ASSERT();

	char *kern_proc[] = {
		"kthreadd",
		"ksoftirqd",
		"kworker",
		"rcu_sched",
		"rcu_bh",
		NULL	// NULL terminated list
	};
	int i;

	if (arg_debug)
		printf("Looking for kernel processes\n");

	// look at the first 10 processes
	// if a kernel process is found, return 1
	for (i = 1; i <= 10; i++) { 
		struct stat s;
		char *fname;
		if (asprintf(&fname, "/proc/%d/comm", i) == -1)
			errExit("asprintf");
		if (stat(fname, &s) == -1) {
			free(fname);
			continue;
		}
		
		// open file
		/* coverity[toctou] */
		FILE *fp = fopen(fname, "r");
		if (!fp) {
			fprintf(stderr, "Warning: cannot open %s\n", fname);
			free(fname);
			continue;
		}
		
		// read file
		char buf[100];
		if (fgets(buf, 10, fp) == NULL) {
			fprintf(stderr, "Warning: cannot read %s\n", fname);
			fclose(fp);
			free(fname);
			continue;
		}
		// clean /n
		char *ptr;
		if ((ptr = strchr(buf, '\n')) != NULL)
			*ptr = '\0';
		
		// check process name against the kernel list
		int j = 0;
		while (kern_proc[j] != NULL) {
			if (strncmp(buf, kern_proc[j], strlen(kern_proc[j])) == 0) {
				if (arg_debug)
					printf("Found %s process, we are not running in a sandbox\n", buf);
				fclose(fp);
				free(fname);
				return 1;
			}
			j++;
		}
		
		fclose(fp);
		free(fname);
	}

	if (arg_debug)
		printf("No kernel processes found, we are already running in a sandbox\n");

	return 0;
}

void run_no_sandbox(int argc, char **argv) {
	EUID_ASSERT();

	// process limited subset of options
	int i;
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--csh") == 0) {
			if (arg_shell_none) {
				fprintf(stderr, "Error: --shell=none was already specified.\n");
				exit(1);
			}
			if (cfg.shell) {
				fprintf(stderr, "Error: only one default user shell can be specified\n");
				exit(1);
			}
			cfg.shell = "/bin/csh";
		}
		else if (strcmp(argv[i], "--zsh") == 0) {
			if (arg_shell_none) {
				fprintf(stderr, "Error: --shell=none was already specified.\n");
				exit(1);
			}
			if (cfg.shell) {
				fprintf(stderr, "Error: only one default user shell can be specified\n");
				exit(1);
			}
			cfg.shell = "/bin/zsh";
		}
		else if (strcmp(argv[i], "--shell=none") == 0) {
			arg_shell_none = 1;
			if (cfg.shell) {
				fprintf(stderr, "Error: a shell was already specified\n");
				exit(1);
			}
		}
		else if (strncmp(argv[i], "--shell=", 8) == 0) {
			if (arg_shell_none) {
				fprintf(stderr, "Error: --shell=none was already specified.\n");
				exit(1);
			}
			invalid_filename(argv[i] + 8);

			if (cfg.shell) {
				fprintf(stderr, "Error: only one user shell can be specified\n");
				exit(1);
			}
			cfg.shell = argv[i] + 8;

			if (is_dir(cfg.shell) || strstr(cfg.shell, "..")) {
				fprintf(stderr, "Error: invalid shell\n");
				exit(1);
			}

			// access call checks as real UID/GID, not as effective UID/GID
			if(cfg.chrootdir) {
				char *shellpath;
				if (asprintf(&shellpath, "%s%s", cfg.chrootdir, cfg.shell) == -1)
					errExit("asprintf");
				if (access(shellpath, R_OK)) {
					fprintf(stderr, "Error: cannot access shell file in chroot\n");
					exit(1);
				}
				free(shellpath);
			} else if (access(cfg.shell, R_OK)) {
				fprintf(stderr, "Error: cannot access shell file\n");
				exit(1);
			}
		}
	}

	// use $SHELL to get shell used in sandbox
	if (!arg_shell_none && !cfg.shell) {
		char *shell =  secure_getenv("SHELL");
		if (access(shell, R_OK) == 0)
			cfg.shell = shell;
	}
	// guess shell otherwise
	if (!arg_shell_none && !cfg.shell) {
		cfg.shell = guess_shell();
		if (arg_debug)
			printf("Autoselecting %s as shell\n", cfg.shell);
	}
	if (!arg_shell_none && !cfg.shell) {
		fprintf(stderr, "Error: unable to guess your shell, please set explicitly by using --shell option.\n");
		exit(1);
	}

	int prog_index = 0;
	// find first non option arg:
	//	- first argument not starting wiht --,
	//	- whatever follows after -c (example: firejail -c ls)
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0) {
			prog_index = i + 1;
			if (prog_index == argc) {
				fprintf(stderr, "Error: option -c requires an argument\n");
				exit(1);
			}
			break;
		}
		// check first argument not starting with --
		if (strncmp(argv[i],"--",2) != 0) {
			prog_index = i;
			break;
		}
	}

	if (!arg_shell_none) {
		if (prog_index == 0) {
			cfg.command_line = cfg.shell;
			cfg.window_title = cfg.shell;
		} else {
			build_cmdline(&cfg.command_line, &cfg.window_title, argc, argv, prog_index);
		}
	}

	cfg.original_argv = argv;
	cfg.original_program_index = prog_index;

	char *command;
	if (prog_index == 0)
		command = cfg.shell;
	else
		command = argv[prog_index];
	if (!arg_quiet)
		fprintf(stderr, "Warning: an existing sandbox was detected. "
			"%s will run without any additional sandboxing features\n", command);

	start_application();
}
