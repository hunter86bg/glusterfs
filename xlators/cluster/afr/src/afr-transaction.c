/*
  Copyright (c) 2007, 2008 Z RESEARCH, Inc. <http://www.zresearch.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/

#include "dict.h"

#include "afr.h"
#include "afr-transaction.h"

/* {{{ unlock */

int32_t
afr_unlock_common_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
		       int32_t op_ret, int32_t op_errno)
{
	afr_local_t *local;
	int call_count = 0;

	local = frame->local;

	LOCK (&frame->lock);
	{
		call_count = --local->call_count;
	}
	UNLOCK (&frame->lock);

	if (call_count == 0) {
		local->transaction.done (frame, this, 0, 0);
	}
	
	return 0;
}


int
afr_unlock (call_frame_t *frame, xlator_t *this)
{
	struct flock flock;			

	int i = 0;				
	int call_count = 0;		     

	afr_local_t *local = NULL;
	afr_private_t * priv = this->private;

	local = frame->local;
	call_count = up_children_count (priv->child_count, local->child_up); 
	
	if (call_count == 0) {
		local->transaction.done (frame, this, 0, 0);
		return 0;
	}

//	if (local->transaction.type == AFR_ENTRY_RENAME_TRANSACTION) 
//		call_count *= 2;

	local->call_count = call_count;		

	for (i = 0; i < priv->child_count; i++) {				
		flock.l_start = local->transaction.start;			
		flock.l_len   = local->transaction.len;
		flock.l_type  = F_UNLCK;			

		if (local->child_up[i]) {
			switch (local->transaction.type) {
			case AFR_INODE_TRANSACTION:
				if (local->fd) {
					STACK_WIND (frame, afr_unlock_common_cbk,	
						    priv->children[i], 
						    priv->children[i]->fops->finodelk, 
						    local->fd, F_SETLK, &flock); 
				} else {
					STACK_WIND (frame, afr_unlock_common_cbk,	
						    priv->children[i], 
						    priv->children[i]->fops->inodelk, 
						    &local->loc,  F_SETLK, &flock); 
				}
				
				break;
			case AFR_ENTRY_RENAME_TRANSACTION:
/*
				STACK_WIND (frame, afr_unlock_common_cbk,	
					    priv->children[i], 
					    priv->children[i]->fops->entrylk, 
					    &local->transaction.new_parent_loc, 
					    local->transaction.new_basename,
					    GF_DIR_LK_UNLOCK, GF_DIR_LK_WRLCK);
*/
				/* fall through */

			case AFR_ENTRY_TRANSACTION:
				STACK_WIND (frame, afr_unlock_common_cbk,	
					    priv->children[i], 
					    priv->children[i]->fops->entrylk, 
					    &local->transaction.parent_loc, 
					    local->transaction.basename,
					    GF_DIR_LK_UNLOCK, GF_DIR_LK_WRLCK);
				break;
			}
			
			if (!--call_count)
				break;
		}
	}

	return 0;
}

/* }}} */


/* {{{ pending */

int32_t
afr_write_pending_post_op_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
			       int32_t op_ret, int32_t op_errno, dict_t *xattr)
{
	afr_local_t *   local = NULL;
	
	int call_count = -1;

	local = frame->local;

	LOCK (&frame->lock);
	{
		call_count = --local->call_count;
	}
	UNLOCK (&frame->lock);

	if (call_count == 0) {
		switch (local->transaction.type) {
		case AFR_INODE_TRANSACTION:
			afr_unlock (frame, this);
			break;
		case AFR_ENTRY_TRANSACTION:
		case AFR_ENTRY_RENAME_TRANSACTION:
			afr_unlock (frame, this);
			break;
		}
	}

	return 0;	
}


int 
afr_write_pending_post_op (call_frame_t *frame, xlator_t *this)
{
	afr_private_t * priv = this->private;

	int i = 0;				
	int call_count = 0;		     
	dict_t *xattr = dict_ref (get_new_dict ());

	afr_local_t *local = NULL;

	local = frame->local;

	dict_set_static_bin (xattr, local->transaction.pending, priv->pending_dec_array, 
			     priv->child_count * sizeof (int32_t));

	call_count = up_children_count (priv->child_count, local->child_up); 

	local->call_count = call_count;		

	if (call_count == 0) {
		/* no child is up */
		dict_unref (xattr);

		local->transaction.done (frame, this, -1, ENOTCONN);
		return 0;
	}

	for (i = 0; i < priv->child_count; i++) {					
		if (local->child_up[i]) {
			switch (local->transaction.type) {
			case AFR_INODE_TRANSACTION:
				STACK_WIND (frame, afr_write_pending_post_op_cbk,
					    priv->children[i], 
					    priv->children[i]->fops->xattrop,
					    local->fd, local->loc.path, 
					    GF_XATTROP_ADD_ARRAY, xattr);
				break;

			case AFR_ENTRY_RENAME_TRANSACTION:
				/* TODO: write pending on new_parent_loc also */

			case AFR_ENTRY_TRANSACTION:
				STACK_WIND (frame, afr_write_pending_post_op_cbk,
					    priv->children[i], 
					    priv->children[i]->fops->xattrop,
					    local->fd, local->transaction.parent_loc.path, 
					    GF_XATTROP_ADD_ARRAY, xattr);
				break;
			}
			if (!--call_count)
				break;
		}
	}
	
	dict_unref (xattr);
	return 0;
}


