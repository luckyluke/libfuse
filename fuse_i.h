/**********************************************************
 * fuse_i.h
 *
 * Copyright 2004, Stefan Siegl <ssiegl@gmx.de>, Germany
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

/* write out message to stderr, that some routine is not yet implemented,
 * thus misbehaviour must be accepted. */
#define NOT_IMPLEMENTED() \
    fprintf(stderr, PACKAGE ": functionality not yet implemented in " \
	    __FILE__ ":%d\nyou're welcome to put your effort to here.\n\n", \
	    __LINE__);

/* pointer to the fuse_operations structure of this translator process */
extern const struct fuse_operations *fuse_ops;


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

  /* pointer to our parent's netnode, if any */
  struct netnode *parent;

  /* reference to our libnetfs node, if any
   * (lock must be held on read or write attempt)
   */
  struct node *node;

  /* lock for *node pointer */
  struct mutex lock;
};

/* make a new netnode for a specific path, with specified parent
 * parent == NULL means root node, i.e. "/" ...
 */
struct netnode *fuse_make_netnode(struct netnode *parent, const char *path);



/*****************************************************************************
 *** nodes (structures used by the GNU Hurd, i.e. libnetfs)                ***
 *****************************************************************************/

/* make a new node for a specific netnode */
struct node *fuse_make_node(struct netnode *nn);

#endif /* FUSE_INTERNAL_H */
