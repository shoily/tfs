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
    *tainfo->datablock_bitmap_data &= ~((1 << tainfo->datablock_index) | (1 << (tainfo->datablock_index + 1)) | (1 << (tainfo->datablock_index + 2)) | (1 << (tainfo->datablock_index + 3)));

  tfs_release_inode_info_blocks(tainfo);
}

struct inode *tfs_new_inode(struct inode *dir, struct tfs_alloc_inode_info *tainfo, int mode)
{
  struct inode *inode_new;
  struct super_block *sb = dir->i_sb;
  struct tfs_sb_info *si = sb->s_fs_info;
  struct tfs_super_block *tsb = si->super_block;
  struct tfs_inode *ti;
  unsigned char *inode_bitmap_data = NULL, *datablock_bitmap_data = NULL;
  unsigned int i, j, k;
  unsigned int block, offset, shift;
  int err;

  memset(tainfo, 0, sizeof(*tainfo));
  
  mutex_lock(&si->inode_bitmap_mutex);
  for (i = 0; i < tsb->inode_bitmap_blocks; ++i)
    {
      tainfo->inode_bitmap_bh = sb_bread(sb, tsb->inode_bitmap_block_start + i);
      if (!tainfo->inode_bitmap_bh)
	{
	  err = -EIO;
	  printk("TFS: error !tainfo->inode_bitmap_bh\n");
	  goto err;
	}
      inode_bitmap_data = tainfo->inode_bitmap_bh->b_data;

      for (j = 0; j < TFS_BLOCK_SIZE; ++j, ++inode_bitmap_data)
	{
	  if (*inode_bitmap_data == 0xFF)
	    continue;

	  for (k = 0; k < 8; ++k)
	    {
	      if ((1 << k) & *inode_bitmap_data)
		continue;

	      tainfo->inode_index = k;
	      tainfo->ino = (i * TFS_BLOCK_SIZE) + (j * 8) + k;
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

  if (!tainfo->ino)
    {
      printk("TFS: no more space for inode\n");
      err = -ENOSPC;
      goto err;
    }

  mutex_lock(&si->data_bitmap_mutex);
  for (i = 0; i < tsb->data_bitmap_blocks; ++i)
    {
      tainfo->data_bitmap_bh = sb_bread(sb, tsb->data_bitmap_block_start + i);
      if (!tainfo->data_bitmap_bh)
	{
	  err = -EIO;
	  printk("TFS: error !tainfo->data_bitmap_bh\n");
	  goto err;
	}
      datablock_bitmap_data = tainfo->data_bitmap_bh->b_data;

      for (j = 0; j < TFS_BLOCK_SIZE; ++j, ++datablock_bitmap_data)
	{
	  if (*datablock_bitmap_data == 0xFF)
	    continue;

	  for (k = 0; k < 8; k += 4)
	    {
	      if (((1 << k) & *datablock_bitmap_data) ||
		  ((1 << (k + 1)) & *datablock_bitmap_data) ||
		  ((1 << (k + 2)) & *datablock_bitmap_data) ||
		  ((1 << (k + 3)) & *datablock_bitmap_data))
		continue;

	      tainfo->datablock_index = k;
	      tainfo->data_block = (i * TFS_BLOCK_SIZE) + (j * 8) + k;
	      *datablock_bitmap_data |= (1 << k) | (1 << (k + 1)) | (1 << (k + 2)) | (1 << (k + 3));
	      tainfo->datablock_bitmap_data = datablock_bitmap_data;

	      goto data_unlock;
	    }
	}
      brelse(tainfo->data_bitmap_bh);
      tainfo->data_bitmap_bh = NULL;
      datablock_bitmap_data = NULL;
    }
data_unlock:
  mutex_unlock(&si->data_bitmap_mutex);

  if (!tainfo->data_block)
    {
      printk("TFS: no more space for data block\n");
      err = -ENOSPC;
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
  ti->size = TFS_BLOCK_SIZE * TFS_DATA_BLOCKS_PER_INODE;
  ti->blocks = TFS_DATA_BLOCKS_PER_INODE;
  for (i = 0; i < TFS_DATA_BLOCKS_PER_INODE; ++i)
    ti->data_blocks[i] = tainfo->data_block + i;

  mark_buffer_dirty(tainfo->inode_table_bh);
  mark_buffer_dirty(tainfo->inode_bitmap_bh);
  mark_buffer_dirty(tainfo->data_bitmap_bh);

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