int32_t
afr_write_pending_pre_op_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
			      int32_t op_ret, int32_t op_errno, dict_t *xattr)
{
	afr_local_t *   local = NULL;
	afr_private_t * priv  = this->private;
	loc_t       *   loc   = NULL;

	int call_count  = -1;
	int child_index = (long) cookie;

	local = frame->local;
	loc   = &local->loc;

	LOCK (&frame->lock);
	{
		if (op_ret == -1) {
			local->child_up[child_index] = 0;

			if (!child_went_down (op_ret, op_errno)) {
				gf_log (this->name, GF_LOG_ERROR,
					"xattrop failed on child %s: %s",
					priv->children[child_index]->name, 
					strerror (op_errno));
			}
		}

		call_count = --local->call_count;
	}
	UNLOCK (&frame->lock);

	if (call_count == 0) {
		local->transaction.fop (frame, this);
	}

	return 0;	
}


int 
afr_write_pending_pre_op (call_frame_t *frame, xlator_t *this)
{
	afr_private_t * priv = this->private;

	int i = 0;				
	int call_count = 0;		     
	dict_t *xattr = dict_ref (get_new_dict ());
	afr_local_t *local = NULL;

	local = frame->local;
	dict_ref (xattr);

	call_count = up_children_count (priv->child_count, local->child_up); 
//	if (local->transaction.type == AFR_DIR_LINK_TRANSACTION)
//		local->call_count *= 2;

	if (call_count == 0) {
		/* no child is up */
		dict_unref (xattr);

		local->transaction.done (frame, this, -1, ENOTCONN);
		return 0;
	}

	local->call_count = call_count;		

	for (i = 0; i < priv->child_count; i++) {					
		if (local->child_up[i]) {
			dict_set_static_bin (xattr, local->transaction.pending, 
					     priv->pending_inc_array, 
					     priv->child_count * sizeof (int32_t));

			switch (local->transaction.type) {
			case AFR_INODE_TRANSACTION:
				STACK_WIND_COOKIE (frame, afr_write_pending_pre_op_cbk,
						   (void *) (long) i,
						   priv->children[i], 
						   priv->children[i]->fops->xattrop,
						   local->fd, local->loc.path, 
						   GF_XATTROP_ADD_ARRAY, xattr);
				break;
				
			case AFR_ENTRY_RENAME_TRANSACTION:
				/* TODO: write pending on new_parent */

			case AFR_ENTRY_TRANSACTION:
				STACK_WIND_COOKIE (frame, afr_write_pending_pre_op_cbk,
						   (void *) (long) i,
						   priv->children[i], 
						   priv->children[i]->fops->xattrop,
						   local->fd, 
						   local->transaction.parent_loc.path, 
						   GF_XATTROP_ADD_ARRAY, xattr);
				break;
			}
			if (!--call_count)
				break;
		}
	}

	dict_unref (xattr);
	return 0;
}

/* }}} */

/* {{{ lock */

static
int afr_lock_rec (call_frame_t *frame, xlator_t *this, int child_index);

int32_t
afr_lock_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
	      int32_t op_ret, int32_t op_errno)
{
	afr_local_t *   local = NULL;
	afr_private_t * priv = NULL;
	
	int child_index = (long) cookie;

	local = frame->local;
	priv  = this->private;

	LOCK (&frame->lock);
	{
		if (op_ret == -1) {
			local->child_up[child_index] = 0;
		}
	}
	UNLOCK (&frame->lock);

	afr_lock_rec (frame, this, child_index + 1);

	return 0;
}


