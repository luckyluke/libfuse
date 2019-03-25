/**********************************************************
 * main.c
 *
 * Copyright (C) 2004,2005,2006 by Stefan Siegl <stesie@brokenpipe.de>, Germany
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

#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <error.h>
#include <hurd/netfs.h>
#include <hurd.h>

#include "fuse_i.h"
#include "fuse.h"

/* global variables, needed for netfs */
char *netfs_server_name = PACKAGE;
char *netfs_server_version = VERSION;
int netfs_maxsymlinks = 12;

/* libfuse filesystem configuration */
struct _libfuse_params libfuse_params = { 0 };

/* pointer to the fuse structure of this translator process */
struct fuse *libfuse_fuse = NULL;

/* the key for the fuse_context */
static pthread_key_t libfuse_ctx_key;

/* the port where to write out debug messages to, NULL to omit these */
FILE *debug_port = NULL;

/* bootstrap fuse translator */
static int fuse_bootstrap(const char *mountpoint);


/* Destroy the fuse_context held as TSD.
 */
static void
fuse_destroy_context(void *data)
{
  free(data);
}

void fuse_destroy(struct fuse *f)
{
  NOT_IMPLEMENTED();
}

/* Create the TSD key.
 */
static void
fuse_create_key(void)
{
  int err = pthread_key_create(&libfuse_ctx_key, fuse_destroy_context);
  assert_perror(err);
}


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

  else if(! strcmp(option, "default_permissions"))
    libfuse_params.deflt_perms = 1;

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

  else if(! strcmp(option, "fsname"))
    {
      /* nothing to do for us */
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

  while((opt = getopt(argc, argv, "d::o:hsf")) >= 0)
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

      case 'f':
	libfuse_params.foreground = 1;
	break;

      case '?':
      default:
	opt_help = stderr;
	break;
      }

  if(argc - optind > 1) 
    {
      opt_help = stderr;
      fprintf(opt_help, "%s: too many command line arguments.\n", argv[0]);
    }

  if(opt_help)
    {
      fprintf(opt_help,
	      "usage: %s [FUSE options]\n\n"
	      "FUSE Options:\n"
	      "    -d[FILENAME]           "
	      "enable debug output (default=stderr)\n"
	      "    -s                     disable multi-threaded operation\n"
	      "    -f                     don't fork to background\n"
	      /* "    -r                     "
	       * "mount read only (equivalent to '-o ro')\n" */
	      "    -o opt,[opt...]        mount options\n"
	      "    -h                     print help\n"
	      "\n"
	      "Mount options:\n"
	      "    default_permissions    enable permission checking\n"
	      "    allow_other            allow access to other users\n"
	      "    allow_root             allow access to root\n"
	      "    use_ino                let filesystem set inode numbers\n"
	      /* "    readdir_ino            "
	       * "try to fill in d_ino in readdir\n" */
	      "    umask                  set file permissions (octal)\n"
	      "    uid                    set file owner\n"
	      "    gid                    set file group\n",
	      translat_path);

      exit(opt_help == stdout ? 0 : 1);
    }
}


int fuse_parse_cmdline(struct fuse_args *args, char **mountpoint,
                       int *multithreaded, int *foreground)
{
  if (args->argc < 2)
    return -1;

  fuse_parse_argv(args->argc, args->argv);
  *mountpoint = args->argv[optind];
  *multithreaded = !libfuse_params.disable_mt;
  *foreground = libfuse_params.foreground;

  return 0;
}

int
fuse_main_real(int argc, char *argv[], const struct fuse_operations *op,
	       size_t op_size, void *user_data)
{
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  char *mnt;
  int fg, mt;
  struct fuse *fs;

  if (fuse_parse_cmdline(&args, &mnt, &mt, &fg))
    return -1;

  struct fuse_chan *ch = fuse_mount(mnt, NULL);
  if (!ch)
    return -2;

  fs = fuse_new(ch, NULL, op, op_size, user_data);
  if (!fs)
    {
      fuse_unmount(mnt, ch);
      return -3;
    }

  fuse_daemonize(fg);

  return (libfuse_params.disable_mt ? fuse_loop : fuse_loop_mt)(fs);
}


int
fuse_main_real_compat25(int argc, char *argv[],
			const struct fuse_operations_compat25 *op,
			size_t op_size)
{
  return fuse_main_real(argc, argv, (const struct fuse_operations *) op,
			op_size, NULL);
}


#undef fuse_main
int fuse_main(void);
int fuse_main(void)
{
  fprintf(stderr, "fuse_main is supposed to be used via the macro, not the symbol\n");
  return -1;
}


/* Create a new FUSE filesystem, actually there's nothing for us to do 
 * on the Hurd. Hmm.
 */
