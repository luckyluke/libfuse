/**********************************************************
 * fuse_i.h
 *
 * Copyright (C) 2004, 2005, 2006 by Stefan Siegl <ssiegl@gmx.de>, Germany
 * 
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Publice License,
 * version 2 or any later. The license is contained in the COPYING
 * file that comes with the libfuse distribution.
 *
 * libfuse internals ...
 */

#ifndef FUSE_INTERNAL_H
#define FUSE_INTERNAL_H

#ifdef FUSE_USE_VERSION
#  include "fuse.h"
#else
#  define FUSE_USE_VERSION 25
#  include "fuse.h"
#  include "fuse_compat.h"
#endif

/* write out message to stderr, that some routine is not yet implemented,
 * thus misbehaviour must be accepted. */
#define NOT_IMPLEMENTED() \
    fprintf(stderr, PACKAGE ": functionality not yet implemented in " \
	    __FILE__ ":%d\nyou're welcome to put your effort to here.\n\n", \
	    __LINE__)

struct fuse {
  int version;
  union {
    struct fuse_operations ops25;
  } op;
  void *private_data;
};

/* pointer to the fuse structure of this translator process */
extern struct fuse *libfuse_fuse;

#define FUSE_OP_HAVE(a) (libfuse_fuse->op.ops25.a != NULL)
#define FUSE_OP_CALL(a,b...) (libfuse_fuse->op.ops25.a(b))

#define NN_INFO(dir)   ((void *) &(dir)->nn->info.info25)

#define NN_INFO_APPLY(node,key) (node)->nn->info.info25.key

#define FUSE_SYMVER(x) __asm__(x)


extern __thread struct fuse_context *libfuse_ctx;

/*****************************************************************************
 *** netnodes (in memory representation of libfuse's files or directories) ***
 *****************************************************************************/
struct netnode;
struct netnode {
  /* full pathname of the referenced file or directory */
  char *path;

  /* inode number assigned to the node, unique id that mustn't be
   * changed - throughout the life of this translator
   */
  ino_t inode;

  /* information about the opened file
   */
  union {
    struct fuse_file_info info25;
  } info;

  /* pointer to our parent's netnode, if any */
  struct netnode *parent;

  /* reference to our libnetfs node, if any
   * (lock must be held on read or write attempt)
   */
  struct node *node;

  /* lock for *node pointer */
  pthread_mutex_t lock;

  /* whether this is an anonymous node, i.e. it has to be deleted,
   * after the last node associated to it, was removed.
   */
  unsigned anonymous :1;

  /* whether the netnode was touched since reading it from disk, 
   * i.e. the was a write access that may not be synced to disk
   */
  unsigned may_need_sync :1;
};

/* make a new netnode for a specific path, with specified parent
 * parent == NULL means root node, i.e. "/" ...
 */
struct netnode *fuse_make_netnode(struct netnode *parent, const char *path);

/* scan the whole netnode hash and call fuse_ops->fsync on every node,
 * that may be out of sync
 */
error_t fuse_sync_filesystem(void);



/*****************************************************************************
 *** nodes (structures used by the GNU Hurd, i.e. libnetfs)                ***
 *****************************************************************************/

/* make a new node for a specific netnode */
struct node *fuse_make_node(struct netnode *nn);



/*****************************************************************************
 *** various parameters which can be used to change libfuse's behaviour    ***
 *****************************************************************************/
struct _libfuse_params {
  /* whether or not the filesystem sets the inode number */
  unsigned use_ino             : 1;

  /* whether uid,gid,umask-parameters are specified or not */
  unsigned force_uid           : 1;
  unsigned force_gid           : 1;
  unsigned force_umask         : 1;

  /* whether either root or other users should be allowed to use the
   * filesystem (this overrides uid/gid/umask checking!)   */
  unsigned deflt_perms         : 1;
  unsigned allow_other         : 1;
  unsigned allow_root          : 1;

  /* whether to disable multithreading (if using fuse_main) */
  unsigned disable_mt          : 1;

  /* whether to fork to background or not (if started without settrans) */
  unsigned foreground          : 1;

  /* the uid and gid to set and which umask to apply (if bitfields are set) */
  uid_t uid;
  gid_t gid;
  mode_t umask;
};

extern struct _libfuse_params libfuse_params;

/* magic number, passed from fuse_mount to fuse_new */
#define FUSE_MAGIC ((int) 0x66757365)



/*****************************************************************************
 *** debug cruft                                                           ***
 *****************************************************************************/

/* the port where to write out debug messages to, NULL to omit these */
extern FILE *debug_port;

#define DEBUG(cat,msg...) \
  if(debug_port) \
    fprintf(debug_port, PACKAGE ": " cat ": " msg)

#define FUNC_PROLOGUE_(func_name, fmt...) \
  do \
    { \
      const char *debug_func_name = func_name; \
      DEBUG("tracing", "entering %s (" __FILE__ ":%d) ", \
	    debug_func_name, __LINE__); \
      if(debug_port) \
        { \
          fmt; \
	  fprintf(debug_port, "\n"); \
	}

#define FUNC_PROLOGUE(func_name) \
  FUNC_PROLOGUE_(func_name, (void)0)

#define FUNC_PROLOGUE_FMT(func_name, fmt...) \
  FUNC_PROLOGUE_(func_name, fprintf(debug_port, fmt))

#define FUNC_PROLOGUE_NODE(func_name, node) \
  FUNC_PROLOGUE_FMT(func_name, "node=%s", (node)->nn->path)

#define FUNC_EPILOGUE_NORET() \
      DEBUG("tracing", "leaving %s\n", debug_func_name); \
    } while(0);

#define FUNC_RETURN_(ret, fmt) \
      { \
        int retval = (ret); \
        DEBUG("tracing", "leaving %s (" __FILE__ ":%d) ret=%d ",      \
	      debug_func_name, __LINE__, retval);		      \
        if(debug_port)						      \
          {							      \
	    if(retval)						      \
	      fprintf(debug_port, "(%s) ", strerror(retval));	      \
	    fmt; \
	    fprintf(debug_port, "\n"); \
	  } \
        return retval; \
      }

#define FUNC_EPILOGUE_(ret, fmt) \
      FUNC_RETURN_(ret, fmt) \
    } while(0)

#define FUNC_RETURN_FMT(ret, fmt...) \
  FUNC_RETURN_(ret, fprintf(debug_port, fmt))

#define FUNC_EPILOGUE_FMT(ret, fmt...) \
  FUNC_EPILOGUE_(ret, fprintf(debug_port, fmt))

#define FUNC_RETURN(ret) \
  FUNC_RETURN_(ret, (void)0)

#define FUNC_EPILOGUE(ret) \
  FUNC_EPILOGUE_(ret, (void)0)



/* malloc debugging */
#if 0
static char *_strdup(const char *s, const char *f, int l) {
void *ptr = strdup(s);
DEBUG("strdup", "ptr=%8p [%s:%d]\n", ptr, f, l);
return ptr;
}
#undef strdup
#define strdup(s) _strdup(s, __FILE__, __LINE__)

static void *_malloc(size_t sz, const char *f, int l) {
void *ptr = malloc(sz);
DEBUG("malloc", "ptr=%8p [%s:%d]\n", ptr, f, l);
return ptr;
}
#define malloc(s) _malloc(s, __FILE__, __LINE__)

static void _free(void *ptr, const char *f, int l) {
DEBUG("  free", "ptr=%8p [%s:%d]\n", ptr, f, l);
free(ptr);
}
#define free(s) _free(s, __FILE__, __LINE__)
#endif /* malloc debugging */

#endif /* FUSE_INTERNAL_H */
