/**********************************************************
 * netfs.c
 *
 * Copyright(C) 2004, 2005 by Stefan Siegl <ssiegl@gmx.de>, Germany
 * 
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Publice License,
 * version 2 or any later. The license is contained in the COPYING
 * file that comes with the fuse4hurd distribution.
 *
 * callback functions for libnetfs
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stddef.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <hurd/netfs.h>

#include <stdio.h>

#include "fuse_i.h"
#include "fuse.h"





/* fuse_dirhandle, passed to ops->getdir to store our information */
struct fuse_dirhandle {
  /*** stuff needed for attempt_lookup *********************/
  unsigned found :1;

  /*** things needed for get_dirents ***********************/
  int first_entry;         /* index of first entry to return */
  int num_entries;         /* max. number of entries to return */
  int count;               /* number of elements, we will return */
  vm_size_t max_data_len;  /* max. size of memory we may allocate */
  size_t size;             /* number of bytes necessary to return data */
  struct dirent *hdrpos;   /* where to write next dirent structure */
  int max_name_len;        /* length of longest filename */
  struct netnode *parent;  /* netnode of the dir which's content we list */

  /*** things used here and there **************************/
  char *abspath;
  char *filename;
};

static int fuse_dirent_helper(fuse_dirh_t handle, const char *name,
			      int type, ino_t ino);
static int fuse_dirent_helper_compat(fuse_dirh_t handle, const char *name,
				     int type);


/* Make sure that NP->nn_stat is filled with current information.  CRED
   identifies the user responsible for the operation.  */
error_t
netfs_validate_stat (struct node *node, struct iouser *cred)
{
  (void) cred;

  FUNC_PROLOGUE_NODE("netfs_validate_stat", node);
  error_t err = EOPNOTSUPP;

  if(FUSE_OP_HAVE(getattr))
    err = -FUSE_OP_CALL(getattr, node->nn->path, &node->nn_stat);

  if(! err)
    {
      if(! fuse_use_ino)
	node->nn_stat.st_ino = node->nn->inode;

      node->nn_stat.st_dev = getpid();
      node->nn_stat.st_blksize = 1 << 12; /* there's probably no sane default,
					   * use 4 kB for the moment */
    }

  FUNC_EPILOGUE(err);
}



/* Read the contents of NODE (a symlink), for USER, into BUF. */
error_t netfs_attempt_readlink (struct iouser *user, struct node *node,
				char *buf)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_readlink", node);
  error_t err = EOPNOTSUPP;

  if((err = netfs_validate_stat(node, user))
     || (err = fshelp_access(&node->nn_stat, S_IREAD, user)))
    goto out;

  if(FUSE_OP_HAVE(readlink))
    err = -FUSE_OP_CALL(readlink, node->nn->path, buf, INT_MAX);

 out:
  FUNC_EPILOGUE(err);
}



/* Attempt to create a file named NAME in DIR for USER with MODE.  Set *NODE
   to the new node upon return.  On any error, clear *NODE.  *NODE should be
   locked on success; no matter what, unlock DIR before returning.  */
error_t
netfs_attempt_create_file (struct iouser *user, struct node *dir,
			   char *name, mode_t mode, struct node **node)
{
  FUNC_PROLOGUE("netfs_attempt_create_file");
  error_t err = EOPNOTSUPP;
  char *path = NULL;

  if(! FUSE_OP_HAVE(mknod))
    goto out;

  if((err = netfs_validate_stat(dir, user))
     || (err = fshelp_checkdirmod(&dir->nn_stat, NULL, user)))
    goto out;

  if(! (path = malloc(strlen(dir->nn->path) + strlen(name) + 2)))
    {
      err = ENOMEM;
      goto out;
    }

  sprintf(path, "%s/%s", dir->nn->path, name);

  /* FUSE expects us to use mknod function to create files. This is allowed
   * on Linux, however neither the Hurd nor the POSIX standard consider that
   */
  err = -FUSE_OP_CALL(mknod, path, (mode & ALLPERMS) | S_IFREG, 0);

  /* If available, call chown to make clear which uid/gid to assign to the
   * new file.  Testing with 'fusexmp' I noticed that new files might be
   * created with wrong gids -- root instead of $user in my case  :(
   *
   * TODO reconsider whether we should setuid/setgid the FUSE_OP_HAVE(mknod)
   * call instead (especially if mknod is not available or returns errors)
   */
  if(! err && FUSE_OP_HAVE(chown)) {
    assert(user->uids->ids[0]);
    assert(user->gids->ids[0]);

    (void)FUSE_OP_CALL(chown, path, user->uids->ids[0], user->gids->ids[0]);
  }

 out:
  if(err)
    *node = NULL;
  else
    {
      /* create a new (net-)node for this file */
      struct netnode *nn = fuse_make_netnode(dir->nn, path);

      if(nn)
	{
	  nn->may_need_sync = 1;
	  *node = fuse_make_node(nn);
	}
      else
	{
	  *node = NULL;
	  err = ENOMEM;
	}
    }

  free(path); /* fuse_make_netnode strdup'ed it. */

  if(*node)
    mutex_lock(&(*node)->lock);

  mutex_unlock (&dir->lock);

  FUNC_EPILOGUE(err);
}



