/**********************************************************
 * netfs.c
 *
 * Copyright(C) 2004,2005,2006 by Stefan Siegl <stesie@brokenpipe.de>, Germany
 * 
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Publice License,
 * version 2 or any later. The license is contained in the COPYING
 * file that comes with the libfuse distribution.
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
  int first_entry;         /* index of first entry to return (counted down
			    * in helper function) */
  int num_entries;         /* number of further entries we may write out to the
			    * buffer, counted down in callback function*/
  int count;               /* number of elements, we already wrote to buffer */
  size_t size;             /* number of further bytes we may write to buffer */
  struct dirent *hdrpos;   /* where to write next dirent structure */
  size_t maxlen;           /* (allocated) length of filename field, def 256 */
  struct netnode *parent;  /* netnode of the dir which's content we list */

  char *abspath;
  char *filename;
};


static inline void
refresh_context_struct(struct iouser *cred)
{
  FUNC_PROLOGUE("refresh_context_struct");
  struct fuse_context *ctx = libfuse_ctx;
  
  if(! ctx) 
    {
      ctx = malloc(sizeof(struct fuse_context));
      if(! ctx) 
	{
	  perror(PACKAGE_NAME);
	  return;
	}

      libfuse_ctx = ctx;

      ctx->fuse = libfuse_fuse;
      ctx->private_data = libfuse_fuse->private_data;
      
      /* FIXME, how to figure out the pid of the program asking for the
       * filesystem operation? */
      ctx->pid = 0;
    }

  if(cred)
    {
      ctx->uid = cred->uids->num ? cred->uids->ids[0] : 
	(libfuse_params.force_uid ? libfuse_params.uid : geteuid());
      ctx->gid = cred->gids->num ? cred->gids->ids[0] : 
	(libfuse_params.force_gid ? libfuse_params.gid : getegid());
    }
  else
    {
      ctx->uid = libfuse_params.force_uid ? libfuse_params.uid : geteuid();
      ctx->gid = libfuse_params.force_gid ? libfuse_params.gid : getegid();
    }      

  FUNC_EPILOGUE_NORET();
}

/* Check whether to allow access to a node, testing the allow_root and
 * allow_other flag.  This does not check whether default permissions
 * are okay to allow access.
 *
 * Return: 0 if access is to be granted, EPERM otherwise
 *
 * Sidenote: This function is mainly called from netfs_validate_stat which in
 *           turn is called by any other of the netfs-operation-functions. This
 *           is, we don't have to call this from all of the operation-functions
 *           since netfs_validate_stat is called anyways!
 */
static error_t
test_allow_root_or_other (struct iouser *cred)
{
  FUNC_PROLOGUE("test_allow_root_or_other");

  assert(cred);

  /* if allow_other is set, access is okay in any case */
  if(libfuse_params.allow_other) 
    FUNC_RETURN_FMT(0, "allow_other is set");

  unsigned int i;
  uid_t proc_uid = getuid();
  for(i = 0; i < cred->uids->num; i ++) 
    {
      DEBUG("test_allow", "testing for uid=%d\n", cred->uids->ids[i]);

      if(cred->uids->ids[i] == 0 && libfuse_params.allow_root)
	FUNC_RETURN_FMT(0, "allowing access for root");

      if(cred->uids->ids[i] == proc_uid)
	FUNC_RETURN_FMT(0, "allowing access for owner");
    }

  /* iouser is not the "filesystem-owner" and allow_other is not set,
   * or iouser is root but allow_root is not set either */
  FUNC_EPILOGUE(EPERM);
}



/* Make sure that NP->nn_stat is filled with current information.  CRED
   identifies the user responsible for the operation. 

   If the user `CRED' is not allowed to perform any operation (considering
   the allow_root and allow_other flags), return EPERM. */
error_t
netfs_validate_stat (struct node *node, struct iouser *cred)
{
  FUNC_PROLOGUE_NODE("netfs_validate_stat", node);
  error_t err = EOPNOTSUPP;

  if(test_allow_root_or_other(cred))
    FUNC_RETURN(EPERM);

  if(FUSE_OP_HAVE(getattr))
    {
      refresh_context_struct(cred);
      err = -FUSE_OP_CALL(getattr, node->nn->path, &node->nn_stat);
    }

  if(! err)
    {
      if(! libfuse_params.use_ino)
	node->nn_stat.st_ino = node->nn->inode;

      node->nn_stat.st_dev = getpid();
      node->nn_stat.st_blksize = 1 << 12; /* there's probably no sane default,
					   * use 4 kB for the moment */

      if(libfuse_params.force_uid)
	node->nn_stat.st_uid = libfuse_params.uid;

      if(libfuse_params.force_gid)
	node->nn_stat.st_gid = libfuse_params.gid;

      if(libfuse_params.force_umask)
	node->nn_stat.st_mode &= ~libfuse_params.umask;
    }

  FUNC_EPILOGUE(err);
}



