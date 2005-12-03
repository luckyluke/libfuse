/**********************************************************
 * main.c
 *
 * Copyright (C) 2004, 2005 by Stefan Siegl <ssiegl@gmx.de>, Germany
 * 
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Publice License,
 * version 2 or any later. The license is contained in the COPYING
 * file that comes with the libfuse distribution.
 *
 * translator startup code (and argp handling)
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <unistd.h>
#include <stdio.h>
#include <error.h>
#include <hurd/netfs.h>

#include "fuse_i.h"
#include "fuse.h"

/* global variables, needed for netfs */
char *netfs_server_name = PACKAGE;
char *netfs_server_version = VERSION;
int netfs_maxsymlinks = 12;

/* libfuse filesystem configuration */
struct _libfuse_params libfuse_params = { 0 };

/* pointer to the fuse_operations structure of this translator process */
const struct fuse_operations *fuse_ops = NULL;
const struct fuse_operations_compat2 *fuse_ops_compat = NULL;

/* the port where to write out debug messages to, NULL to omit these */
FILE *debug_port = NULL;

/* magic number, passed from fuse_mount to fuse_new */
#define FUSE_MAGIC ((int) 0x66757365)



/* Interpret a __single__ mount option 
 * The option itself `opt' may be modified during this call.
 */
static int
fuse_parse_opt(char *opt)
{
  char *ptrptr;
  char *option = strtok_r(opt, "=", &ptrptr);
  char *value = strtok_r(NULL, "=", &ptrptr);

  DEBUG("parse_opt", "option `%s' => `%s'\n", option, value);

  if(! strcmp(option, "use_ino"))
    libfuse_params.use_ino = 1;

  else if(! strcmp(option, "allow_other"))
    libfuse_params.allow_other = 1;

  else if(! strcmp(option, "allow_root"))
    libfuse_params.allow_root = 1;

  else if(! strcmp(option, "uid"))
    {
      char *endptr;
      if(! value || (libfuse_params.uid = strtol(value, &endptr, 10), 
		     endptr == value)) 
	{
	  fprintf(stderr, PACKAGE_NAME ": missing or invalid argument to "
		  "mount option '%s': %s\n", option, value);
	  return EINVAL;
	}

      libfuse_params.force_uid = 1;
    }

  else if(! strcmp(option, "gid"))
    {
      char *endptr;
      if(! value || (libfuse_params.gid = strtol(value, &endptr, 10), 
		     endptr == value)) 
	{
	  fprintf(stderr, PACKAGE_NAME ": missing or invalid argument to "
		  "mount option '%s': %s\n", option, value);
	  return EINVAL;
	}

      libfuse_params.force_gid = 1;
    }

  else if(! strcmp(option, "umask"))
    {
      char *endptr;
      if(! value || (libfuse_params.umask = strtol(value, &endptr, 8), 
		     endptr == value))
	{
	  fprintf(stderr, PACKAGE_NAME ": missing or invalid argument to "
		  "mount option '%s': %s\n", option, value);
	  return EINVAL;
	}

      libfuse_params.force_umask = 1;
    }

  else
    {
      fprintf(stderr, PACKAGE_NAME ": unsupported mount option: %s\n", option);
      return EINVAL;
    }

  return 0;
}

/* Parse a single (or a comma-separated list) of mount options
 * (those that are specified using `-o' on the command line for example)
 */
static int
fuse_parse_opts(const char *opts)
{
  if(! opts) return 0;          /* ... why did'ya call us? */

  char *copy = strdup(opts);    /* copy string to allow strtok calls */
  if(! copy) return ENOMEM;

  char *ptrptr, *token, *tok_me = copy;
  while((token = strtok_r(tok_me, ",", &ptrptr)))
    {
      tok_me = NULL;
      if(fuse_parse_opt(token))
	{
	  free(copy);
	  return EINVAL;
	}
    }

  free(copy);
  return 0;
}



/* Parse the command line arguments given to fuse_main (or _compat function).
 * If --help specified, output help string and call exit.  If there are any
 * options that shall be passed to the file system itself, return them.
 */
