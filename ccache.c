/*
  a re-implementation of the compilercache scripts in C

  The idea is based on the shell-script compilercache by Erik Thiele <erikyyy@erikyyy.de>

   Copyright (C) Andrew Tridgell 2002
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "ccache.h"

char *cache_dir = NULL;
char *cache_logfile = NULL;
static ARGS *stripped_args;
static ARGS *orig_args;
static char *output_file;
static char *hashname;
char *stats_file = NULL;
static int found_debug;

/*
  something went badly wrong - just execute the real compiler
*/
static void failed(void)
{
	execv(orig_args->argv[0], orig_args->argv);
	cc_log("execv returned (%s)!\n", strerror(errno));
	exit(1);
}

/* run the real compiler and put the result in cache */
static void to_cache(ARGS *args)
{
	char *path_stderr;
	char *tmp_stdout, *tmp_stderr, *tmp_hashname;
	struct stat st1, st2;
	int status;

	x_asprintf(&tmp_stdout, "%s/tmp.stdout.%d", cache_dir, getpid());
	x_asprintf(&tmp_stderr, "%s/tmp.stderr.%d", cache_dir, getpid());
	x_asprintf(&tmp_hashname, "%s/tmp.hash.%d.o", cache_dir, getpid());

	args_add(args, "-o");
	args_add(args, tmp_hashname);
	status = execute(args->argv, tmp_stdout, tmp_stderr);
	args_pop(args, 2);

	if (stat(tmp_stdout, &st1) != 0 || st1.st_size != 0) {
		cc_log("compiler produced stdout for %s\n", output_file);
		stats_update(STATS_STDOUT);
		unlink(tmp_stdout);
		unlink(tmp_stderr);
		unlink(tmp_hashname);
		failed();
	}
	unlink(tmp_stdout);

	if (status != 0) {
		int fd;
		cc_log("compile of %s gave status = %d\n", output_file, status);
		stats_update(STATS_STATUS);

		fd = open(tmp_stderr, O_RDONLY);
		if (fd != -1 && 
		    (rename(tmp_hashname, output_file) == 0 || errno == ENOENT)) {
			/* we can use a quick method of getting the failed output */
			copy_fd(fd, 2);
			close(fd);
			unlink(tmp_stderr);
			exit(status);
		}
		
		unlink(tmp_stderr);
		unlink(tmp_hashname);
		failed();
	}

	x_asprintf(&path_stderr, "%s.stderr", hashname);

	if (stat(tmp_stderr, &st1) != 0 ||
	    stat(tmp_hashname, &st2) != 0 ||
	    rename(tmp_hashname, hashname) != 0 ||
	    rename(tmp_stderr, path_stderr) != 0) {
		cc_log("failed to rename tmp files\n");
		stats_update(STATS_ERROR);
		failed();
	}

	cc_log("Placed %s into cache\n", output_file);
	stats_tocache(file_size(&st1) + file_size(&st2));

	free(tmp_hashname);
	free(tmp_stderr);
	free(tmp_stdout);
	free(path_stderr);
}


/* hash a file that consists of preprocessor output, but remove any line 
   number information from the hash
*/
static void stabs_hash(const char *fname)
{
	int fd;
	struct stat st;	
	char *map;
	int l_start, l_end;

	fd = open(fname, O_RDONLY);
	if (fd == -1 || fstat(fd, &st) != 0) {
		cc_log("Failed to open preprocessor output %s\n", fname);
		stats_update(STATS_PREPROCESSOR);
		failed();
	}

	/* we use mmap() to make it easy to handle arbitrarily long
           lines in preprocessor output. I have seen lines of over
           100k in length, so this is well worth it */
	map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == (void *)-1) {
		cc_log("Failed to mmap %s\n", fname);
		failed();
	}
	close(fd);

	l_start = 0;
	while (l_start < st.st_size) {
		l_end = l_start;
		while (l_end < st.st_size && map[l_end] != '\n') {
			l_end++;
		}
		/* skip the hash on any lines that look like compiler line
		   numbers */
		if ((l_end - l_start) > 2 &&
		    map[l_start] == '#' && map[l_start+1] == ' ' && 
		    isdigit(map[l_start+2])) {
			l_start = l_end+1;
			continue;
		}
		hash_buffer(&map[l_start], 1 + l_end - l_start);
		l_start = l_end+1;
	}

	munmap(map, st.st_size);
}


/* find the hash for a command. The hash includes all argument lists,
   plus the output from running the compiler with -E */