/* This should attempt a chmod call for the user specified by CRED on node
   NODE, to change the owner to UID and the group to GID. */
error_t netfs_attempt_chown (struct iouser *cred, struct node *node,
			     uid_t uid, uid_t gid)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_chown", node);
  error_t err = EOPNOTSUPP;

  if(! FUSE_OP_HAVE(chown))
    goto out;

  if((err = netfs_validate_stat(node, cred))
     || (err = fshelp_isowner(&node->nn_stat, cred)))
    goto out;

  /* FIXME, make sure, that user CRED is not able to change permissions
   * to somebody who is not. That is, don't allow $unpriv_user to change
   * owner to e.g. root.
   */

  err = -FUSE_OP_CALL(chown, node->nn->path, uid, gid);
  node->nn->may_need_sync = 1;
  
 out:
  FUNC_EPILOGUE(err);
}



/* This should attempt to fetch filesystem status information for the remote
   filesystem, for the user CRED. */
error_t
netfs_attempt_statfs (struct iouser *cred, struct node *node,
		      fsys_statfsbuf_t *st)
{
  (void) cred;

  FUNC_PROLOGUE_NODE("netfs_attempt_statfs", node);
  error_t err = EOPNOTSUPP;

  if(FUSE_OP_HAVE(statfs))
    err = -FUSE_OP_CALL(statfs, node->nn->path, st);

  FUNC_EPILOGUE(err);
}



/* Attempt to create a new directory named NAME in DIR for USER with mode
   MODE.  */
error_t netfs_attempt_mkdir (struct iouser *user, struct node *dir,
			     char *name, mode_t mode)
{
  FUNC_PROLOGUE("netfs_attempt_mkdir");
  error_t err = EOPNOTSUPP;
  char *path = NULL;

  if(! FUSE_OP_HAVE(mkdir))
    goto out;

  if((err = netfs_validate_stat(dir, user))
     || (err = fshelp_checkdirmod(&dir->nn_stat, NULL, user)))
    goto out;

  if(! (path = malloc(strlen(dir->nn->path) + strlen(name) + 2)))
    {
      err = ENOMEM;
      goto out;
    }

  sprintf(path, "%s/%s", dir->nn->path, name);

  err = -FUSE_OP_CALL(mkdir, path, mode & ALLPERMS);

  /* If available, call chown to make clear which uid/gid to assign to the
   * new file.  Testing with 'fusexmp' I noticed that new files might be
   * created with wrong gids -- root instead of $user in my case  :(
   *
   * TODO reconsider whether we should setuid/setgid the FUSE_OP_HAVE(mknod)
   * call instead (especially if mknod is not available or returns errors)
   */
  if(! err && FUSE_OP_HAVE(chown)) {
    assert(user->uids->ids[0]);
    assert(user->gids->ids[0]);

    (void)FUSE_OP_CALL(chown, path, user->uids->ids[0], user->gids->ids[0]);
  }

 out:
  /* we don't need to make a netnode already, lookup will be called and do
   * that for us.
   *
   * FIXME we must make sure, that the netnode will have may_need_sync is set
   */
  free(path);
  FUNC_EPILOGUE(err);
}



/* This should attempt a chflags call for the user specified by CRED on node
   NODE, to change the flags to FLAGS. */
error_t netfs_attempt_chflags (struct iouser *cred, struct node *node,
			       int flags)
{
  (void) cred;
  (void) flags;

  FUNC_PROLOGUE_NODE("netfs_attempt_chflags", node);
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(EOPNOTSUPP);
}



/* Node NODE is being opened by USER, with FLAGS.  NEWNODE is nonzero if we
   just created this node.  Return an error if we should not permit the open
   to complete because of a permission restriction. */
error_t
netfs_check_open_permissions (struct iouser *user, struct node *node,
			      int flags, int newnode)
{
  (void) newnode;

  FUNC_PROLOGUE_NODE("netfs_check_open_permissions", node);
  error_t err = 0;

  if((err = netfs_validate_stat(node, user)))
    goto out;

  if (flags & O_READ)
    err = fshelp_access (&node->nn_stat, S_IREAD, user);
      
  if (!err && (flags & O_WRITE))
    err = fshelp_access (&node->nn_stat, S_IWRITE, user);
      
  if (!err && (flags & O_EXEC))
    err = fshelp_access (&node->nn_stat, S_IEXEC, user);

 out:
  FUNC_EPILOGUE(err);
}



/* This should attempt a chmod call for the user specified by CRED on node
   NODE, to change the mode to MODE.  Unlike the normal Unix and Hurd meaning
   of chmod, this function is also used to attempt to change files into other
   types.  If such a transition is attempted which is impossible, then return
   EOPNOTSUPP.  */
error_t netfs_attempt_chmod (struct iouser *cred, struct node *node,
			     mode_t mode)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_chmod", node);
  error_t err = EOPNOTSUPP;

  if(! FUSE_OP_HAVE(chmod))
    goto out;

  if((err = netfs_validate_stat(node, cred))
     || (err = fshelp_isowner(&node->nn_stat, cred)))
    goto out;

  err = -FUSE_OP_CALL(chmod, node->nn->path, mode);
  node->nn->may_need_sync = 1;
  
 out:
  FUNC_EPILOGUE(err);
}



