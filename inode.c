/**********************************************************
 * inode.c
 *
 * Copyright 2004, Stefan Siegl <ssiegl@gmx.de>, Germany
 * 
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Publice License,
 * version 2 or any later. The license is contained in the COPYING
 * file that comes with the fuse4hurd distribution.
 *
 * handling inode numbers - we need to use right the same inode number to
 * a specific file over and over again; we use a hash, mapping from full
 * pathnames to the numbers.
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
  ino_t inode;
  char *path;

  struct hash_element *next; /* singly linked list */
} fuse_inode_hash[HASH_BUCKETS] = { 0 };

/* rwlock needs to be held when touching either fuse_inode_hash or
 * fuse_next_inode variable
 */
struct rwlock fuse_inode_hash_lock = RWLOCK_INITIALIZER;

/* next inode number that will be assigned
 * (fuse_inode_hash_lock must be write locked, when touching this)
 */
ino_t fuse_next_inode = 1;



static int
fuse_inode_hash_value(const char *path)
{
  int hash = 0;
  int maxlen = 8; /* use the first 8 chars for hash generation only */

  while(maxlen -- && *path)
    hash += *(path ++);

  return hash % HASH_BUCKETS;
}



ino_t
fuse_lookup_ino(const char *path)
{
  int hash_value = fuse_inode_hash_value(path);
  struct hash_element *hash_el;

  rwlock_reader_lock(&fuse_inode_hash_lock);

  hash_el = &fuse_inode_hash[hash_value];
  if(hash_el->path)
    do
      if(! strcmp(hash_el->path, path))
	{
	  ino_t inode = hash_el->inode;
	  rwlock_reader_unlock(&fuse_inode_hash_lock);

	  return inode;
	}
    while((hash_el = hash_el->next));

  rwlock_reader_unlock(&fuse_inode_hash_lock);
  rwlock_writer_lock(&fuse_inode_hash_lock);

  /* unable to find hash element, need to generate a new one */
  if(! hash_el) 
    hash_el = &fuse_inode_hash[hash_value];

  if(hash_el->path)
    {
      struct hash_element *new = malloc(sizeof(*new));

      if(! new) {
	rwlock_writer_lock(&fuse_inode_hash_lock);
	return -ENOMEM; /* can't help, sorry. */
      }

      /* enqueue new hash_element */
      new->next = hash_el->next;
      hash_el->next = new;

      hash_el = new;
    }

  hash_el->name = strdup(path);
  {
    ino_t inode = hash_el->inode = fuse_next_inode ++;
    rwlock_writer_lock(&fuse_inode_hash_lock);
    return inode;
  }
}