static void find_hash(ARGS *args)
{
	int i;
	char *path_stdout, *path_stderr;
	char *hash_dir;
	char *s;
	struct stat st;
	int status;

	hash_start();

	/* first the arguments */
	for (i=0;i<args->argc;i++) {
		/* some arguments don't contribute to the hash. The
		   theory is that these arguments will change the
		   output of -E if they are going to have any effect
		   at all, or they only affect linking */
		if (i < args->argc-1) {
			if (strcmp(args->argv[i], "-I") == 0 ||
			    strcmp(args->argv[i], "-include") == 0 ||
			    strcmp(args->argv[i], "-L") == 0 ||
			    strcmp(args->argv[i], "-D") == 0 ||
			    strcmp(args->argv[i], "-isystem") == 0) {
				i++;
				continue;
			}
			if (strncmp(args->argv[i], "-I", 2) == 0 ||
			    strncmp(args->argv[i], "-L", 2) == 0 ||
			    strncmp(args->argv[i], "-D", 2) == 0 ||
			    strncmp(args->argv[i], "-isystem", 8) == 0) {
				continue;
			}
		}

		hash_string(args->argv[i]);
	}

	/* the compiler driver size and date. This is a simple minded way
	   to try and detect compiler upgrades. It is not 100% reliable */
	if (stat(args->argv[0], &st) != 0) {
		cc_log("Couldn't stat the compiler!?\n");
		stats_update(STATS_COMPILER);
		failed();
	}
	hash_int(st.st_size);
	hash_int(st.st_mtime);

	/* now the run */
	x_asprintf(&path_stdout, "%s/tmp.stdout.%d", cache_dir, getpid());
	x_asprintf(&path_stderr, "%s/tmp.stderr.%d", cache_dir, getpid());

	args_add(args, "-E");
	status = execute(args->argv, path_stdout, path_stderr);
	args_pop(args, 1);

	if (status != 0) {
		unlink(path_stdout);
		unlink(path_stderr);
		cc_log("the preprocessor gave %d\n", status);
		stats_update(STATS_PREPROCESSOR);
		failed();
	}

	/* if the compilation is with -g then we have to inlcude the whole of the
	   preprocessor output, which means we are sensitive to line number
	   information. Otherwise we can discard line number info, which makes
	   us less sensitive to reformatting changes 
	*/
	if (found_debug) {
		hash_file(path_stdout);
	} else {
		stabs_hash(path_stdout);
	}
	hash_file(path_stderr);

	unlink(path_stdout);
	unlink(path_stderr);
	free(path_stdout);
	free(path_stderr);

	/* we use a single level subdir for the cache path to reduce the impact
	   on filesystems which are slow for large directories
	*/
	s = hash_result();
	x_asprintf(&hash_dir, "%s/%c", cache_dir, *s);
	if (create_dir(hash_dir) != 0) {
		cc_log("failed to create %s\n", cache_dir);
		failed();
	}
	x_asprintf(&hashname, "%s/%s", hash_dir, s+1);
	x_asprintf(&stats_file, "%s/stats", hash_dir);
	free(hash_dir);
}


/* 
   try to return the compile result from cache. If we can return from
   cache then this function exits with the correct status code,
   otherwise it returns */
static void from_cache(int first)
{
	int fd_stderr;
	char *stderr_file;
	int ret;
	struct stat st;

	x_asprintf(&stderr_file, "%s.stderr", hashname);
	fd_stderr = open(stderr_file, O_RDONLY);
	if (fd_stderr == -1) {
		/* it isn't in cache ... */
		free(stderr_file);
		return;
	}

	/* make sure the output is there too */
	if (stat(hashname, &st) != 0) {
		close(fd_stderr);
		unlink(stderr_file);
		free(stderr_file);
		return;
	}

	utime(stderr_file, NULL);

	unlink(output_file);
	ret = link(hashname, output_file);

	/* the hash file might have been deleted by some external process */
	if (ret == -1 && errno == ENOENT) {
		cc_log("hashfile missing for %s\n", output_file);
		stats_update(STATS_MISSING);
		close(fd_stderr);
		unlink(stderr_file);
		return;
	}
	free(stderr_file);

	if (ret == -1) {
		ret = copy_file(hashname, output_file);
		if (ret == -1) {
			cc_log("failed to copy %s -> %s (%s)\n", 
			       hashname, output_file, strerror(errno));
			stats_update(STATS_ERROR);
			failed();
		}
	}
	if (ret == 0) {
		/* update the mtime on the file so that make doesn't get confused */
		utime(output_file, NULL);
	}

	/* send the stderr */
	copy_fd(fd_stderr, 2);
	close(fd_stderr);

	/* and exit with the right status code */
	if (first) {
		cc_log("got cached result for %s\n", output_file);
		stats_update(STATS_CACHED);
	}

	exit(0);
}