/* Attempt to create an anonymous file related to DIR for USER with MODE.
   Set *NODE to the returned file upon success.  No matter what, unlock DIR. */
error_t netfs_attempt_mkfile (struct iouser *user, struct node *dir,
			      mode_t mode, struct node **node)
{
  FUNC_PROLOGUE("netfs_attempt_mkfile");
  error_t err = EOPNOTSUPP;
  char name[20];
  static int num = 0;

  if(FUSE_OP_HAVE(mknod))
    if(! (err = netfs_validate_stat(dir, user)))
      err = fshelp_checkdirmod(&dir->nn_stat, NULL, user);

  if(err)
    {
      /* dir has to be unlocked no matter what ... */
      mutex_unlock (&dir->lock);
      goto out;
    }

  /* call netfs_attempt_create_file with O_EXCL and O_CREAT bits set */
  mode |= O_EXCL | O_CREAT;

  do 
    {
      snprintf(name, sizeof(name), ".fuse4hurd-%06d", num ++);
      err = netfs_attempt_create_file(user, dir, name, mode, node);

      if(err == EEXIST)
	mutex_lock(&dir->lock); /* netfs_attempt_create_file just unlocked
				 * it for us, however we need to call it once
				 * more ...
				 */
    } 
  while(err == EEXIST);

 out:
  if(err) 
    *node = 0;
  else
    (*node)->nn->anonymous = 1; /* mark netnode of created file as anonymous,
				 * i.e. mark it as to delete in noref routine
				 */

  /* we don't have to mark the netnode as may_need_sync, as
   * netfs_attempt_create_file has already done that for us
   */

  /* mutex_unlock (&dir->lock);
   * netfs_attempt_create_file already unlocked the node for us.
   */
  FUNC_EPILOGUE(err);
}



/* This should sync the entire remote filesystem.  If WAIT is set, return
   only after sync is completely finished.  */
error_t netfs_attempt_syncfs (struct iouser *cred, int wait)
{
  (void) cred;   /* cannot use anything but translator's rights,
		  * FIXME, maybe setuid/setgid? */
  (void) wait;   /* there's no such flag in libfuse */

  FUNC_PROLOGUE("netfs_attempt_syncfs");
  error_t err = fuse_sync_filesystem();

  FUNC_EPILOGUE(err);
}



/* This should sync the file NODE completely to disk, for the user CRED.  If
   WAIT is set, return only after sync is completely finished.  */
error_t
netfs_attempt_sync (struct iouser *cred, struct node *node, int wait)
{
  (void) wait;   /* there's no such flag in libfuse */

  FUNC_PROLOGUE_NODE("netfs_attempt_sync", node);
  error_t err = EOPNOTSUPP;

  if(! FUSE_OP_HAVE(fsync))
    goto out;

  if((err = netfs_validate_stat(node, cred))
     || (err = fshelp_access(&node->nn_stat, S_IWRITE, cred)))
    goto out;

  if(fuse_ops)
    err = -fuse_ops->fsync(node->nn->path, 0, &node->nn->info);
  else
    err = -fuse_ops_compat->fsync(node->nn->path, 0);
        
  if(! err)
    node->nn->may_need_sync = 0; 

 out:
  FUNC_EPILOGUE(err);
}



/* Delete NAME in DIR for USER.
 * The node DIR is locked, and shall stay locked
 */
error_t netfs_attempt_unlink (struct iouser *user, struct node *dir,
			      char *name)
{
  FUNC_PROLOGUE("netfs_attempt_unlink");
  error_t err = EOPNOTSUPP;
  struct node *node = NULL;

  if(! FUSE_OP_HAVE(unlink))
    goto out;

  err = netfs_attempt_lookup(user, dir, name, &node);
  assert(dir != node);
  mutex_lock(&dir->lock); /* re-lock directory, since netfs_attempt_lookup
			   * unlocked it for us
			   */  
  if(err)
    goto out;

  mutex_unlock(&node->lock);

  if((err = netfs_validate_stat(dir, user))
     || (err = netfs_validate_stat(node, user))
     || (err = fshelp_checkdirmod(&dir->nn_stat, &node->nn_stat, user)))
    goto out;

  err = -FUSE_OP_CALL(unlink, node->nn->path);

  /* TODO free associated netnode. really? 
   * FIXME, make sure nn->may_need_sync is set */
 out:
  if(node)
    netfs_nrele(node);

  FUNC_EPILOGUE(err);
}



/* This should attempt to set the size of the file NODE (for user CRED) to
   SIZE bytes long. */
error_t netfs_attempt_set_size (struct iouser *cred, struct node *node,
				loff_t size)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_set_size", node);
  error_t err = EOPNOTSUPP;

  if(! FUSE_OP_HAVE(truncate))
    goto out;

  if((err = netfs_validate_stat(node, cred))
     || (err = fshelp_access(&node->nn_stat, S_IWRITE, cred)))
    goto out;

  err = -FUSE_OP_CALL(truncate, node->nn->path, size);

 out:
  FUNC_EPILOGUE(err);
}



/* Attempt to turn NODE (user CRED) into a device.  TYPE is either S_IFBLK or
   S_IFCHR. */