struct fuse *
fuse_new(struct fuse_chan *ch, struct fuse_args *args,
	 const struct fuse_operations *op, size_t op_size, void *user_data)
{
  struct fuse *new;

  if(ch != (void *) FUSE_MAGIC)
    return NULL; 

  new = calloc(1, sizeof *new);
  if(! new)
    return NULL;

  switch (op_size)
    {
    case sizeof(new->op.ops25):
      new->version = 25;
      break;
    case sizeof(new->op.ops):
      new->version = 26;
      break;
    default:
      fprintf(stderr, "Unhandled size of fuse_operations: %d\n", op_size);
      fprintf(stderr, "libfuse will abort now\n");
      abort();
    }
  memcpy(&new->op, op, op_size);

  /* FIXME: figure out better values for fuse_conn_info fields.  */
  new->conn.proto_major = FUSE_MAJOR_VERSION;
  new->conn.proto_minor = FUSE_MINOR_VERSION;
  new->conn.async_read = 1;
  new->conn.max_write = UINT_MAX;
  new->conn.max_readahead = UINT_MAX;

  update_context_struct(NULL, new);
  fuse_get_context()->private_data = user_data;

  if(new->op.ops.init != NULL)
    {
    if (new->version >= 26)
      new->private_data = new->op.ops.init(&new->conn);
    else
      new->private_data = new->op.ops25.init();
    }

  update_context_struct(NULL, NULL);

  return new;
}


struct fuse *
fuse_new_compat25(int fd, struct fuse_args *args,
		  const struct fuse_operations_compat25 *op, size_t op_size)
{
  return fuse_new((struct fuse_chan *) fd, args,
		  (const struct fuse_operations *) op, op_size, NULL);
}


/* Create a new mountpoint for our fuse filesystem, i.e. do the netfs
 * initialization stuff ...
 */
struct fuse_chan *
fuse_mount(const char *mountpoint, struct fuse_args *args)
{
  return (struct fuse_chan *) fuse_bootstrap(mountpoint);
}

void fuse_unmount(const char *mountpoint, struct fuse_chan *ch)
{
  // goaway
  NOT_IMPLEMENTED();
}

int
fuse_mount_compat25(const char *mountpoint, struct fuse_args *args)
{
  if(args && args->allocated)
    {
      int i;
      for(i = 0; i < args->argc; i ++)
	if(fuse_parse_opt(args->argv[i]))
	  return 0;
    }

  return fuse_bootstrap(mountpoint);
}


static int
fuse_bootstrap(const char *mountpoint)
{
  mach_port_t bootstrap, ul_node;
  task_get_bootstrap_port(mach_task_self(), &bootstrap);

  if(! mountpoint || bootstrap)
    {
      netfs_init();
      ul_node = netfs_startup(bootstrap, 0);
    }
  else 
    {
      /* 
       * we don't have a bootstrap port, i.e. we were not started using 
       * settrans, but know the mountpoint, therefore try to become
       * a translator the hard way ...
       */
      ul_node = file_name_lookup(mountpoint, 0, 0);

      if(ul_node == MACH_PORT_NULL)
	error(10, 0, "Unable to access underlying node");

      /* fork first, we are expected to act from the background */
      pid_t pid = libfuse_params.foreground ? 0 : fork();
      if(pid < 0)
	{
	  perror(PACKAGE ": failed to fork to background");
	  exit(1);
	}
      else if(pid)
	exit(0); /* parent process */

      /* finally try to get it on ... */
      netfs_init();

      struct port_info *newpi;
      error_t err = ports_create_port(netfs_control_class, netfs_port_bucket,
				      sizeof(struct port_info), &newpi);
      
      if(! err)
	{
	  mach_port_t right = ports_get_send_right(newpi);
	  
	  err = file_set_translator(ul_node, 0, FS_TRANS_SET, 0, "", 0, 
				    right, MACH_MSG_TYPE_COPY_SEND);
	  mach_port_deallocate(mach_task_self(), right);
	  ports_port_deref(newpi);
	}

      if(err)
	error(11, err, "Translator startup failure: fuse_mount_compat22");
    }

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
  if(! f)
    return -1; 

  static int server_timeout = 1000 * 60 * 10; /* ten minutes, just like in
					       * init-loop.c of libnetfs */
  libfuse_fuse = f;

  ports_manage_port_operations_one_thread(netfs_port_bucket,
					  netfs_demuxer,
					  server_timeout);
  return 0;
}


struct fuse_cmd {
  mach_msg_header_t *inp;
  mach_msg_header_t *outp;
  int return_value;
};

static fuse_processor_t fuse_proc = NULL;
static void *fuse_proc_data;