/* find the real compiler. We just search the PATH to find a executable of the 
   same name that isn't a link to ourselves */
static void find_compiler(int argc, char **argv)
{
	char *base;
	char *path, *tok;
	struct stat st1, st2;

	orig_args = args_init();

	orig_args->argv = argv;
	orig_args->argc = argc;

	base = basename(argv[0]);

	/* we might be being invoked like "ccache gcc -c foo.c" */
	if (strcmp(base, MYNAME) == 0) {
		orig_args->argv++;
		orig_args->argc--;
		free(base);
		base = basename(argv[1]);
	}

	path = getenv("CCACHE_PATH");
	if (!path) {
		path = getenv("PATH");
	}
	if (!path) {
		cc_log("no PATH variable!?\n");
		failed();
	}

	path = x_strdup(path);
	
	/* search the path looking for the first compiler of the right name
	   that isn't us */
	for (tok=strtok(path,":"); tok; tok = strtok(NULL, ":")) {
		char *fname;
		x_asprintf(&fname, "%s/%s", tok, base);
		/* look for a normal executable file */
		if (access(fname, X_OK) == 0 &&
		    lstat(fname, &st1) == 0 &&
		    stat(fname, &st2) == 0 &&
		    S_ISREG(st2.st_mode)) {
			char buf[1024];
			int len;

			/* if its a symlink then ensure it doesn't
                           point at something called "ccache" */
			if (S_ISLNK(st1.st_mode)) {
				char *p;
				len = readlink(fname, buf, sizeof(buf));
				if (len != -1 && len < (int)sizeof(buf)) {
					buf[len] = 0;
					p = basename(buf);
					if (strcmp(p, MYNAME) == 0) {
						/* its a link to "ccache" ! */
						free(p);
						continue;
					}
					free(p);
				}
			}


			/* found it! */
			free(path);
			orig_args->argv[0] = fname;
			free(base);
			return;
		}
		free(fname);
	}

	/* can't find the compiler! */
	perror(base);
	exit(1);
}


/* 
   process the compiler options to form the correct set of options 
   for obtaining the preprocessor output
*/
static void process_args(int argc, char **argv)
{
	int i;
	int found_c_opt = 0;
	int found_S_opt = 0;
	char *input_file = NULL;
	struct stat st;

	stripped_args = args_init();

	args_add(stripped_args, argv[0]);

	for (i=1; i<argc; i++) {
		/* some options will never work ... */
		if (strncmp(argv[i], "-E", 2) == 0 ||
		    strncmp(argv[i], "-M", 2) == 0) {
			failed();
		}

		/* we must have -c */
		if (strcmp(argv[i], "-c") == 0) {
			args_add(stripped_args, argv[i]);
			found_c_opt = 1;
			continue;
		}

		/* -S changes the default extension */
		if (strcmp(argv[i], "-S") == 0) {
			args_add(stripped_args, argv[i]);
			found_S_opt = 1;
			continue;
		}
		
		/* we need to work out where the output was meant to go */
		if (strcmp(argv[i], "-o") == 0) {
			if (i == argc-1) {
				cc_log("missing argument to %s\n", argv[i]);
				stats_update(STATS_ARGS);
				failed();
			}
			output_file = argv[i+1];
			i++;
			continue;
		}

		/* debugging is handled specially, so that we know if we
		   can strip line number info 
		*/
		if (strncmp(argv[i], "-g", 2) == 0) {
			args_add(stripped_args, argv[i]);
			if (strcmp(argv[i], "-g0") != 0) {
				found_debug = 1;
			}
			continue;
		}

		/* options that take an argument */
		if (strcmp(argv[i], "-I") == 0 ||
		    strcmp(argv[i], "-include") == 0 ||
		    strcmp(argv[i], "-L") == 0 ||
		    strcmp(argv[i], "-D") == 0 ||
		    strcmp(argv[i], "-isystem") == 0) {
			if (i == argc-1) {
				cc_log("missing argument to %s\n", argv[i]);
				stats_update(STATS_ARGS);
				failed();
			}
						
			args_add(stripped_args, argv[i]);
			args_add(stripped_args, argv[i+1]);
			i++;
			continue;
		}

		/* other options */
		if (argv[i][0] == '-') {
			args_add(stripped_args, argv[i]);
			continue;
		}

		/* if an argument isn't a plain file then assume its
		   an option, not an input file. This allows us to
		   cope better with unusual compiler options */
		if (stat(argv[i], &st) != 0 || !S_ISREG(st.st_mode)) {
			args_add(stripped_args, argv[i]);
			continue;			
		}

		if (input_file) {
			cc_log("multiple input files (%s and %s)\n",
			       input_file, argv[i]);
			stats_update(STATS_LINK);
			failed();
		}

		input_file = argv[i];
		args_add(stripped_args, argv[i]);
	}

	if (!input_file) {
		cc_log("No input file found\n");
		stats_update(STATS_ARGS);
		failed();
	}

	if (!found_c_opt) {
		cc_log("No -c option found for %s\n", input_file);
		stats_update(STATS_LINK);
		failed();
	}

	if (!output_file) {
		char *p;
		output_file = x_strdup(input_file);
		if ((p = strrchr(output_file, '/'))) {
			output_file = p+1;
		}
		p = strrchr(output_file, '.');
		if (!p || !p[1]) {
			cc_log("badly formed output_file %s\n", output_file);
			stats_update(STATS_ARGS);
			failed();
		}
		p[1] = found_S_opt ? 's' : 'o';
		p[2] = 0;
#if 0
		cc_log("Formed output file %s from input_file %s\n", 
		       output_file, input_file);
#endif
	}
}