/* Read the contents of NODE (a symlink), for USER, into BUF. */
error_t netfs_attempt_readlink (struct iouser *user, struct node *node,
				char *buf)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_readlink", node);
  error_t err;

  if((err = netfs_validate_stat(node, user))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_access(&node->nn_stat, S_IREAD, user))))
    goto out;

  if(FUSE_OP_HAVE(readlink))
    err = -FUSE_OP_CALL(readlink, node->nn->path, buf, node->nn_stat.st_size + 1);
  else
    err = EOPNOTSUPP;

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
  error_t err;
  char *path = NULL;

  if((err = netfs_validate_stat(dir, user))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_checkdirmod(&dir->nn_stat, NULL, user))))
    goto out;

  if(! FUSE_OP_HAVE(mknod))
    {
      err = EOPNOTSUPP;
      goto out;
    }

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
    pthread_mutex_lock(&(*node)->lock);

  pthread_mutex_unlock (&dir->lock);

  FUNC_EPILOGUE(err);
}



/* This should attempt a chmod call for the user specified by CRED on node
   NODE, to change the owner to UID and the group to GID. */
error_t netfs_attempt_chown (struct iouser *cred, struct node *node,
			     uid_t uid, uid_t gid)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_chown", node);
  error_t err;

  if((err = netfs_validate_stat(node, cred))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_isowner(&node->nn_stat, cred))))
    goto out;

  if(! FUSE_OP_HAVE(chown))
    {
      err = EOPNOTSUPP;
      goto out;
    }

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
  FUNC_PROLOGUE_NODE("netfs_attempt_statfs", node);
  error_t err;

  if(test_allow_root_or_other(cred))
    err = EPERM;

  else if(FUSE_OP_HAVE(statfs))
    {
      struct statvfs stvfs;

      refresh_context_struct(cred);
      err = -FUSE_OP_CALL(statfs, node->nn->path, &stvfs);

      if(! err)
	{
	  st->f_type = stvfs.__f_type;
	  st->f_bsize = stvfs.f_bsize;
	  st->f_blocks = stvfs.f_blocks;
	  st->f_bfree = stvfs.f_bfree;
	  st->f_bavail = stvfs.f_bavail;
	  st->f_files = stvfs.f_files;
	  st->f_ffree = stvfs.f_ffree;
	  st->f_fsid = stvfs.f_fsid;
	  st->f_namelen = stvfs.f_namemax;
	  st->f_favail = stvfs.f_favail;
	  st->f_frsize = stvfs.f_frsize;
	  st->f_flag = stvfs.f_flag;
	}
    }

  else
    err = EOPNOTSUPP;

  FUNC_EPILOGUE(err);
}



/* Attempt to create a new directory named NAME in DIR for USER with mode
   MODE.  */