error_t netfs_attempt_mkdev (struct iouser *cred, struct node *node,
			     mode_t type, dev_t indexes)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_mkdev", node);
  error_t err = EOPNOTSUPP;

  if(! FUSE_OP_HAVE(mknod))
    goto out;

  /* we need to unlink the existing node, therefore, if unlink is not
   * available, we cannot turn *node into a device.
   */
  if(! FUSE_OP_HAVE(unlink))
    goto out;

  /* check permissions
   * XXX, shall we check permissions of the parent directory as well,
   * since we're going to unlink files?
   */

  if((err = netfs_validate_stat(node, cred))
     || (err = fshelp_access(&node->nn_stat, S_IWRITE, cred)))
    goto out;

  /* unlink the already existing node, to be able to create the (new)
   * device file
   */
  if((err = -FUSE_OP_CALL(unlink, node->nn->path)))
    goto out;

  err = -FUSE_OP_CALL(mknod, node->nn->path,
			 type & (ALLPERMS | S_IFBLK | S_IFCHR), indexes);

  /* If available, call chown to make clear which uid/gid to assign to the
   * new file.  Testing with 'fusexmp' I noticed that new files might be
   * created with wrong gids -- root instead of $user in my case  :(
   *
   * TODO reconsider whether we should setuid/setgid the FUSE_OP_HAVE(mknod)
   * call instead (especially if mknod is not available or returns errors)
   */
  if(! err && FUSE_OP_HAVE(chown)) {
    assert(cred->uids->ids[0]);
    assert(cred->gids->ids[0]);

    (void)FUSE_OP_CALL(chown, node->nn->path, cred->uids->ids[0],
			  cred->gids->ids[0]);
  }

  node->nn->may_need_sync = 1;

 out:
  FUNC_EPILOGUE(err);
}



/* Return the valid access types (bitwise OR of O_READ, O_WRITE, and O_EXEC)
   in *TYPES for file NODE and user CRED.  */
error_t
netfs_report_access (struct iouser *cred, struct node *node, int *types)
{
  FUNC_PROLOGUE_NODE("netfs_report_access", node);
  error_t err;
  *types = 0;

  if((err = netfs_validate_stat(node, cred)))
    goto out;
  
  if (fshelp_access (&node->nn_stat, S_IREAD, cred) == 0)
    *types |= O_READ;
  
  if (fshelp_access (&node->nn_stat, S_IWRITE, cred) == 0)
    *types |= O_WRITE;

  if (fshelp_access (&node->nn_stat, S_IEXEC, cred) == 0)
    *types |= O_EXEC;
  
 out:
  FUNC_EPILOGUE(0);
}


/* callback-helper of netfs_attempt_lookup, check whether the name of
 * file (of this callback) is equal to the one we're looking for, in case
 * set 'found' from fuse_dirh_t handle to TRUE.
 *
 * version for new fuse API (since 2.1)
 */
static int
fuse_lookup_helper(fuse_dirh_t handle, const char *name, int type, ino_t ino)
{
  (void) type; /* we want to know whether the file exists at all,
		* the type is not of any interest */
  (void) ino;  /* we don't care for inodes here, netfs_validate_stat does it */

  if(! strcmp(name, handle->filename))
    {
      handle->found = 1;
      return ENOMEM; /* send ENOMEM to stop being called, TODO make sure that
		      * all programs, depending on libfuse, can live with that.
		      */
    }

  return 0;
}


/* callback-helper of netfs_attempt_lookup, check whether the name of
 * file (of this callback) is equal to the one we're looking for, in case
 * set 'found' from fuse_dirh_t handle to TRUE.
 *
 * version for old API
 */
static int
fuse_lookup_helper_compat(fuse_dirh_t handle, const char *name, int type)
{
  assert(fuse_use_ino == 0);
  return fuse_lookup_helper(handle, name, type, 0); /* set ino to 0, it will
						     * be ignored anyways,
						     * since fuse_use_ino 
						     * is off.     */
}

/* Lookup NAME in DIR for USER; set *NODE to the found name upon return.  If
   the name was not found, then return ENOENT.  On any error, clear *NODE.
   (*NODE, if found, should be locked, this call should unlock DIR no matter
   what.) */