static
int afr_lock_rec (call_frame_t *frame, xlator_t *this, int child_index)
{
	afr_local_t *   local = NULL;
	afr_private_t * priv  = NULL;

	struct flock flock;

	local = frame->local;
	priv  = this->private;

	flock.l_start = local->transaction.start;
	flock.l_len   = local->transaction.len;
	flock.l_type  = F_WRLCK;

	/* skip over children that are down */
	while ((child_index < priv->child_count)
	       && !local->child_up[child_index])
		child_index++;

	if (child_index == priv->child_count) {
		/* we're done locking */
		afr_write_pending_pre_op (frame, this);
		return 0;
	}

	switch (local->transaction.type) {
	case AFR_INODE_TRANSACTION:		
		if (local->fd) {
			STACK_WIND_COOKIE (frame, afr_lock_cbk,
					   (void *) (long) child_index,
					   priv->children[child_index], 
					   priv->children[child_index]->fops->finodelk,
					   local->fd, F_SETLKW, &flock);
			
		} else {
			STACK_WIND_COOKIE (frame, afr_lock_cbk,
					   (void *) (long) child_index,
					   priv->children[child_index], 
					   priv->children[child_index]->fops->inodelk,
					   &local->loc, F_SETLKW, &flock);
		}
		
		break;
		
	case AFR_ENTRY_RENAME_TRANSACTION:
/*
  STACK_WIND_COOKIE (frame, afr_lock_cbk,
  (void *) (long) child_index,
  priv->children[child_index], 
  priv->children[child_index]->fops->entrylk, 
  &local->transaction.new_parent_loc, 
  local->transaction.new_basename,
  GF_DIR_LK_LOCK, GF_DIR_LK_WRLCK);
*/
		/* fall through */
		
	case AFR_ENTRY_TRANSACTION:
		if (local->fd) {
			STACK_WIND_COOKIE (frame, afr_lock_cbk,
					   (void *) (long) child_index,	
					   priv->children[child_index], 
					   priv->children[child_index]->fops->fentrylk, 
					   local->fd, 
					   local->transaction.basename,
					   GF_DIR_LK_LOCK, GF_DIR_LK_WRLCK);
		} else {
			STACK_WIND_COOKIE (frame, afr_lock_cbk,
					   (void *) (long) child_index,	
					   priv->children[child_index], 
					   priv->children[child_index]->fops->entrylk, 
					   &local->transaction.parent_loc, 
					   local->transaction.basename,
					   GF_DIR_LK_LOCK, GF_DIR_LK_WRLCK);
			}
		break;
	}

	return 0;
}


int32_t afr_lock (call_frame_t *frame, xlator_t *this)
{
	return afr_lock_rec (frame, this, 0);
}


/* }}} */

int32_t
afr_transaction_resume (call_frame_t *frame, xlator_t *this)
{
	afr_local_t *local = NULL;
	
	local = frame->local;
	
	if (local->transaction.erase_pending) {
		afr_write_pending_post_op (frame, this);
	} else {
		afr_unlock (frame, this);
	}

	return 0;
}


/**
 * afr_transaction_child_died - inform that a child died during an fop
 */

void
afr_transaction_child_died (call_frame_t *frame, xlator_t *this)
{
	afr_local_t * local = NULL;

	local = frame->local;
	local->transaction.erase_pending = 0;
}


int32_t
afr_inode_transaction (call_frame_t *frame, xlator_t *this)
{
	afr_local_t *local = NULL;
	afr_private_t *priv = NULL;

	local = frame->local;
	priv  = this->private;

	local->transaction.resume = afr_transaction_resume;
	local->transaction.type   = AFR_INODE_TRANSACTION;

	if (up_children_count (priv->child_count, local->child_up) !=
	    priv->child_count) {
		local->transaction.erase_pending = 0;
	}

	afr_lock (frame, this);

	return 0;
}


int32_t
afr_entry_transaction (call_frame_t *frame, xlator_t *this)

{
	afr_local_t *   local = NULL;
	afr_private_t * priv  = NULL;

	local = frame->local;
	priv  = this->private;

	local->transaction.resume = afr_transaction_resume;
	local->transaction.type   = AFR_ENTRY_TRANSACTION;

	if (up_children_count (priv->child_count, local->child_up) !=
	    priv->child_count) {
		local->transaction.erase_pending = 0;
	}

	afr_lock (frame, this);

	return 0;
}


int32_t
afr_entry_rename_transaction (call_frame_t *frame, xlator_t *this)
{
	afr_local_t *   local = NULL;
	afr_private_t * priv  = NULL;

	local = frame->local;
	priv  = this->private;

	local->transaction.resume = afr_transaction_resume;
	local->transaction.type   = AFR_ENTRY_RENAME_TRANSACTION;

	if (up_children_count (priv->child_count, local->child_up) !=
	    priv->child_count) {
		local->transaction.erase_pending = 0;
	}

	afr_lock (frame, this);

	return 0;
}