error_t netfs_attempt_mkdir (struct iouser *user, struct node *dir,
			     char *name, mode_t mode)
{
  FUNC_PROLOGUE("netfs_attempt_mkdir");
  error_t err;
  char *path = NULL;

  if((err = netfs_validate_stat(dir, user))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_checkdirmod(&dir->nn_stat, NULL, user))))
    goto out;

  if(! FUSE_OP_HAVE(mkdir))
    {
      err = EOPNOTSUPP;
      goto out;
    }

  if(! (path = malloc(strlen(dir->nn->path) + strlen(name) + 2)))
    {
      err = ENOMEM;
      goto out;
    }

  sprintf(path, "%s/%s", dir->nn->path, name);

  err = -FUSE_OP_CALL(mkdir, path, mode & ALLPERMS);

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

  FUNC_PROLOGUE_FMT("netfs_check_open_permissions", "node=%s, flags=%d", 
		    node->nn->path, flags);
  error_t err = 0;

  if((err = netfs_validate_stat(node, user)))
    goto out;

  if(libfuse_params.deflt_perms)
    {
      if (flags & O_READ)
	err = fshelp_access (&node->nn_stat, S_IREAD, user);
      
      if (!err && (flags & O_WRITE))
	err = fshelp_access (&node->nn_stat, S_IWRITE, user);
      
      if (!err && (flags & O_EXEC))
	err = fshelp_access (&node->nn_stat, S_IEXEC, user);
    }

  /* store provided flags for later open/read/write/release operation call.
   *
   * If O_EXEC is set, make sure O_RDONLY is set, this is, if you want to 
   * execute a binary, only O_EXEC is set, but we want to read the binary
   * into memory. */
  if(! err) 
    {
      NN_INFO_APPLY(node, flags = flags);
      if(flags & O_EXEC) NN_INFO_APPLY(node, flags |= O_RDONLY);
    }

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
  error_t err;

  if((err = netfs_validate_stat(node, cred))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_isowner(&node->nn_stat, cred))))
    goto out;

  if(! FUSE_OP_HAVE(chmod))
    {
      err = EOPNOTSUPP;
      goto out;
    }

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
  error_t err;
  char name[20];
  static int num = 0;

  if(! (err = netfs_validate_stat(dir, user))
     && libfuse_params.deflt_perms)
    err = fshelp_checkdirmod(&dir->nn_stat, NULL, user);

  if(! err && ! FUSE_OP_HAVE(mknod))
    err = EOPNOTSUPP;

  if(err)
    {
      /* dir has to be unlocked no matter what ... */
      pthread_mutex_unlock (&dir->lock);
      goto out;
    }

  /* call netfs_attempt_create_file with O_EXCL and O_CREAT bits set */
  mode |= O_EXCL | O_CREAT;

  do 
    {
      snprintf(name, sizeof(name), ".libfuse-%06d", num ++);
      err = netfs_attempt_create_file(user, dir, name, mode, node);

      if(err == EEXIST)
	pthread_mutex_lock(&dir->lock); /* netfs_attempt_create_file just unlocked
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

  /* pthread_mutex_unlock (&dir->lock);
   * netfs_attempt_create_file already unlocked the node for us.
   */
  FUNC_EPILOGUE(err);
}



/* This should sync the entire remote filesystem.  If WAIT is set, return
   only after sync is completely finished.  */
error_t netfs_attempt_syncfs (struct iouser *cred, int wait)
{
  (void) wait;   /* there's no such flag in libfuse */

  FUNC_PROLOGUE("netfs_attempt_syncfs");
  error_t err;

  if(cred && test_allow_root_or_other(cred))
    err = EPERM;
  else
    {
      refresh_context_struct(cred);
      err = fuse_sync_filesystem();
    }

  FUNC_EPILOGUE(err);
}



/* This should sync the file NODE completely to disk, for the user CRED.  If
   WAIT is set, return only after sync is completely finished.  */
error_t
netfs_attempt_sync (struct iouser *cred, struct node *node, int wait)
{
  (void) wait;   /* there's no such flag in libfuse */

  FUNC_PROLOGUE_NODE("netfs_attempt_sync", node);
  error_t err;

  if((err = netfs_validate_stat(node, cred))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_access(&node->nn_stat, S_IWRITE, cred))))
    goto out;

  if(! FUSE_OP_HAVE(fsync))
    {
      err = EOPNOTSUPP;
      goto out;
    }
  
  err = -FUSE_OP_CALL(fsync, node->nn->path, 0, NN_INFO(node));

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
  error_t err;
  struct node *node = NULL;

  err = netfs_attempt_lookup(user, dir, name, &node);
  assert(dir != node);
  pthread_mutex_lock(&dir->lock); /* re-lock directory, since netfs_attempt_lookup
			   * unlocked it for us
			   */  
  if(err)
    goto out;

  pthread_mutex_unlock(&node->lock);

  if((err = netfs_validate_stat(dir, user))
     || (err = netfs_validate_stat(node, user))
     || (libfuse_params.deflt_perms 
	 && (err = fshelp_checkdirmod(&dir->nn_stat, &node->nn_stat, user))))
    goto out;

  if(FUSE_OP_HAVE(unlink))
    err = -FUSE_OP_CALL(unlink, node->nn->path);
  else
    err = EOPNOTSUPP;

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
  error_t err;

  if((err = netfs_validate_stat(node, cred))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_access(&node->nn_stat, S_IWRITE, cred))))
    goto out;

  if(FUSE_OP_HAVE(truncate))
    err = -FUSE_OP_CALL(truncate, node->nn->path, size);
  else
    err = EOPNOTSUPP;

 out:
  FUNC_EPILOGUE(err);
}