error_t netfs_attempt_lookup (struct iouser *user, struct node *dir,
			      char *name, struct node **node)
{
  FUNC_PROLOGUE_FMT("netfs_attempt_lookup", "name=%s, dir=%s",
		    name, dir->nn->path);

  error_t err;

  if((err = netfs_validate_stat(dir, user))
     || (err = fshelp_access(&dir->nn_stat, S_IREAD, user))
     || (err = fshelp_access(&dir->nn_stat, S_IEXEC, user)))
    goto out;
  else
    err = ENOENT; /* default to return ENOENT */

  if(! strcmp(name, "."))
    {
      /* lookup for current directory, return another refernce to it */
      netfs_nref(dir);
      *node = dir;
      err = 0; /* it's alright ... */
    }

  else if(! strcmp(name, ".."))
    {
      if(dir->nn->parent)
	{
	  /* okay, there is a parent directory, return a reference to that */
	  *node = fuse_make_node(dir->nn->parent);
	  err = 0;
	}
      else
	/* cannot go up from top directory */
	err = EAGAIN;
    }

  else if(FUSE_OP_HAVE(getdir))
    {
      /* lookup for common file */
      struct netnode *nn;
      char *path;
      fuse_dirh_t handle = malloc(sizeof(struct fuse_dirhandle));

      if(! handle)
	{
	  err = ENOMEM; /* sorry, translator not available ... */
	  goto out;
	}

      handle->found = 0;
      handle->filename = name;
	
      if(fuse_ops)
	fuse_ops->getdir(dir->nn->path, handle, fuse_lookup_helper);
      else
	fuse_ops_compat->getdir(dir->nn->path, handle,
				fuse_lookup_helper_compat);

      /* we cannot rely on exit status of ->getdir() func, since we
       * return an error from the helper to abort write out
       */

      if(! handle->found)
	{
	  err = ENOENT;
	  goto out;
	}

      /* well, file exists - create a handle ... */
      if(! (path = malloc(strlen(dir->nn->path) + strlen(name) + 2)))
	{
	  err = ENOMEM;
	  goto out;
	}

      sprintf(path, "%s/%s", dir->nn->parent ? dir->nn->path : "", name);
      nn = fuse_make_netnode(dir->nn, path);
      free(path); /* fuse_make_netnode strdup()s the pathname */

      if(! path)
	{
	  err = ENOMEM;
	  goto out;
	}

      if((*node = fuse_make_node(nn)))
	err = 0;
    }

out:
  mutex_unlock(&dir->lock);

  if(err)
    *node = NULL;
  else
    mutex_lock(&(*node)->lock);

  FUNC_EPILOGUE(err);
}



/* Create a link in DIR with name NAME to FILE for USER.  Note that neither
   DIR nor FILE are locked.  If EXCL is set, do not delete the target, but
   return EEXIST if NAME is already found in DIR.  */
error_t netfs_attempt_link (struct iouser *user, struct node *dir,
			    struct node *file, char *name, int excl)
{
  FUNC_PROLOGUE_FMT("netfs_attempt_link", "link=%s/%s, to=%s",
		    dir->nn->path, name, file->nn->path);
  error_t err = EOPNOTSUPP;
  struct node *node = NULL;

  if(! FUSE_OP_HAVE(link))
    goto out_nounlock;

  mutex_lock(&dir->lock);

  if((err = netfs_attempt_lookup(user, dir, name, &node)))
    goto out_nounlock; /* netfs_attempt_lookup unlocked dir */

  assert(dir != node);
  mutex_lock(&dir->lock);

  if((err = netfs_validate_stat(node, user))
     || (err = fshelp_checkdirmod(&dir->nn_stat, &node->nn_stat, user)))
    goto out;

  if(! excl && FUSE_OP_HAVE(unlink))
    /* EXCL is not set, therefore we may remove the target, i.e. call
     * unlink on it.  Ignoring return value, as it's mostly not interesting,
     * since the file does not exist in most case
     */
    (void) FUSE_OP_CALL(unlink, node->nn->path);

  err = -FUSE_OP_CALL(link, file->nn->path, node->nn->path);

  /* TODO
   * create a netnode with the may_need_sync flag set!!   */

  /* If available, call chown to make clear which uid/gid to assign to the
   * new file.  Testing with 'fusexmp' I noticed that new files might be
   * created with wrong gids -- root instead of $user in my case  :(
   *
   * TODO reconsider whether we should setuid/setgid the FUSE_OP_HAVE(mknod)
   * call instead (especially if mknod is not available or returns errors)
   */
  /* if(! err && FUSE_OP_HAVE(chown))
   *   {
   *     assert(user->uids->ids[0]);
   *     assert(user->gids->ids[0]);
   *
   *     (void)FUSE_OP_CALL(chown, path, user->uids->ids[0], user->gids->ids[0]);
   *   }
   */
  /* FIXME
   * This is most probably not a good idea to do here, as it would change
   * the user and group-id of the other (linked) files as well, sharing the
   * same inode.
   */

 out:
  mutex_unlock(&dir->lock);

  mutex_unlock(&node->lock);
  netfs_nrele(node);

 out_nounlock:
  FUNC_EPILOGUE(err);
}



/* Attempt to remove directory named NAME in DIR for USER.
 * directory DIR is locked, and shall stay locked. */
error_t netfs_attempt_rmdir (struct iouser *user,
			     struct node *dir, char *name)
{
  FUNC_PROLOGUE("netfs_attempt_rmdir");
  error_t err = EOPNOTSUPP;
  struct node *node = NULL;

  if(! FUSE_OP_HAVE(rmdir))
    goto out_nounlock;

  err = netfs_attempt_lookup(user, dir, name, &node);
  assert(dir != node);
  mutex_lock(&dir->lock); /* netfs_attempt_lookup unlocked dir */

  if(err)
    goto out_nounlock; 

  if((err = netfs_validate_stat(node, user))
     || (err = fshelp_checkdirmod(&dir->nn_stat, &node->nn_stat, user)))
    goto out;

  err = -FUSE_OP_CALL(rmdir, node->nn->path);

  /* TODO free associated netnode. really? 
   * FIXME, make sure nn->may_need_sync is set */

 out:
  mutex_unlock(&node->lock);
  netfs_nrele(node);

