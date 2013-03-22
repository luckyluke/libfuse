/**********************************************************
 * netnode.c
 *
 * Copyright(C) 2004,2005,2006 by Stefan Siegl <stesie@brokenpipe.de>, Germany
 * 
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Publice License,
 * version 2 or any later. The license is contained in the COPYING
 * file that comes with the libfuse distribution.
 *
 * netnode handling
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

#define HASH_BUCKETS 512

struct hash_element;
static struct hash_element {
  struct netnode *nn;
  struct hash_element *next; /* singly linked list */
} fuse_netnodes[HASH_BUCKETS] = {{ 0 }};

/* rwlock needs to be held when touching either fuse_netnodes hash or
 * fuse_next_inode variable
 */
pthread_rwlock_t fuse_netnodes_lock = PTHREAD_RWLOCK_INITIALIZER;

/* next inode number that will be assigned
 * (fuse_netnodes_lock must be write locked, when touching this)
 */
ino_t fuse_next_inode = 1;



static unsigned int
fuse_netnode_hash_value(const char *path)
{
  unsigned int hash = 0, a = 23, b = 251;

  while(*path)
    {
      hash = hash * a + *(path ++);
      a *= b;
    }

  return hash % HASH_BUCKETS;
}



struct netnode *
fuse_make_netnode(struct netnode *parent, const char *path)
{
  struct hash_element *hash_el;
  struct netnode *nn;

  unsigned int hash_value = fuse_netnode_hash_value(path);
  DEBUG("make_netnode", "hash for '%s' is %u\n", path, hash_value);

  DEBUG("netnodes_lock", "aquiring pthread_rwlock_rdlock for %s.\n", path);
  pthread_rwlock_rdlock(&fuse_netnodes_lock);

  hash_el = &fuse_netnodes[hash_value];
  if(hash_el->nn)
    do
      if(! strcmp(hash_el->nn->path, path))
	{
	  nn = hash_el->nn;
	  pthread_rwlock_unlock(&fuse_netnodes_lock);
	  DEBUG("netnodes_lock", "releasing pthread_rwlock_rdlock.\n");

	  return nn;
	}
    while((hash_el = hash_el->next));

  pthread_rwlock_unlock(&fuse_netnodes_lock);
  DEBUG("netnodes_lock", "releasing pthread_rwlock_rdlock.\n");

  nn = calloc(1, sizeof(*nn));
  if(! nn)
    return NULL; /* unfortunately we cannot serve a netnode .... */

  nn->path = strdup(path);
  nn->parent = parent;
  pthread_mutex_init(&nn->lock, NULL);

  DEBUG("netnodes_lock", "aquiring pthread_rwlock_wrlock for %s.\n", path);
  pthread_rwlock_wrlock(&fuse_netnodes_lock);

  nn->inode = fuse_next_inode ++;

  /* unable to find hash element, need to generate a new one */
  if(! hash_el) 
    hash_el = &fuse_netnodes[hash_value];

  if(hash_el->nn)
    {
      struct hash_element *new = malloc(sizeof(*new));

      if(! new) {
	pthread_rwlock_unlock(&fuse_netnodes_lock);
	DEBUG("netnodes_lock", "releasing pthread_rwlock_wrlock.\n");
	free(nn);
	return NULL; /* can't help, sorry. */
      }

      /* enqueue new hash_element */
      new->next = hash_el->next;
      hash_el->next = new;

      hash_el = new;
    }

  hash_el->nn = nn;

  pthread_rwlock_unlock(&fuse_netnodes_lock);
  DEBUG("netnodes_lock", "releasing pthread_rwlock_wrlock.\n");
  return nn;
}


/* scan the whole netnode hash and call fuse_ops->fsync on every node,
 * that may be out of sync
 */
error_t
fuse_sync_filesystem(void)
{
  int i;
  error_t err = 0;

  if(! FUSE_OP_HAVE(fsync))
    return err; /* success */

  /* make sure, nobody tries to confuse us */
  pthread_rwlock_wrlock(&fuse_netnodes_lock);

  for(i = 0; i < HASH_BUCKETS; i ++)
    {
      struct hash_element *he = &fuse_netnodes[i];

      if(he->nn)
	do
	  {
	    if(he->nn->may_need_sync)
	      {
		err = -FUSE_OP_CALL(fsync, he->nn->path, 0,
				    NN_INFO(he));
		if(err)
		  goto out;
		else
		  he->nn->may_need_sync = 0;
	      }

	    if(he->nn->node)
	      DEBUG("netnode-scan", "netnode=%s has attached node\n",
		    he->nn->path);
	  }
	while((he = he->next));
    }

 out:
  pthread_rwlock_unlock(&fuse_netnodes_lock);
  return err;
}
