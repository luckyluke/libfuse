/**********************************************************
 * fuse_i.h
 *
 * Copyright (C) 2004, 2005 by Stefan Siegl <ssiegl@gmx.de>, Germany
 * 
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Publice License,
 * version 2 or any later. The license is contained in the COPYING
 * file that comes with the fuse4hurd distribution.
 *
 * fuse4hurd internals ...
 */

#ifndef FUSE_INTERNAL_H
#define FUSE_INTERNAL_H

#ifdef FUSE_USE_VERSION
#  include "fuse.h"
#else
#  define FUSE_USE_VERSION 22
#  include "fuse.h"
#  include "fuse_compat.h"
#endif

/* write out message to stderr, that some routine is not yet implemented,
 * thus misbehaviour must be accepted. */
#define NOT_IMPLEMENTED() \
    fprintf(stderr, PACKAGE ": functionality not yet implemented in " \
	    __FILE__ ":%d\nyou're welcome to put your effort to here.\n\n", \
	    __LINE__);

/* pointer to the fuse_operations structure of this translator process */
extern int fuse_use_ino;
extern const struct fuse_operations *fuse_ops;
extern const struct fuse_operations_compat2 *fuse_ops_compat;

#define FUSE_OP_HAVE(a) ((fuse_ops) ? \
                          (fuse_ops->a != NULL) : (fuse_ops_compat->a != NULL))
#define FUSE_OP_CALL(a,b...) ((fuse_ops) ? \
                               (fuse_ops->a(b)) : (fuse_ops_compat->a(b)))

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
  struct fuse_file_info info;

  /* pointer to our parent's netnode, if any */
  struct netnode *parent;

  /* reference to our libnetfs node, if any
   * (lock must be held on read or write attempt)
   */
  struct node *node;

  /* lock for *node pointer */
  struct mutex lock;

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
 *** debug cruft                                                           ***
 *****************************************************************************/

/* the port where to write out debug messages to, NULL to omit these */
extern FILE *debug_port;

#define DEBUG(cat,msg...) \
  if(debug_port) \
    fprintf(debug_port, PACKAGE ": " cat ": " msg);

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
        DEBUG("tracing", "leaving %s (" __FILE__ ":%d) ret=%d ", \
	      debug_func_name, __LINE__, retval); \
        if(debug_port) \
          { \
	    fmt; \
	    fprintf(debug_port, "\n"); \
	  } \
        return retval; \
      }

#define FUNC_EPILOGUE_(ret, fmt) \
      FUNC_RETURN_(ret, fmt) \
    } while(0);

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