/* the main ccache driver function */
static void ccache(int argc, char *argv[])
{
	/* find the real compiler */
	find_compiler(argc, argv);
	
	/* we might be disabled */
	if (getenv("CCACHE_DISABLE")) {
		cc_log("ccache is disabled\n");
		failed();
	}

	/* process argument list, returning a new set of arguments for pre-processing */
	process_args(orig_args->argc, orig_args->argv);

	/* run with -E to find the hash */
	find_hash(stripped_args);

	/* if we can return from cache at this point then do */
	from_cache(1);
	
	/* run real compiler, semding output to cache */
	to_cache(stripped_args);

	/* return from cache */
	from_cache(0);

	/* oh oh! */
	cc_log("secondary from_cache failed!\n");
	stats_update(STATS_ERROR);
	failed();
}


static void usage(void)
{
	printf("ccache, a compiler cache. Version %s\n", CCACHE_VERSION);
	printf("Copyright Andrew Tridgell, 2002\n\n");
	
	printf("Usage:\n");
	printf("\tccache [options]\n");
	printf("\tccache compiler [compile options]\n");
	printf("\tcompiler [compile options]    (via symbolic link)\n");
	printf("\nOptions:\n");

	printf("-s                      show statistics summary\n");
	printf("-h                      zero statistics\n");
	printf("-c                      run a cache cleanup\n");
	printf("-F <maxfiles>           set maximum files in cache\n");
	printf("-M <maxsize>            set maximum size of cache (use G, M or K)\n");
	printf("-h                      this help page\n");
	printf("-V                      print version number\n");
}

/* the main program when not doing a compile */
static int ccache_main(int argc, char *argv[])
{
	extern int optind;
	int c;
	size_t v;

	while ((c = getopt(argc, argv, "hszcF:M:V")) != -1) {
		switch (c) {
		case 'V':
			printf("ccache version %s\n", CCACHE_VERSION);
			exit(0);

		case 'h':
			usage();
			exit(0);
			
		case 's':
			stats_summary();
			break;

		case 'c':
			cleanup_all(cache_dir);
			printf("Cleaned cached\n");
			break;

		case 'z':
			stats_zero();
			printf("Statistics cleared\n");
			break;

		case 'F':
			v = atoi(optarg);
			stats_set_limits(v, -1);
			printf("Set cache file limit to %u\n", (unsigned)v);
			break;

		case 'M':
			v = value_units(optarg);
			stats_set_limits(-1, v);
			printf("Set cache size limit to %uk\n", (unsigned)v);
			break;

		default:
			usage();
			exit(1);
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	cache_dir = getenv("CCACHE_DIR");
	if (!cache_dir) {
		x_asprintf(&cache_dir, "%s/.ccache", getenv("HOME"));
	}

	cache_logfile = getenv("CCACHE_LOGFILE");

	/* check if we are being invoked as "ccache" */
	if (strlen(argv[0]) >= strlen(MYNAME) &&
	    strcmp(argv[0] + strlen(argv[0]) - strlen(MYNAME), MYNAME) == 0) {
		if (argc < 2) {
			usage();
			exit(1);
		}
		/* if the first argument isn't an option, then assume we are
		   being passed a compiler name and options */
		if (argv[1][0] == '-') {
			return ccache_main(argc, argv);
		}
	}

	/* make sure the cache dir exists */
	if (create_dir(cache_dir) != 0) {
		fprintf(stderr,"ccache: failed to create %s (%s)\n", 
			cache_dir, strerror(errno));
		exit(1);
	}

	ccache(argc, argv);
	return 1;
}
