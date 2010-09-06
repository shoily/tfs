#include "alloc.h"

void tfs_release_inode_info_blocks(struct tfs_alloc_inode_info *tainfo)
{
  if (tainfo->inode_bitmap_bh)
    brelse(tainfo->inode_bitmap_bh);
  if (tainfo->data_bitmap_bh)
    brelse(tainfo->data_bitmap_bh);
  if (tainfo->inode_table_bh)
    brelse(tainfo->inode_table_bh);
}

void tfs_error_inode_info(struct tfs_alloc_inode_info *tainfo)
{
  if (tainfo->inode_bitmap_data)
    *tainfo->inode_bitmap_data &= ~(1 << tainfo->inode_index);

  if (tainfo->datablock_bitmap_data)
    *tainfo->datablock_bitmap_data &= ~(1 << tainfo->datablock_index);

  tfs_release_inode_info_blocks(tainfo);
}

int alloc_inode_bitmap(struct super_block *sb, struct tfs_alloc_inode_info *tainfo)
{
  struct tfs_sb_info *si = sb->s_fs_info; 
  struct tfs_super_block *tsb = si->super_block;
  unsigned long *inode_bitmap_data = NULL;
  int ret = 0;
  unsigned int i, j, k;

  mutex_lock(&si->inode_bitmap_mutex);
  for (i = 0; i < tsb->inode_bitmap_blocks; ++i)
    {
      tainfo->inode_bitmap_bh = sb_bread(sb, tsb->inode_bitmap_block_start + i);
      if (!tainfo->inode_bitmap_bh)
	{
	  ret = -EIO;
	  printk("TFS: error !tainfo->inode_bitmap_bh\n");
	  goto inode_unlock;
	}
      inode_bitmap_data = (unsigned long *) tainfo->inode_bitmap_bh->b_data;

      for (j = 0; j < TFS_BLOCK_SIZE; ++j, ++inode_bitmap_data)
	{
	  if (*inode_bitmap_data == (unsigned long) -1)
	    continue;

	  for (k = 0; k < BITS_PER_LONG; ++k)
	    {
	      if ((1 << k) & *inode_bitmap_data)
		continue;

	      tainfo->inode_index = k;
	      tainfo->ino = (i * TFS_BLOCK_SIZE) + (j * BITS_PER_LONG) + k;
	      *inode_bitmap_data |= (1 << k);
	      tainfo->inode_bitmap_data = inode_bitmap_data;
	      goto inode_unlock;
	    }
	}
      brelse(tainfo->inode_bitmap_bh);
      tainfo->inode_bitmap_bh = NULL;
      inode_bitmap_data = NULL;
    }
inode_unlock:
  mutex_unlock(&si->inode_bitmap_mutex);

  if (!ret && !tainfo->ino)
    {
      printk("TFS: no more space for inode\n");
      ret = -ENOSPC;
    }

  return ret;
}

int alloc_datablock_bitmap(struct super_block *sb, struct tfs_alloc_inode_info *tainfo)
{
  struct tfs_sb_info *si = sb->s_fs_info; 
  struct tfs_super_block *tsb = si->super_block;
  unsigned long *datablock_bitmap_data = NULL;
  int ret = 0;
  unsigned int i, j, k;

  mutex_lock(&si->data_bitmap_mutex);
  for (i = 0; i < tsb->data_bitmap_blocks; ++i)
    {
      tainfo->data_bitmap_bh = sb_bread(sb, tsb->data_bitmap_block_start + i);
      if (!tainfo->data_bitmap_bh)
	{
	  ret = -EIO;
	  printk("TFS: error !tainfo->data_bitmap_bh\n");
	  goto data_unlock;
	}
      datablock_bitmap_data = (unsigned long *) tainfo->data_bitmap_bh->b_data;

      for (j = 0; j < TFS_BLOCK_SIZE; ++j, ++datablock_bitmap_data)
	{
	  if (*datablock_bitmap_data == (unsigned long) -1)
	    continue;

	  for (k = 0; k < BITS_PER_LONG; ++k)
	    {
	      if ((1 << k) & *datablock_bitmap_data)
		continue;

	      tainfo->datablock_index = k;
	      tainfo->data_block = (i * TFS_BLOCK_SIZE) + (j * BITS_PER_LONG) + k;
	      *datablock_bitmap_data |= (1 << k);
	      tainfo->datablock_bitmap_data = datablock_bitmap_data;

	      printk("TFS: datablock: %u\n", tainfo->data_block);
	      goto data_unlock;
	    }
	}
      brelse(tainfo->data_bitmap_bh);
      tainfo->data_bitmap_bh = NULL;
      datablock_bitmap_data = NULL;
    }
data_unlock:
  mutex_unlock(&si->data_bitmap_mutex);

  if (!ret && !tainfo->data_block)
    {
      printk("TFS: no more space for data block\n");
      ret = -ENOSPC;
    }

  return ret;
}

struct inode *tfs_new_inode(struct inode *dir, struct tfs_alloc_inode_info *tainfo, int mode)
{
  struct inode *inode_new;
  struct super_block *sb = dir->i_sb;
  struct tfs_sb_info *si = sb->s_fs_info;
  struct tfs_super_block *tsb = si->super_block;
  struct tfs_inode *ti;
  unsigned int block, offset, shift;
  int err, i;

  err = alloc_inode_bitmap(sb, tainfo);
  if (err)
      goto err;

  if (mode & S_IFDIR)
    {
      err = alloc_datablock_bitmap(sb, tainfo);
      if (err)
	goto err;
    }

  shift = (tainfo->ino << TFS_INODE_SIZE_BITS);
  block = tsb->inode_table_block_start + (shift >> TFS_BLOCK_SIZE_BITS);
  offset = shift % TFS_BLOCK_SIZE;

  tainfo->inode_table_bh = sb_bread(sb, block);
  if (!tainfo->inode_table_bh)
    {
      printk("TFS: error reading inode table block: %u\n", block);
      err = -EIO;
      goto err;
    }

  ti = (struct tfs_inode *) (tainfo->inode_table_bh->b_data + offset);
  ti->mode = mode;
  ti->uid = dir->i_uid;
  ti->gid = dir->i_gid;
  ti->ctime = ti->mtime = ti->atime = CURRENT_TIME_SEC.tv_sec;
  ti->hard_link_count = 1;

  for (i = 0; i < TFS_DATA_BLOCKS_PER_INODE; ++i)
    ti->data_blocks[i] = 0;

  if (mode & S_IFDIR)
    {
      ti->size = TFS_BLOCK_SIZE;
      ti->blocks = 1;
      ti->data_blocks[0] = tainfo->data_block;
      mark_buffer_dirty(tainfo->data_bitmap_bh);
    }
  else
    {
      ti->size = 0;
      ti->blocks = 0;
    }

  mark_buffer_dirty(tainfo->inode_table_bh);
  mark_buffer_dirty(tainfo->inode_bitmap_bh);

  inode_new = tfs_inode_get(sb, tainfo->ino);
  if (IS_ERR(inode_new))
    {
      err = PTR_ERR(inode_new);
      printk("TFS: error in tfs_inode_get: %d\n", err);
      goto err;
    }

  printk("TFS: inode creation successful: %u\n", (unsigned int) inode_new->i_ino);

  return inode_new;
  
err:
  tainfo->err = err;
  tfs_error_inode_info(tainfo);

  return NULL;
}