static void
fuse_parse_argv(int argc, char *argv[])
{
  const char *translat_path = argv[0];

  /* parse command line arguments */
  int opt;
  FILE *opt_help = NULL;

  while((opt = getopt(argc, argv, "d::o:hs")) >= 0)
    switch(opt)
      {
      case 'd':
	if(optarg)
	  debug_port = fopen(optarg, "w");
	if(! debug_port)
	  debug_port = stderr;

	setvbuf(debug_port, NULL, _IONBF, 0);
	fprintf(debug_port, "translator %s starting up.\n", translat_path);
	break;

      case 'o':
	assert(optarg);
	if(fuse_parse_opts(optarg))
	  exit(1);
	break;

      case 'h':
	opt_help = stdout;
	break;

      case 's':
	libfuse_params.disable_mt = 1;
	break;

      case '?':
      default:
	opt_help = stderr;
	break;
      }

  if(opt_help)
    {
      fprintf(opt_help,
	      "\nusage: %s [options]\n\n"
	      "Options:\n"
	      "    -d[FILENAME]        enable debug output (default=stderr)\n"
	      "    -s                  disable multi-threaded operation\n"
	      "    -o opt,[opt...]     mount options\n"
	      "    -h                  print help\n"
	      "\n", translat_path);

      exit(opt_help == stdout ? 0 : 1);
    }
}



/* Main function of FUSE. (compatibility one for old Fuse API) */
int
fuse_main_compat2(int argc, char *argv[],
		  const struct fuse_operations_compat2 *op)
{
  fuse_parse_argv(argc, argv);

  int fd = fuse_mount(NULL, NULL);
  return (libfuse_params.disable_mt ? fuse_loop : fuse_loop_mt)
    (fuse_new_compat2(fd, NULL, op));
}



/* Main function of FUSE. 
 * named fuse_main_real, since originial fuse.h defines a macro renaming it */
int 
fuse_main_real(int argc, char *argv[],
	       const struct fuse_operations *op, size_t op_size)
{
  fuse_parse_argv(argc, argv);

  int fd = fuse_mount(NULL, NULL);
  return (libfuse_params.disable_mt ? fuse_loop : fuse_loop_mt)
    (fuse_new(fd, NULL, op, op_size));
}



/* Create a new FUSE filesystem, actually there's nothing for us to do 
 * on the Hurd.
 *
 * (Compatibility function for the old Fuse API)
 */
struct fuse *
fuse_new_compat2(int fd, const char *opts, 
		 const struct fuse_operations_compat2 *op)
{
  if(fd != FUSE_MAGIC)
    return NULL; 

  if(fuse_parse_opts(opts))
    return NULL;

  fuse_ops_compat = op;

  return (void *) FUSE_MAGIC; /* we don't have a fuse structure, sorry. */
}



/* Create a new FUSE filesystem, actually there's nothing for us to do 
 * on the Hurd.
 */
struct fuse *
fuse_new(int fd, const char *opts, 
	 const struct fuse_operations *op, size_t op_size)
{
  (void) op_size; /* FIXME, see what the real Fuse library does with 
		   * this argument */

  if(fd != FUSE_MAGIC)
    return NULL; 

  if(fuse_parse_opts(opts))
    return NULL;

  fuse_ops = op;

  return (void *) FUSE_MAGIC; /* we don't have a fuse structure, sorry. */
}



/* Create a new mountpoint for our fuse filesystem, i.e. do the netfs
 * initialization stuff ...
 */
int 
fuse_mount(const char *mountpoint, const char *opts)
{
  (void) mountpoint; /* we don't care for the specified mountpoint, as 
		      * we need to be set up using settrans ... */

  if(fuse_parse_opts(opts))
    return 0;

  mach_port_t bootstrap, ul_node;

  task_get_bootstrap_port(mach_task_self(), &bootstrap);
  if(bootstrap == MACH_PORT_NULL)
    {
      /* no assigned bootstrap port, i.e. we got called as a
       * common program, not using settrans
       */
      fprintf(stderr, "program must be started as a translator.\n");
      return EPERM;
    }

  /* we have got a bootstrap port, that is, we were set up
   * using settrans and may start with normal operation ... */
  netfs_init();

  ul_node = netfs_startup(bootstrap, 0);

  /* create our root node */
  {
    struct netnode *root = fuse_make_netnode(NULL, "/");
    netfs_root_node = fuse_make_node(root);
  }

  if(! netfs_root_node)
    {
      perror(PACKAGE ": cannot create rootnode");
      return -EAGAIN;
    }

  return FUSE_MAGIC;
}



int
fuse_loop(struct fuse *f)
{
  if(f != ((void *) FUSE_MAGIC))
    return -1; 

  static int server_timeout = 1000 * 60 * 10; /* ten minutes, just like in
					       * init-loop.c of libnetfs */

  ports_manage_port_operations_one_thread(netfs_port_bucket,
					  netfs_demuxer,
					  server_timeout);
  return 0;
}


int
fuse_loop_mt(struct fuse *f) 
{
  if(f != ((void *) FUSE_MAGIC))
    return -1; 
  
  netfs_server_loop();
  return 0;
}