/* Attempt to turn NODE (user CRED) into a device.  TYPE is either S_IFBLK or
   S_IFCHR. */
error_t netfs_attempt_mkdev (struct iouser *cred, struct node *node,
			     mode_t type, dev_t indexes)
{
  FUNC_PROLOGUE_NODE("netfs_attempt_mkdev", node);
  error_t err;

  /* check permissions
   * XXX, shall we check permissions of the parent directory as well,
   * since we're going to unlink files?
   */
  if((err = netfs_validate_stat(node, cred))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_access(&node->nn_stat, S_IWRITE, cred))))
    goto out;

  /* check whether the operations are available at all */
  if(! FUSE_OP_HAVE(mknod))
    {
      err = EOPNOTSUPP;
      goto out;
    }

  /* we need to unlink the existing node, therefore, if unlink is not
   * available, we cannot turn *node into a device.
   */
  if(! FUSE_OP_HAVE(unlink))
    {
      err = EOPNOTSUPP;
      goto out;
    }

  /* unlink the already existing node, to be able to create the (new)
   * device file
   */
  if((err = -FUSE_OP_CALL(unlink, node->nn->path)))
    goto out;

  err = -FUSE_OP_CALL(mknod, node->nn->path,
		      type & (ALLPERMS | S_IFBLK | S_IFCHR), indexes);

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
  
  if(libfuse_params.deflt_perms) 
    {
      if(fshelp_access (&node->nn_stat, S_IREAD, cred) == 0)
	*types |= O_READ;
      
      if(fshelp_access (&node->nn_stat, S_IWRITE, cred) == 0)
	*types |= O_WRITE;

      if(fshelp_access (&node->nn_stat, S_IEXEC, cred) == 0)
	*types |= O_EXEC;
    }
  else
    /* check_allow_root_or_other allowed to get here, this is, we're
     * allowed to access the file. Default permissions checking is disabled,
     * thus, grant access
     */
    *types |= O_READ | O_WRITE | O_EXEC;

 out:
  FUNC_EPILOGUE(0);
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
     || (libfuse_params.deflt_perms
	 && ((err = fshelp_access(&dir->nn_stat, S_IREAD, user))
	     || (err = fshelp_access(&dir->nn_stat, S_IEXEC, user)))))
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

  else if(FUSE_OP_HAVE(getattr))
    {
      /* lookup for common file */
      struct netnode *nn;
      char *path;
      struct stat stbuf;

      if(asprintf(&path, "%s/%s",
		  dir->nn->parent ? dir->nn->path : "", name) < 0)
	{
	  err = ENOMEM;
	  goto out;
	}

      if(! (err = -FUSE_OP_CALL(getattr, path, &stbuf)))
	if(! (nn = fuse_make_netnode(dir->nn, path)) ||
	   ! (*node = fuse_make_node(nn)))
	  err = ENOMEM;

      free(path); /* fuse_make_netnode strdup()s the pathname */
    }