 out_nounlock:
  FUNC_EPILOGUE(err);
}



/* This should attempt a chauthor call for the user specified by CRED on node
   NODE, to change the author to AUTHOR. */
error_t netfs_attempt_chauthor (struct iouser *cred, struct node *node,
				uid_t author)
{
  (void) cred;
  (void) author;

  FUNC_PROLOGUE_NODE("netfs_attempt_chauthor", node);
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(EROFS);
}



/* Attempt to turn NODE (user CRED) into a symlink with target NAME. */
error_t netfs_attempt_mksymlink (struct iouser *cred, struct node *node,
				 char *name)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_mksymlink", node);
  error_t err = EOPNOTSUPP;

  /* we need to unlink the existing node, therefore, if unlink is not
   * available, we cannot create symlinks
   */
  if(! FUSE_OP_HAVE(unlink))
    goto out;

  /* symlink function available? if not, fail. */
  if(! FUSE_OP_HAVE(symlink))
    goto out;

  /* check permissions
   * XXX, shall we check permissions of the parent directory as well,
   * since we're going to unlink files?
   */

  if((err = netfs_validate_stat(node, cred))
     || (err = fshelp_access(&node->nn_stat, S_IWRITE, cred)))
    goto out;

  /* try to remove the existing node (probably an anonymous file) */
  if((err = -FUSE_OP_CALL(unlink, node->nn->path)))
    goto out;

  err = -FUSE_OP_CALL(symlink, name, node->nn->path);

  /* we don't have to adjust nodes/netnodes, as these are already existing.
   * netfs_attempt_mkfile did that for us.
   */
  if(! err)
    node->nn->may_need_sync = 1;

 out:
  FUNC_EPILOGUE(err);
}



/* Note that in this one call, neither of the specific nodes are locked. */
error_t netfs_attempt_rename (struct iouser *user, struct node *fromdir,
			      char *fromname, struct node *todir,
			      char *toname, int excl)
{
  FUNC_PROLOGUE("netfs_attempt_rename");
  error_t err = EOPNOTSUPP;
  struct node *fromnode;
  char *topath = NULL;

  if(! FUSE_OP_HAVE(rename))
    goto out_nounlock; 

  if(! (topath = malloc(strlen(toname) + strlen(todir->nn->path) + 2)))
    {
      err = ENOMEM;
      goto out_nounlock;
    }

  mutex_lock(&fromdir->lock);

  if(netfs_attempt_lookup(user, fromdir, fromname, &fromnode))
    {
      err = ENOENT;
      goto out_nounlock; /* netfs_attempt_lookup unlocked fromdir and locked
			  * fromnode for us. 
			  */
    }

  mutex_lock(&todir->lock);

  if((err = netfs_validate_stat(fromdir, user))
     || fshelp_checkdirmod(&fromdir->nn_stat, &fromnode->nn_stat, user))
    goto out;

  if((err = netfs_validate_stat(todir, user))
     || fshelp_checkdirmod(&todir->nn_stat, NULL, user))
    goto out;

  sprintf(topath, "%s/%s", todir->nn->path, toname);

  if(! excl && FUSE_OP_HAVE(unlink))
    /* EXCL is not set, therefore we may remove the target, i.e. call
     * unlink on it.  Ignoring return value, as it's mostly not interesting,
     * since the file does not exist in most case
     */
    (void) FUSE_OP_CALL(unlink, topath);

  err = -FUSE_OP_CALL(rename, fromnode->nn->path, topath);

  if(! err) 
    {
      struct netnode *nn;

      if(! (nn = fuse_make_netnode(todir->nn, topath)))
	goto out;

      nn->may_need_sync = 1;
      /* FIXME mark fromnode to destroy it's associated netnode */
    }

 out:
  free(topath);
  mutex_unlock(&todir->lock);

  mutex_unlock(&fromnode->lock);
  netfs_nrele(fromnode);

 out_nounlock:
  FUNC_EPILOGUE(err);
}



/* Write to the file NODE for user CRED starting at OFSET and continuing for up
   to *LEN bytes from DATA.  Set *LEN to the amount seccessfully written upon
   return. */
error_t netfs_attempt_write (struct iouser *cred, struct node *node,
			     loff_t offset, size_t *len, void *data)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_write", node);
  error_t err = EOPNOTSUPP;

  if(! FUSE_OP_HAVE(write))
    goto out;

  if((err = netfs_validate_stat(node, cred))
     || (err = fshelp_access(&node->nn_stat, S_IWRITE, cred)))
    goto out;

  node->nn->info.writepage = 0; /* cannot distinct on the Hurd :( */

  int sz = fuse_ops ? 
    (fuse_ops->write(node->nn->path, data, *len, offset, &node->nn->info)) : 
    (fuse_ops_compat->write(node->nn->path, data, *len, offset));
     
  if(sz < 0)
    err = -sz;
  else
    *len = sz;

 out:
  FUNC_EPILOGUE(err);
}



/* This should attempt a utimes call for the user specified by CRED on node
   NODE, to change the atime to ATIME and the mtime to MTIME. */
