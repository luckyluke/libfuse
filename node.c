/**********************************************************
 * node.c
 *
 * Copyright(C) 2004, 2005 by Stefan Siegl <ssiegl@gmx.de>, Germany
 * 
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Publice License,
 * version 2 or any later. The license is contained in the COPYING
 * file that comes with the fuse4hurd distribution.
 *
 * create (and care for) nodes ...
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <error.h>
#include <string.h>
#include <hurd/netfs.h>

#include "fuse_i.h"
#include "fuse.h"

/* create a new node for the specified netnode
 */
struct node *
fuse_make_node(struct netnode *nn) 
{
  struct node *node;

  DEBUG("fuse_make_node", "creating node for %s.\n", nn->path);
  DEBUG("netnode-lock", "locking netnode, path=%s\n", nn->path);
  mutex_lock(&nn->lock);

  if((node = nn->node))
    {
      /* there already is a node, therefore return another reference to it */
      netfs_nref(node);
      mutex_unlock(&nn->lock);
      DEBUG("netnode-lock", "UNlocking netnode, path=%s\n", nn->path);
      DEBUG("fuse_make_node", "reusing already existing node, %s\n", nn->path);
      return node;
    }

  if(! (node = netfs_make_node(nn)))
    {
      mutex_unlock(&nn->lock);
      DEBUG("netnode-lock", "UNlocking netnode, path=%s\n", nn->path);
      return NULL; /* doesn't look to good for us :-(   */
    }

  /* now initialize those stats for which we are reliable ...
   */
  node->nn_stat.st_ino = nn->inode;
  node->nn_stat.st_blksize = 4096; /* depends on our host program, but since
				    * we're expected to fill, assume 4k for now
				    */
  node->nn_stat.st_rdev = 0;

  /* add pointer to our new node structure to the netnode */
  nn->node = node;

  mutex_unlock(&nn->lock);
  DEBUG("netnode-lock", "UNlocking netnode, path=%s\n", nn->path);
  DEBUG("fuse_make_node", "created a new node for %s.\n", nn->path);
  return node;
}