out:
  pthread_mutex_unlock(&dir->lock);

  if(err)
    *node = NULL;
  else
    pthread_mutex_lock(&(*node)->lock);

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
  error_t err;
  struct node *node = NULL;

  pthread_mutex_lock(&dir->lock);

  if((err = netfs_attempt_lookup(user, dir, name, &node)))
    goto out_nounlock; /* netfs_attempt_lookup unlocked dir */

  assert(dir != node);
  pthread_mutex_lock(&dir->lock);

  if((err = netfs_validate_stat(node, user))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_checkdirmod(&dir->nn_stat, &node->nn_stat, user))))
    goto out;

  if(! FUSE_OP_HAVE(link)) {
    err = EOPNOTSUPP;
    goto out;
  }

  if(! excl && FUSE_OP_HAVE(unlink))
    /* EXCL is not set, therefore we may remove the target, i.e. call
     * unlink on it.  Ignoring return value, as it's mostly not interesting,
     * since the file does not exist in most case
     */
    (void) FUSE_OP_CALL(unlink, node->nn->path);

  err = -FUSE_OP_CALL(link, file->nn->path, node->nn->path);

  /* TODO
   * create a netnode with the may_need_sync flag set!!   */

 out:
  pthread_mutex_unlock(&dir->lock);

  pthread_mutex_unlock(&node->lock);
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
  error_t err;
  struct node *node = NULL;

  err = netfs_attempt_lookup(user, dir, name, &node);
  assert(dir != node);
  pthread_mutex_lock(&dir->lock); /* netfs_attempt_lookup unlocked dir */

  if(err)
    goto out_nounlock; 

  if((err = netfs_validate_stat(node, user))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_checkdirmod(&dir->nn_stat, &node->nn_stat, user))))
    goto out;

  if(FUSE_OP_HAVE(rmdir))
    err = -FUSE_OP_CALL(rmdir, node->nn->path);
  else
    err = EOPNOTSUPP;


  /* TODO free associated netnode. really? 
   * FIXME, make sure nn->may_need_sync is set */

 out:
  pthread_mutex_unlock(&node->lock);
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
  error_t err;

  /* check permissions
   * XXX, shall we check permissions of the parent directory as well,
   * since we're going to unlink files?
   */
  if((err = netfs_validate_stat(node, cred))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_access(&node->nn_stat, S_IWRITE, cred))))
    goto out;

  /* we need to unlink the existing node, therefore, if unlink is not
   * available, we cannot create symlinks
   */
  if(! FUSE_OP_HAVE(unlink))
    { 
      err = EOPNOTSUPP;
      goto out;
    }

  /* symlink function available? if not, fail. */
  if(! FUSE_OP_HAVE(symlink))
    {
      err = EOPNOTSUPP;
      goto out;
    }

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
  error_t err;
  struct node *fromnode;
  char *topath = NULL;

  if(! (topath = malloc(strlen(toname) + strlen(todir->nn->path) + 2)))
    {
      err = ENOMEM;
      goto out_nounlock;
    }

  pthread_mutex_lock(&fromdir->lock);

  if(netfs_attempt_lookup(user, fromdir, fromname, &fromnode))
    {
      err = ENOENT;
      goto out_nounlock; /* netfs_attempt_lookup unlocked fromdir and locked
			  * fromnode for us. 
			  */
    }

  pthread_mutex_lock(&todir->lock);

  if((err = netfs_validate_stat(fromdir, user))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_checkdirmod(&fromdir->nn_stat,
				      &fromnode->nn_stat, user))))
    goto out;

  if((err = netfs_validate_stat(todir, user))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_checkdirmod(&todir->nn_stat, NULL, user))))
    goto out;

  if(! FUSE_OP_HAVE(rename))
    {
      err = EOPNOTSUPP;
      goto out; 
    }

  sprintf(topath, "%s/%s", todir->nn->path, toname);

  if(! excl && FUSE_OP_HAVE(unlink))
    /* EXCL is not set, therefore we may remove the target, i.e. call
     * unlink on it.  Ignoring return value, as it's mostly not interesting,
     * since the file does not exist in most cases
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
  pthread_mutex_unlock(&todir->lock);

  pthread_mutex_unlock(&fromnode->lock);
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
  error_t err;

  if((err = netfs_validate_stat(node, cred))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_access(&node->nn_stat, S_IWRITE, cred))))
    goto out;

  if(! FUSE_OP_HAVE(write)) 
    {
      err = EOPNOTSUPP;
      goto out;
    }

  NN_INFO_APPLY(node, writepage = 0); /* cannot distinct on the Hurd :( */

  if(FUSE_OP_HAVE(open))
    {
      err = FUSE_OP_CALL(open, node->nn->path, NN_INFO(node));

      if(err) goto out;
    }

  int sz = FUSE_OP_CALL(write, node->nn->path, data, *len, offset,
			NN_INFO(node));

  /* FIXME: open, flush and release handling probably should be changed
   * completely, I mean, we probably should do fuse_ops->open in 
   * the netfs_check_open_permissions function and leave it open
   * until the node is destroyed.
   *
   * This way we wouldn't be able to report any errors back.
   */
  if(sz >= 0 && FUSE_OP_HAVE(flush))
    err = FUSE_OP_CALL(flush, node->nn->path, NN_INFO(node));

  if(FUSE_OP_HAVE(open) && FUSE_OP_HAVE(release))
    FUSE_OP_CALL(release, node->nn->path, NN_INFO(node));
  
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
  error_t err;

  /* test whether operation is supported and permission are sufficient */
  if((err = netfs_validate_stat(node, cred))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_isowner(&node->nn_stat, cred))))
    goto out;

  if(FUSE_OP_HAVE(utimens))
    {
      /* Prepare TS for FUSE_OP_HAVE(utimens) call.  */
      struct timespec ts[2];
      ts[0] = atime ? *atime : node->nn_stat.st_atim;
      ts[1] = mtime ? *mtime : node->nn_stat.st_mtim;

      err = -FUSE_OP_CALL(utimens, node->nn->path, ts);

      if (!err)
	{
	  node->nn_stat.st_mtim = ts[1];

	  node->nn_stat.st_atim = ts[0];

	  node->nn->may_need_sync = 1;
	}
    }
  else if(FUSE_OP_HAVE(utime))
    {
      /* Prepare UTB for FUSE_OP_HAVE(utime) call.  */
      struct utimbuf utb;
      utb.actime = atime ? atime->tv_sec : node->nn_stat.st_atime;
      utb.modtime = mtime ? mtime->tv_sec : node->nn_stat.st_mtime;

      err = -FUSE_OP_CALL(utime, node->nn->path, &utb);

      if (!err)
	{
	  node->nn_stat.st_mtim.tv_sec = utb.modtime;
	  node->nn_stat.st_mtim.tv_nsec = 0;

	  node->nn_stat.st_atim.tv_sec = utb.actime;
	  node->nn_stat.st_atim.tv_nsec = 0;

	  node->nn->may_need_sync = 1;
	}
    }
  else
    err = EOPNOTSUPP;

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
  error_t err;

  if((err = netfs_validate_stat(node, cred))
     || (libfuse_params.deflt_perms
	 && (err = fshelp_access(&node->nn_stat, S_IREAD, cred))))
    goto out;

  if(! FUSE_OP_HAVE(read))
    {
      err = EOPNOTSUPP;
      goto out;
    }

  if(FUSE_OP_HAVE(open))
    {
      err = FUSE_OP_CALL(open, node->nn->path, NN_INFO(node));

      if(err) goto out;
    }

  int sz = FUSE_OP_CALL(read, node->nn->path, data, *len, offset,
			NN_INFO(node));

  /* FIXME: open, flush and release handling probably should be changed
   * completely, I mean, we probably should do fuse_ops->open in 
   * the netfs_check_open_permissions function and leave it open
   * until the node is destroyed.
   *
   * This way we wouldn't be able to report any errors back.
   */
  if(sz >= 0 && FUSE_OP_HAVE(flush))
    err = FUSE_OP_CALL(flush, node->nn->path, NN_INFO(node));

  if(FUSE_OP_HAVE(open) && FUSE_OP_HAVE(release))
    FUSE_OP_CALL(release, node->nn->path, NN_INFO(node));

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

