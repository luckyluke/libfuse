/**********************************************************
 * main.c
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


/* Make sure that NP->nn_stat is filled with current information.  CRED
   identifies the user responsible for the operation.  */
error_t
netfs_validate_stat (struct node *node, struct iouser *cred)
{
  FUNC_PROLOGUE_NODE("netfs_validate_stat", node);
  error_t err = EOPNOTSUPP;

  if(fuse_ops->getattr)
    err = -fuse_ops->getattr(node->nn->path, &node->nn_stat);

  FUNC_EPILOGUE(err);
}



/* Read the contents of NODE (a symlink), for USER, into BUF. */
error_t netfs_attempt_readlink (struct iouser *user, struct node *node,
				char *buf)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_readlink", node);
  error_t err = EOPNOTSUPP;

  if(fuse_ops->readlink)
    err = -fuse_ops->readlink(node->nn->path, buf, INT_MAX);

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
  error_t err = EROFS;
  char *path = NULL;

  if(! fuse_ops->mknod)
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
  err = -fuse_ops->mknod(path, (mode & ALLPERMS) | S_IFREG, 0);

  /* If available, call chown to make clear which uid/gid to assign to the
   * new file.  Testing with 'fusexmp' I noticed that new files might be
   * created with wrong gids -- root instead of $user in my case  :(
   *
   * TODO reconsider whether we should setuid/setgid the fuse_ops->mknod
   * call instead (especially if mknod is not available or returns errors)
   */
  if(! err && fuse_ops->chown) {
    assert(user->uids->ids[0]);
    assert(user->gids->ids[0]);

    (void)fuse_ops->chown(path, user->uids->ids[0], user->gids->ids[0]);
  }

 out:
  if(err)
    *node = NULL;
  else
    {
      /* create a new (net-)node for this file */
      struct netnode *nn = fuse_make_netnode(dir->nn, path);

      if(nn)
	*node = fuse_make_node(nn);
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
  error_t err = EROFS;

  if(fuse_ops->chown)
    err = -fuse_ops->chown(node->nn->path, uid, gid);

  FUNC_EPILOGUE(err);
}



/* This should attempt to fetch filesystem status information for the remote
   filesystem, for the user CRED. */
error_t
netfs_attempt_statfs (struct iouser *cred, struct node *node,
		      fsys_statfsbuf_t *st)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_statfs", node);
  error_t err = EOPNOTSUPP;

  if(fuse_ops->statfs)
    err = -fuse_ops->statfs(node->nn->path, st);

  FUNC_EPILOGUE(err);
}



/* Attempt to create a new directory named NAME in DIR for USER with mode
   MODE.  */
error_t netfs_attempt_mkdir (struct iouser *user, struct node *dir,
			     char *name, mode_t mode)
{
  FUNC_PROLOGUE("netfs_attempt_mkdir");
  error_t err = EROFS;
  char *path = NULL;

  if(! fuse_ops->mkdir)
    goto out;

  if(! (path = malloc(strlen(dir->nn->path) + strlen(name) + 2)))
    {
      err = ENOMEM;
      goto out;
    }

  sprintf(path, "%s/%s", dir->nn->path, name);

  err = -fuse_ops->mkdir(path, mode & ALLPERMS);

  /* If available, call chown to make clear which uid/gid to assign to the
   * new file.  Testing with 'fusexmp' I noticed that new files might be
   * created with wrong gids -- root instead of $user in my case  :(
   *
   * TODO reconsider whether we should setuid/setgid the fuse_ops->mknod
   * call instead (especially if mknod is not available or returns errors)
   */
  if(! err && fuse_ops->chown) {
    assert(user->uids->ids[0]);
    assert(user->gids->ids[0]);

    (void)fuse_ops->chown(path, user->uids->ids[0], user->gids->ids[0]);
  }

 out:
  /* we don't need to make a netnode already, lookup will be called and do
   * that for us.
   */
  free(path);
  FUNC_EPILOGUE(err);
}



/* This should attempt a chflags call for the user specified by CRED on node
   NODE, to change the flags to FLAGS. */