error_t
netfs_attempt_utimes (struct iouser *cred, struct node *node,
		      struct timespec *atime, struct timespec *mtime)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_utimes", node);
  error_t err = EOPNOTSUPP;

  /* prepare utimebuf for FUSE_OP_HAVE(utime) call */
  struct utimbuf utb;
  utb.actime = atime ? atime->tv_sec : node->nn_stat.st_atime;
  utb.modtime = mtime ? mtime->tv_sec : node->nn_stat.st_mtime;

  /* test whether operation is supported and permission are sufficient */
  if(! FUSE_OP_HAVE(utime))
    goto out;

  if((err = netfs_validate_stat(node, cred))
     || (err = fshelp_isowner(&node->nn_stat, cred)))
    goto out;

  err = -FUSE_OP_CALL(utime, node->nn->path, &utb);

  if (! err)
    {
      node->nn_stat.st_mtime = utb.modtime;
      node->nn_stat.st_mtime_usec = 0;
      
      node->nn_stat.st_atime = utb.actime;
      node->nn_stat.st_atime_usec = 0;
	
      node->nn->may_need_sync = 1;
    }

 out:
  FUNC_EPILOGUE(err);
}



/* Read from the file NODE for user CRED starting at OFFSET and continuing for
   up to *LEN bytes.  Put the data at DATA.  Set *LEN to the amount
   successfully read upon return.  */
error_t netfs_attempt_read (struct iouser *cred, struct node *node,
			    loff_t offset, size_t *len, void *data)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_read", node);
  error_t err = EOPNOTSUPP;

  if(! FUSE_OP_HAVE(read))
    goto out;

  if((err = netfs_validate_stat(node, cred))
     || (err = fshelp_access(&node->nn_stat, S_IREAD, cred)))
    goto out;

  int sz = fuse_ops ? 
    (fuse_ops->read(node->nn->path, data, *len, offset, &node->nn->info)) : 
    (fuse_ops_compat->read(node->nn->path, data, *len, offset));

  if(sz < 0)
    err = -sz;
  else
    {
      err = 0;
      *len = sz;
    }

 out:
  FUNC_EPILOGUE(err);
}



/* Returned directory entries are aligned to blocks this many bytes long.
   Must be a power of two.  */
#define DIRENT_ALIGN 4
#define DIRENT_NAME_OFFS offsetof (struct dirent, d_name)

/* Length is structure before the name + the name + '\0', all
   padded to a four-byte alignment.  */
#define DIRENT_LEN(name_len)						      \
  ((DIRENT_NAME_OFFS + (name_len) + 1 + (DIRENT_ALIGN - 1))		      \
   & ~(DIRENT_ALIGN - 1))

/* callback handler used by netfs_get_dirents to calculate amount of 
 * memory necessary to return directory entries 
 */
static int
fuse_bump_helper(fuse_dirh_t handle, const char *name, int type)
{
  (void) type; /* we just allocate the memory, type is not of interest
		* for that .. */

  if(handle->first_entry)
    {
      /* skip this entry, it's before the first one we got to write out ... */
      handle->first_entry --;
      return 0;
    }

  if(handle->num_entries || handle->count < handle->num_entries)
    {
      int name_len = strlen(name);
      size_t new_size = handle->size + DIRENT_LEN(name_len);

      handle->max_name_len =
	handle->max_name_len < name_len ? name_len : handle->max_name_len;

      if(handle->max_data_len && new_size > handle->max_data_len)
	return 0; /* not enough space left */

      handle->size = new_size;
      handle->count ++;
    }
  return 0;
}



/* callback handler used by netfs_get_dirents to write our dirents
 * to the mmaped memory
 *
 * version for new fuse api
 */
static int
fuse_dirent_helper(fuse_dirh_t handle, const char *name, int type, ino_t ino)
{
  size_t name_len;
  size_t dirent_len;
  struct netnode *nn;
  size_t inode;

  if(! fuse_use_ino)
    return fuse_dirent_helper_compat(handle, name, type);

  ino_t fuse_get_inode(const char *name)
    {
      struct stat stat;

      assert(fuse_ops);
      assert(fuse_ops->getattr);

      fuse_ops->getattr(name, &stat);
      return stat.st_ino;
    }

  if(handle->first_entry)
    {
      /* skip this entry, it's before the first one we got to write out ... */
      handle->first_entry --;
      return 0;
    }

  if(! handle->count)
    /* already wrote all entries, get outta here */
    return ENOMEM;

  name_len = strlen(name);
  dirent_len = DIRENT_LEN(name_len);

  /* look the inode of this element up ... */
  if(! strcmp(name, "."))
    inode = fuse_get_inode(handle->parent->path);

  else if(handle->parent->parent && ! strcmp(name, ".."))
    inode = fuse_get_inode(handle->parent->parent->path);

  else
    {
      strcpy(handle->filename, name);
      nn = fuse_make_netnode(handle->parent, handle->abspath);
      inode = ino;
    }

  /* write out struct dirent ... */
  handle->hdrpos->d_fileno = inode;
  handle->hdrpos->d_reclen = dirent_len;
  handle->hdrpos->d_type = type;
  handle->hdrpos->d_namlen = name_len;

  /* copy file's name ... */
  memcpy(((void *) handle->hdrpos) + DIRENT_NAME_OFFS, name, name_len + 1);

  /* update hdrpos pointer */
  handle->hdrpos = ((void *) handle->hdrpos) + dirent_len;

  handle->count --;
  return 0;
}



