/**********************************************************
 * netnode.c
 *
 * Copyright(C) 2004, 2005 by Stefan Siegl <ssiegl@gmx.de>, Germany
 * 
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Publice License,
 * version 2 or any later. The license is contained in the COPYING
 * file that comes with the fuse4hurd distribution.
 *
 * netnode handling
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <error.h>
#include <string.h>
#include <rwlock.h>
#include <hurd/netfs.h>

#include "fuse_i.h"
#include "fuse.h"

#define HASH_BUCKETS 256

struct hash_element;
static struct hash_element {
  struct netnode *nn;
  struct hash_element *next; /* singly linked list */
} fuse_netnodes[HASH_BUCKETS] = {{ 0 }};

/* rwlock needs to be held when touching either fuse_netnodes hash or
 * fuse_next_inode variable
 */
struct rwlock fuse_netnodes_lock = RWLOCK_INITIALIZER;

/* next inode number that will be assigned
 * (fuse_netnodes_lock must be write locked, when touching this)
 */
ino_t fuse_next_inode = 1;



static int
fuse_netnode_hash_value(const char *path)
{
  int hash = 0;
  int maxlen = 12; /* use the first 12 chars for hash generation only */

  while(maxlen -- && *path)
    hash += *(path ++);

  return hash % HASH_BUCKETS;
}



struct netnode *
fuse_make_netnode(struct netnode *parent, const char *path)
{
  struct hash_element *hash_el;
  struct netnode *nn;
  int hash_value = fuse_netnode_hash_value(path);

  DEBUG("netnodes_lock", "aquiring rwlock_reader_lock.\n");
  rwlock_reader_lock(&fuse_netnodes_lock);

  hash_el = &fuse_netnodes[hash_value];
  if(hash_el->nn)
    do
      if(! strcmp(hash_el->nn->path, path))
	{
	  nn = hash_el->nn;
	  rwlock_reader_unlock(&fuse_netnodes_lock);
	  DEBUG("netnodes_lock", "releasing rwlock_reader_lock.\n");

	  return nn;
	}
    while((hash_el = hash_el->next));

  rwlock_reader_unlock(&fuse_netnodes_lock);
  DEBUG("netnodes_lock", "releasing rwlock_reader_lock.\n");

  nn = malloc(sizeof(*nn));
  if(! nn)
    return NULL; /* unfortunately we cannot serve a netnode .... */

  nn->path = strdup(path);
  nn->parent = parent;
  nn->node = NULL;
  mutex_init(&nn->lock);

  DEBUG("netnodes_lock", "aquiring rwlock_writer_lock.\n");
  rwlock_writer_lock(&fuse_netnodes_lock);
  nn->inode = fuse_next_inode ++;

  /* unable to find hash element, need to generate a new one */
  if(! hash_el) 
    hash_el = &fuse_netnodes[hash_value];

  if(hash_el->nn)
    {
      struct hash_element *new = malloc(sizeof(*new));

      if(! new) {
	rwlock_writer_unlock(&fuse_netnodes_lock);
	DEBUG("netnodes_lock", "releasing rwlock_writer_lock.\n");
	free(nn);
	return NULL; /* can't help, sorry. */
      }

      /* enqueue new hash_element */
      new->next = hash_el->next;
      hash_el->next = new;

      hash_el = new;
    }

  hash_el->nn = nn;

  rwlock_writer_unlock(&fuse_netnodes_lock);
  DEBUG("netnodes_lock", "releasing rwlock_writer_lock.\n");
  return nn;
}