error_t netfs_attempt_chflags (struct iouser *cred, struct node *node,
			       int flags)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_chflags", node);
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(EROFS);
}



/* Node NODE is being opened by USER, with FLAGS.  NEWNODE is nonzero if we
   just created this node.  Return an error if we should not permit the open
   to complete because of a permission restriction. */
error_t
netfs_check_open_permissions (struct iouser *user, struct node *node,
			      int flags, int newnode)
{
  FUNC_PROLOGUE_NODE("netfs_check_open_permissions", node);
  error_t err = 0;

  if(fuse_ops->open)
    {
      err = -fuse_ops->open(node->nn->path, flags);

      if(! err && fuse_ops->release)
	(void)fuse_ops->release(node->nn->path, flags);
    }
  else
    {
      /* the fuse translator doesn't bring an open routine with it.
       * try to figure out whether writing should be okay
       */
      if (flags & O_READ)
	err = fshelp_access (&node->nn_stat, S_IREAD, user);
      
      if (!err && (flags & O_WRITE))
	err = fshelp_access (&node->nn_stat, S_IWRITE, user);
      
      if (!err && (flags & O_EXEC))
	err = fshelp_access (&node->nn_stat, S_IEXEC, user);
    }

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
  error_t err = EROFS;

  if(fuse_ops->chmod)
    err = -fuse_ops->chmod(node->nn->path, mode);

  FUNC_EPILOGUE(err);
}



/* Attempt to create an anonymous file related to DIR for USER with MODE.
   Set *NODE to the returned file upon success.  No matter what, unlock DIR. */
error_t netfs_attempt_mkfile (struct iouser *user, struct node *dir,
			      mode_t mode, struct node **node)
{
  FUNC_PROLOGUE("netfs_attempt_mkfile");
  error_t err = EROFS;
  char name[20];
  static int num = 0;

  if(! fuse_ops->mknod)
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

  /* mutex_unlock (&dir->lock);
   * netfs_attempt_create_file already unlocked the node for us.
   */
  FUNC_EPILOGUE(err);
}



/* This should sync the entire remote filesystem.  If WAIT is set, return
   only after sync is completely finished.  */
error_t netfs_attempt_syncfs (struct iouser *cred, int wait)
{
  FUNC_PROLOGUE("netfs_attempt_syncfs");
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(0);
}



/* This should sync the file NODE completely to disk, for the user CRED.  If
   WAIT is set, return only after sync is completely finished.  */
error_t
netfs_attempt_sync (struct iouser *cred, struct node *node, int wait)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_sync", node);
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(0);
}



/* Delete NAME in DIR for USER.
 * The node DIR is locked, and shall stay locked
 */
error_t netfs_attempt_unlink (struct iouser *user, struct node *dir,
			      char *name)
{
  FUNC_PROLOGUE("netfs_attempt_unlink");
  error_t err = EROFS;
  char *path = NULL;

  if(! fuse_ops->unlink)
    return err; /* EROFS */

  if(! (path = malloc(strlen(name) + strlen(dir->nn->path) + 2)))
    {
      err = ENOMEM;
      goto out;
    }

  sprintf(path, "%s/%s", dir->nn->path, name);
  err = -fuse_ops->unlink(path);

  /* TODO free associated netnode. really? */
 out:
  free(path);

  FUNC_EPILOGUE(err);
}



/* This should attempt to set the size of the file NODE (for user CRED) to
   SIZE bytes long. */
error_t netfs_attempt_set_size (struct iouser *cred, struct node *node,
				loff_t size)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_set_size", node);
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(EOPNOTSUPP);
}



/* Attempt to turn NODE (user CRED) into a device.  TYPE is either S_IFBLK or
   S_IFCHR. */
error_t netfs_attempt_mkdev (struct iouser *cred, struct node *node,
			     mode_t type, dev_t indexes)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_mkdev", node);
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(EROFS);
}



/* Return the valid access types (bitwise OR of O_READ, O_WRITE, and O_EXEC)
   in *TYPES for file NODE and user CRED.  */