/* callback handler used by netfs_get_dirents to write our dirents
 * to the mmaped memory
 *
 * version for old api
 */
static int
fuse_dirent_helper_compat(fuse_dirh_t handle, const char *name, int type)
{
  size_t name_len;
  size_t dirent_len;
  struct netnode *nn;
  ino_t inode;

  if(handle->first_entry)
    {
      /* skip this entry, it's before the first one we got to write out ... */
      handle->first_entry --;
      return 0;
    }

  if(! handle->count)
    /* already wrote all entries, get outta here */
    return ENOMEM;

  name_len = strlen(name);
  dirent_len = DIRENT_LEN(name_len);

  /* look the inode of this element up ... */
  if(! strcmp(name, "."))
    inode = handle->parent->inode;

  else if(handle->parent->parent && ! strcmp(name, ".."))
    inode = handle->parent->parent->inode;

  else
    {
      strcpy(handle->filename, name);
      nn = fuse_make_netnode(handle->parent, handle->abspath);
      inode = nn->inode;
    }

  /* write out struct dirent ... */
  handle->hdrpos->d_fileno = inode;
  handle->hdrpos->d_reclen = dirent_len;
  handle->hdrpos->d_type = type;
  handle->hdrpos->d_namlen = name_len;

  /* copy file's name ... */
  memcpy(((void *) handle->hdrpos) + DIRENT_NAME_OFFS, name, name_len + 1);

  /* update hdrpos pointer */
  handle->hdrpos = ((void *) handle->hdrpos) + dirent_len;

  handle->count --;
  return 0;
}


error_t
netfs_get_dirents (struct iouser *cred, struct node *dir,
		   int first_entry, int num_entries, char **data,
		   mach_msg_type_number_t *data_len,
		   vm_size_t max_data_len, int *data_entries)
{
  FUNC_PROLOGUE_NODE("netfs_get_dirents", dir);
  error_t err;
  fuse_dirh_t handle;

  if(! FUSE_OP_HAVE(getdir))
    FUNC_RETURN(EOPNOTSUPP);

  if((err = netfs_validate_stat(dir, cred))
     || (err = fshelp_access(&dir->nn_stat, S_IREAD, cred))
     || (err = fshelp_access(&dir->nn_stat, S_IEXEC, cred)))
    FUNC_RETURN(err);

  if(! (handle = malloc(sizeof(struct fuse_dirhandle))))
    FUNC_RETURN(ENOMEM); /* sorry, translator not available ... */

  handle->first_entry = first_entry;
  handle->num_entries = num_entries;
  handle->count = 0;
  handle->max_data_len = max_data_len;
  handle->size = 0;

  /* using fuse_bump_helper for new and old api, since we just allocate
   * the memory for real calls, done later.
   */
  err = FUSE_OP_CALL(getdir, dir->nn->path, handle, (void *)fuse_bump_helper);
  
  if(err)
    {
      free(handle);
      FUNC_RETURN(err);
    }

  /* allocate handle->abspath */
  {
    size_t path_len = strlen(dir->nn->path);

    if(! (handle->abspath = malloc(path_len + handle->max_name_len + 2)))
      {
	free(handle);
	FUNC_RETURN(ENOMEM);
      }

    memcpy(handle->abspath, dir->nn->path, path_len);
    handle->filename = handle->abspath + path_len;

    /* add a delimiting slash if there are parent directories */
    if(dir->nn->parent)
      *(handle->filename ++) = '/';

    handle->parent = dir->nn;
  }

  /* Allocate it.  */
  *data = mmap (0, handle->size, PROT_READ | PROT_WRITE, MAP_ANON, 0, 0);
  err = ((void *) *data == (void *) -1) ? errno : 0;

  if(! err)
    {
      /* okay, fill our memory piece now ... */
      *data_len = handle->size;
      *data_entries = handle->count;
      
      /* restore first_entry value, which got destroyed by our bump-helper */
      handle->first_entry = first_entry;

      handle->hdrpos = (struct dirent*) *data;

      if(fuse_ops)
	fuse_ops->getdir(dir->nn->path, handle, fuse_dirent_helper);
      else
	fuse_ops_compat->getdir(dir->nn->path, handle,
				fuse_dirent_helper_compat);
    }

  /* TODO: fshelp_touch ATIME here */
  FUNC_EPILOGUE_FMT(err, "%d entries.", *data_entries);
}



/* Node NP is all done; free all its associated storage. */
void
netfs_node_norefs (struct node *node)
{
  FUNC_PROLOGUE_NODE("netfs_node_norefs", node);

  DEBUG("netnode-lock", "locking netnode, path=%s\n", node->nn->path);
  mutex_lock(&node->nn->lock);

  if(node->nn->anonymous && FUSE_OP_HAVE(unlink))
    {
      /* FIXME, need to lock parent directory structure */
      FUSE_OP_CALL(unlink, node->nn->path);

      /* FIXME, free associated netnode somehow. */
    }

  node->nn->node = NULL;

  mutex_unlock(&node->nn->lock);
  DEBUG("netnode-lock", "netnode unlocked.\n"); /* no ref to node->nn av. */

  FUNC_EPILOGUE_NORET();
}