static int
fuse_demuxer(mach_msg_header_t *inp,
	     mach_msg_header_t *outp)
{
  struct fuse_cmd cmd;

  cmd.inp = inp;
  cmd.outp = outp;

  fuse_proc(libfuse_fuse, &cmd, fuse_proc_data);

  return cmd.return_value;
}

void
fuse_process_cmd(struct fuse *f, struct fuse_cmd *cmd)
{
  if(! f)
    {
      cmd->return_value = -1; 
      return;
    }
  
  int netfs_fs_server (mach_msg_header_t *, mach_msg_header_t *);
  int netfs_io_server (mach_msg_header_t *, mach_msg_header_t *);
  int netfs_fsys_server (mach_msg_header_t *, mach_msg_header_t *);
  int netfs_ifsock_server (mach_msg_header_t *, mach_msg_header_t *);

  mach_msg_header_t *inp = cmd->inp;
  mach_msg_header_t *outp = cmd->outp;

  cmd->return_value = (netfs_io_server (inp, outp)
		       || netfs_fs_server (inp, outp)
		       || ports_notify_server (inp, outp)
		       || netfs_fsys_server (inp, outp)
		       || ports_interrupt_server (inp, outp)
		       || netfs_ifsock_server (inp, outp));
}


int
fuse_loop_mt_proc(struct fuse *f, fuse_processor_t proc, void *data)
{
  static int thread_timeout = 1000 * 60 * 2;  /* two minutes */
  static int server_timeout = 1000 * 60 * 10; /* ten minutes, just like in
					       * init-loop.c of libnetfs */

  if(! f)
    return -1; 
  
  /* copy the provided arguments to global variables to make them available
   * to fuse_demuxer ... */
  fuse_proc = proc;
  fuse_proc_data = data;

  libfuse_fuse = f;

  ports_manage_port_operations_multithread(netfs_port_bucket,
					   fuse_demuxer,
					   thread_timeout,
					   server_timeout, 
					   0);
  return 0;
}


int
fuse_loop_mt(struct fuse *f) 
{
  if (f == NULL)
    return -1;

  return fuse_loop_mt_proc(f, (fuse_processor_t) fuse_process_cmd, NULL);
}


void
fuse_exit(struct fuse *f)
{
  (void) f;
  /*
   * well, we should make fuse_main exit, this is, we would have to
   * cancel ports_manage_port_operations_one_thread. however this is
   * not possible, therefore buy the farm for the moment.
   */
  error(1, EIEIO, "fuse_exit called");
}


int
fuse_exited(struct fuse *f)
{
  (void) f;

  /*
   * if fuse_exit is called, we buy the farm, therefore we still must be alive.
   */
  return 0;
}


struct fuse_context *
fuse_get_context(void)
{
  static pthread_once_t libfuse_ctx_key_once = PTHREAD_ONCE_INIT;

  pthread_once(&libfuse_ctx_key_once, fuse_create_key);

  struct fuse_context *ctx = pthread_getspecific(libfuse_ctx_key);

  if(! ctx)
    {
      ctx = calloc(1, sizeof(*ctx));
      if(! ctx)
	{
	  fprintf(stderr, "Cannot allocate a new fuse_context\n");
	  fprintf(stderr, "libfuse will abort now\n");
	  abort();
	}

      pthread_setspecific(libfuse_ctx_key, ctx);
    }

  return ctx;
}

struct fuse_session *fuse_get_session(struct fuse *f)
{
  NOT_IMPLEMENTED();
  return NULL;
}

int fuse_daemonize(int foreground)
{
  if (!foreground)
    {
      int nullfd;

      /* It seems that forking here causes some trouble. Just setsid() and let
       * the fork happen in fuse_bootstrap() */
      if (setsid() == -1)
	{
	  perror("fuse_daemonize: setsid");
	  return -1;
	}

      nullfd = open("/dev/null", O_RDWR, 0);
      if (nullfd != -1)
	{
	  (void) dup2(nullfd, 0);
	  (void) dup2(nullfd, 1);
	  (void) dup2(nullfd, 2);
	  if (nullfd > 2)
	    close(nullfd);
	}

      (void) chdir("/");
    }
  else
    {
      (void) chdir("/");
    }
  return 0;
}

int
fuse_invalidate(struct fuse *f, const char *path)
{
  (void) f;
  (void) path;

  return -EINVAL;
}

int fuse_set_signal_handlers(struct fuse_session *se)
{
  NOT_IMPLEMENTED();
  return 0;
}

void fuse_remove_signal_handlers(struct fuse_session *se)
{
  NOT_IMPLEMENTED();
}

FUSE_SYMVER(".symver fuse_main_real_compat25,fuse_main_real@FUSE_2.5");
FUSE_SYMVER(".symver fuse_new_compat25,fuse_new@FUSE_2.5");
FUSE_SYMVER(".symver fuse_mount_compat25,fuse_mount@FUSE_2.5");