error_t
netfs_report_access (struct iouser *cred, struct node *node, int *types)
{
  FUNC_PROLOGUE_NODE("netfs_report_access", node);
  *types = 0;

  if (fshelp_access (&node->nn_stat, S_IREAD, cred) == 0)
    *types |= O_READ;
  
  if (fshelp_access (&node->nn_stat, S_IWRITE, cred) == 0)
    *types |= O_WRITE;

  if (fshelp_access (&node->nn_stat, S_IEXEC, cred) == 0)
    *types |= O_EXEC;
  
  FUNC_EPILOGUE(0);
}


/* callback-helper of netfs_attempt_lookup, check whether the name of
 * file (of this callback) is equal to the on we're looking for, in case
 * set 'found' from fuse_dirh_t handle to TRUE.
 */
static int
fuse_lookup_helper(fuse_dirh_t handle, const char *name, int type)
{
  if(! strcmp(name, handle->filename))
    {
      handle->found = 1;
      return ENOMEM; /* send ENOMEM to stop being called, TODO make sure that
		      * all programs, depending on libfuse, can live with that.
		      */
    }

  return 0;
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

  error_t err = ENOENT;

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

  else if(fuse_ops->getdir)
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
	  
      fuse_ops->getdir(dir->nn->path, handle, fuse_lookup_helper);
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
  char *path = NULL;

  if(! fuse_ops->link)
    return err; /* EOPNOTSUPP */

  mutex_lock(&dir->lock);

  if(! (path = malloc(strlen(name) + strlen(dir->nn->path) + 2)))
    {
      err = ENOMEM;
      goto out;
    }

  sprintf(path, "%s/%s", dir->nn->path, name);

  if(! excl && fuse_ops->unlink)
    /* EXCL is not set, therefore we may remove the target, i.e. call
     * unlink on it.  Ignoring return value, as it's mostly not interesting,
     * since the file does not exist in most case
     */
    (void)fuse_ops->unlink(path);

  err = -fuse_ops->link(file->nn->path, path);

  /* If available, call chown to make clear which uid/gid to assign to the
   * new file.  Testing with 'fusexmp' I noticed that new files might be
   * created with wrong gids -- root instead of $user in my case  :(
   *
   * TODO reconsider whether we should setuid/setgid the fuse_ops->mknod
   * call instead (especially if mknod is not available or returns errors)
   */
  /* if(! err && fuse_ops->chown)
   *   {
   *     assert(user->uids->ids[0]);
   *     assert(user->gids->ids[0]);
   *
   *     (void)fuse_ops->chown(path, user->uids->ids[0], user->gids->ids[0]);
   *   }
   */
  /* FIXME
   * This is most probably not a good idea to do here, as it would change
   * the user and group-id of the other (linked) files as well, sharing the
   * same inode.
   */

 out:
  mutex_unlock(&dir->lock);
  free(path);

  FUNC_EPILOGUE(err);
}



/* Attempt to remove directory named NAME in DIR for USER. */
error_t netfs_attempt_rmdir (struct iouser *user,
			     struct node *dir, char *name)
{
  FUNC_PROLOGUE("netfs_attempt_rmdir");
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(EROFS);
}



/* This should attempt a chauthor call for the user specified by CRED on node
   NODE, to change the author to AUTHOR. */
error_t netfs_attempt_chauthor (struct iouser *cred, struct node *node,
				uid_t author)
{
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
  if(! fuse_ops->unlink)
    goto out;

  /* symlink function available? if not, fail. */
  if(! fuse_ops->symlink)
    goto out;

  /* try to remove the existing node (probably an anonymous file) */
  if((err = -fuse_ops->unlink(node->nn->path)))
    goto out;

  err = -fuse_ops->symlink(name, node->nn->path);

  /* we don't have to adjust nodes/netnodes, as these are already existing.
   * netfs_attempt_mkfile did that for us.
   */

 out:
  FUNC_EPILOGUE(err);
}



/* Note that in this one call, neither of the specific nodes are locked. */
error_t netfs_attempt_rename (struct iouser *user, struct node *fromdir,
			      char *fromname, struct node *todir,
			      char *toname, int excl)
{
  FUNC_PROLOGUE("netfs_attempt_rename");
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(EROFS);
}