/* fuse_get_inode
 *
 * look up the inode of the named file (full path) in fuse_use_ino mode,
 * where we don't have the inode number of parent directories in our structs
 */
static ino_t 
fuse_get_inode(const char *name)
{
  struct stat stat;
  
  assert(FUSE_OP_HAVE(getattr));
  FUSE_OP_CALL(getattr, name, &stat);

  return stat.st_ino;
}

/* callback handler used by netfs_get_dirents to write our dirents
 * to the mmaped memory (getdir case)
 */
static int
get_dirents_getdir_helper(fuse_dirh_t handle, const char *name,
			  int type, ino_t ino)
{
  ino_t inode;

  if(handle->first_entry)
    {
      /* skip this entry, it's before the first one we got to write out ... */
      handle->first_entry --;
      return 0;
    }

  if(! (handle->num_entries --))
    return ENOMEM;

  size_t name_len = strlen(name);
  size_t dirent_len = DIRENT_LEN(name_len);

  if(dirent_len > handle->size)
    {
      handle->num_entries = 0; /* don't write any further data, e.g. in case */
      return ENOMEM;           /* next filename's shorter */
    }
  else
    handle->size -= dirent_len;

  /* look the inode of this element up ... */
  if(libfuse_params.use_ino)
    {
      /* using the inode based api */
      if(! strcmp(name, "."))
	inode = fuse_get_inode(handle->parent->path);

      else if(handle->parent->parent && ! strcmp(name, ".."))
	inode = fuse_get_inode(handle->parent->parent->path);

      else
	inode = ino;
    }
  else
    {
      /* using the old (lookup) method ... */

      if(! strcmp(name, "."))
	inode = handle->parent->inode;

      else if(handle->parent->parent && ! strcmp(name, ".."))
	inode = handle->parent->parent->inode;

      else
	{
	  if(name_len > handle->maxlen)
	    {
	      /* allocated field in handle structure is to small, enlarge it */
	      handle->maxlen = name_len << 1;
	      void *a = realloc(handle->abspath, handle->maxlen +
				(handle->filename - handle->abspath) + 1);
	      if(! a)
		return ENOMEM;

	      handle->filename = a + (handle->filename - handle->abspath);
	      handle->abspath = a;
	    }

	  strcpy(handle->filename, name);
	  struct netnode *nn = fuse_make_netnode(handle->parent,
						 handle->abspath);

	  if(! nn)
	    return ENOMEM;

	  inode = nn->inode;
	}
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

  handle->count ++;
  return 0;
}


static error_t
get_dirents_getdir(struct node *dir, int first_entry, int num_entries, 
		   char **data, mach_msg_type_number_t *data_len,
		   int *data_entries)
{
  FUNC_PROLOGUE_NODE("get_dirents_getdir", dir);

  if(! FUSE_OP_HAVE(getdir))
    FUNC_RETURN(EOPNOTSUPP);

  fuse_dirh_t handle;
  if(! (handle = malloc(sizeof(struct fuse_dirhandle))))
    FUNC_RETURN(ENOMEM);
  
  handle->first_entry = first_entry;
  handle->num_entries = num_entries;
  handle->count = 0;
  handle->size = *data_len;

  /* allocate handle->abspath */
  size_t path_len = strlen(dir->nn->path);
  if(! (handle->abspath = malloc((handle->maxlen = 256) + path_len + 2)))
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
  handle->hdrpos = (struct dirent*) *data;

  FUSE_OP_CALL(getdir, dir->nn->path, handle, get_dirents_getdir_helper);

  *data_len -= handle->size; /* subtract number of bytes left in the
			      * buffer from the length of the buffer we
			      * got. */
  *data_entries = handle->count;

  free(handle->abspath);
  free(handle);

  FUNC_EPILOGUE(0);
}


/* callback handler used by netfs_get_dirents to write our dirents
 * to the mmaped memory (in readdir case)
 *
 * be careful, according to fuse.h `stat' may be NULL!
 */
static int
get_dirents_readdir_helper(void *buf, const char *name,
			   const struct stat *stat, off_t off)
{
  fuse_dirh_t handle = buf;
  ino_t inode;

  if(handle->first_entry && !off)
    {
      /* skip this entry, it's before the first one 
       * we got to write out ... */
      handle->first_entry --;
      return 0;
    }

  if(! (handle->num_entries --))
    return 1;

  size_t name_len = strlen(name);
  size_t dirent_len = DIRENT_LEN(name_len);

  if(dirent_len > handle->size)
    {
      handle->num_entries = 0; /* don't write any further data, e.g. in case */
      return ENOMEM;           /* next filename's shorter */
    }
  else
    handle->size -= dirent_len;

  /* look the inode of this element up ... */
  if(libfuse_params.use_ino)
    {
      /* using the inode based api */
      if(! strcmp(name, "."))
	inode = fuse_get_inode(handle->parent->path);

      else if(handle->parent->parent && ! strcmp(name, ".."))
	inode = fuse_get_inode(handle->parent->parent->path);

      if(! stat)
	{
	  DEBUG("critical", "use_ino flag set, but stat ptr not available.\n");
	  inode = 0;
	}

      else
	inode = stat->st_ino;
    }
  else
    {
      /* using the old (lookup) method ... */

      if(! strcmp(name, "."))
	inode = handle->parent->inode;

      else if(handle->parent->parent && ! strcmp(name, ".."))
	inode = handle->parent->parent->inode;

      else
	{
	  if(name_len > handle->maxlen)
	    {
	      /* allocated field in handle structure is to small, enlarge it */
	      handle->maxlen = name_len << 1;
	      void *a = realloc(handle->abspath, handle->maxlen +
				(handle->filename - handle->abspath) + 1);
	      if(! a)
		return ENOMEM;

	      handle->filename = a + (handle->filename - handle->abspath);
	      handle->abspath = a;
	    }

	  strcpy(handle->filename, name);
	  struct netnode *nn = fuse_make_netnode(handle->parent,
						 handle->abspath);

	  if(! nn) 
	    return ENOMEM;

	  inode = nn->inode;
	}
    }

  /* write out struct dirent ... */
  handle->hdrpos->d_fileno = inode;
  handle->hdrpos->d_reclen = dirent_len;
  handle->hdrpos->d_type = stat ? (stat->st_mode >> 12) : 0;
  handle->hdrpos->d_namlen = name_len;

  /* copy file's name ... */
  memcpy(((void *) handle->hdrpos) + DIRENT_NAME_OFFS, name, name_len + 1);

  /* update hdrpos pointer */
  handle->hdrpos = ((void *) handle->hdrpos) + dirent_len;

  handle->count ++;
  return 0;
}


static error_t
get_dirents_readdir(struct node *dir, int first_entry, int num_entries, 
		    char **data, mach_msg_type_number_t *data_len,
		    int *data_entries)
{
  error_t err;
  FUNC_PROLOGUE_NODE("get_dirents_readdir", dir);

  assert(FUSE_OP_HAVE(readdir));

  fuse_dirh_t handle;
  if(! (handle = malloc(sizeof(struct fuse_dirhandle))))
    FUNC_RETURN(ENOMEM);
  
  handle->first_entry = first_entry;
  handle->num_entries = num_entries;
  handle->count = 0;
  handle->size = *data_len;

  /* allocate handle->abspath */
  size_t path_len = strlen(dir->nn->path);
  if(! (handle->abspath = malloc((handle->maxlen = 256) + path_len + 2)))
    {
      err = ENOMEM;
      goto out;
    }

  memcpy(handle->abspath, dir->nn->path, path_len);
  handle->filename = handle->abspath + path_len;

  /* add a delimiting slash if there are parent directories */
  if(dir->nn->parent)
    *(handle->filename ++) = '/';

  handle->parent = dir->nn;
  handle->hdrpos = (struct dirent*) *data;

  if(FUSE_OP_HAVE(opendir)
     && (err = FUSE_OP_CALL(opendir, dir->nn->path, NN_INFO(dir))))
    goto out;

  if((err = FUSE_OP_CALL(readdir, dir->nn->path, handle, 
			 get_dirents_readdir_helper,
			 first_entry, NN_INFO(dir))))
    {
      if(FUSE_OP_HAVE(releasedir))
	FUSE_OP_CALL(releasedir, dir->nn->path, NN_INFO(dir));
      goto out;
    }

  if(FUSE_OP_HAVE(releasedir)
     && (err = FUSE_OP_CALL(releasedir, dir->nn->path, NN_INFO(dir))))
    goto out;

  *data_len -= handle->size; /* subtract number of bytes left in the
			      * buffer from the length of the buffer we
			      * got. */
  *data_entries = handle->count;

 out:
  free(handle->abspath);
  free(handle);

  FUNC_EPILOGUE(err);
}

error_t
netfs_get_dirents (struct iouser *cred, struct node *dir,
		   int first_entry, int num_entries, char **data,
		   mach_msg_type_number_t *data_len,
		   vm_size_t max_data_len, int *data_entries)
{
  (void) max_data_len; /* we live with the supplied mmap area in any case,
			* i.e. never allocate any further memory */

  FUNC_PROLOGUE_NODE("netfs_get_dirents", dir);
  error_t err;

  if((err = netfs_validate_stat(dir, cred))
     || (libfuse_params.deflt_perms
	 && ((err = fshelp_access(&dir->nn_stat, S_IREAD, cred))
	     || (err = fshelp_access(&dir->nn_stat, S_IEXEC, cred)))))
    goto out;


  if(FUSE_OP_HAVE(readdir))
    err = get_dirents_readdir(dir, first_entry, num_entries, data, data_len,
			      data_entries);

  else if(FUSE_OP_HAVE(getdir))
    err = get_dirents_getdir(dir, first_entry, num_entries, data, data_len,
			     data_entries);

  else
    err = EOPNOTSUPP;

  /* TODO: fshelp_touch ATIME here */

 out:
  FUNC_EPILOGUE_FMT(err, "%d entries.", *data_entries);
}



/* Node NP is all done; free all its associated storage. */
void
netfs_node_norefs (struct node *node)
{
  FUNC_PROLOGUE_NODE("netfs_node_norefs", node);
  assert(node);

  DEBUG("netnode-lock", "locking netnode, path=%s\n", node->nn->path);
  pthread_mutex_lock(&node->nn->lock);

  if(node->nn->anonymous && FUSE_OP_HAVE(unlink))
    {
      /* FIXME, need to lock parent directory structure */
      FUSE_OP_CALL(unlink, node->nn->path);

      /* FIXME, free associated netnode somehow. */
    }

  node->nn->node = NULL;

  pthread_mutex_unlock(&node->nn->lock);
  DEBUG("netnode-lock", "netnode unlocked.\n"); /* no ref to node->nn av. */

  FUNC_EPILOGUE_NORET();
}