/* Write to the file NODE for user CRED starting at OFSET and continuing for up
   to *LEN bytes from DATA.  Set *LEN to the amount seccessfully written upon
   return. */
error_t netfs_attempt_write (struct iouser *cred, struct node *node,
			     loff_t offset, size_t *len, void *data)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_write", node);
  NOT_IMPLEMENTED();
  FUNC_EPILOGUE(EROFS);
}



/* This should attempt a utimes call for the user specified by CRED on node
   NODE, to change the atime to ATIME and the mtime to MTIME. */
error_t
netfs_attempt_utimes (struct iouser *cred, struct node *node,
		      struct timespec *atime, struct timespec *mtime)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_utimes", node);
  error_t err = fshelp_isowner (&node->nn_stat, cred);
  int flags = TOUCH_CTIME;

  NOT_IMPLEMENTED();
  
  if (! err)
    {
      if (mtime)
	{
	  node->nn_stat.st_mtime = mtime->tv_sec;
	  node->nn_stat.st_mtime_usec = mtime->tv_nsec / 1000;
	}
      else
	flags |= TOUCH_MTIME;
      
      if (atime)
	{
	  node->nn_stat.st_atime = atime->tv_sec;
	  node->nn_stat.st_atime_usec = atime->tv_nsec / 1000;
	}
      else
	flags |= TOUCH_ATIME;
      
      /* fshelp_touch (&node->nn_stat, flags, cvsfs_maptime); */
    }

  FUNC_EPILOGUE(err);
}



/* Read from the file NODE for user CRED starting at OFFSET and continuing for
   up to *LEN bytes.  Put the data at DATA.  Set *LEN to the amount
   successfully read upon return.  */
error_t netfs_attempt_read (struct iouser *cred, struct node *node,
			    loff_t offset, size_t *len, void *data)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_read", node);
  size_t sz;

  if(! fuse_ops->read)
    FUNC_RETURN(EOPNOTSUPP);

  sz = fuse_ops->read(node->nn->path, data, *len, offset);

  if(sz < 0)
    FUNC_RETURN(sz);

  *len = sz;
  FUNC_EPILOGUE(0);
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
  if(handle->first_entry)
    {
      /* skip this entry, it's before the first one we got to write out ... */
      handle->first_entry --;
      return 0;
    }

  if(handle->num_entries || handle->count < handle->num_entries)
    {
      size_t name_len = strlen(name);
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
 * to the mmaped memmroy
 */
static int
fuse_dirent_helper(fuse_dirh_t handle, const char *name, int type)
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

  if(! fuse_ops->getdir)
    FUNC_RETURN(EOPNOTSUPP);

  if(! (handle = malloc(sizeof(struct fuse_dirhandle))))
    FUNC_RETURN(ENOMEM); /* sorry, translator not available ... */

  handle->first_entry = first_entry;
  handle->num_entries = num_entries;
  handle->count = 0;
  handle->max_data_len = max_data_len;
  handle->size = 0;

  err = fuse_ops->getdir(dir->nn->path, handle, fuse_bump_helper);
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
      fuse_ops->getdir(dir->nn->path, handle, fuse_dirent_helper);      
    }

  /* TODO: fshelp_touch ATIME here */
  FUNC_EPILOGUE(err);
}



/* Node NP is all done; free all its associated storage. */
void
netfs_node_norefs (struct node *node)
{
  FUNC_PROLOGUE_NODE("netfs_node_norefs", node);

  DEBUG("netnode-lock", "locking netnode, path=%s\n", node->nn->path);
  mutex_lock(&node->nn->lock);

  if(node->nn->anonymous && fuse_ops->unlink)
    {
      /* FIXME, need to lock parent directory structure */
      fuse_ops->unlink(node->nn->path);

      /* FIXME, free associated netnode somehow. */
    }

  node->nn->node = NULL;

  mutex_unlock(&node->nn->lock);
  DEBUG("netnode-lock", "netnode unlocked.\n"); /* no ref to node->nn av. */

  FUNC_EPILOGUE_NORET();
}
