#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <linux/writeback.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/mpage.h>
#include <linux/time.h>

#include "tfs_module.h"
#include "alloc.h"

static const struct address_space_operations tfs_aops;
extern struct file_operations tfs_file_operations;
extern struct file_operations tfs_dir_operations;
extern struct inode_operations tfs_file_inode_operations;
extern struct inode_operations tfs_dir_inode_operations;

struct inode *tfs_inode_get(struct super_block *sb, int ino)
{
  struct inode *inode = NULL;
  struct tfs_inode_info *ti;
  struct tfs_inode *tfs_inode;
  struct buffer_head *bh = NULL;
  struct tfs_sb_info *si = sb->s_fs_info;
  unsigned int block, offset, shift;
  int ret, i;

  printk("TFS: tfs_inode_get: %u\n", ino);

  inode = iget_locked(sb, ino);
  if (!inode)
    {
      ret = -ENOMEM;
      goto err;
    }

  if (!(inode->i_state & I_NEW))
    {
      printk("TFS: tfs_inode_get inode is not I_NEW\n");
      return inode;
    }

  ti = TFS_INODE(inode);

  shift = ino << TFS_INODE_SIZE_BITS;
  block = si->super_block->inode_table_block_start + (shift >> TFS_BLOCK_SIZE_BITS);
  offset = shift % TFS_BLOCK_SIZE;

  printk("TFS: block and offset: %u, %u\n", block, offset);

  bh = sb_bread(sb, block);
  if (!bh)
    {
      ret = -EIO;
      goto err;
    }

  tfs_inode = (struct tfs_inode *) (bh->b_data + offset);

  printk("TFS: inode mode: %u\n", tfs_inode->mode);

  inode->i_mode = tfs_inode->mode;
  inode->i_uid = tfs_inode->uid;
  inode->i_gid = tfs_inode->gid;
  inode->i_nlink = tfs_inode->hard_link_count;
  inode->i_size = tfs_inode->size;
  inode->i_atime.tv_sec = tfs_inode->atime;
  inode->i_ctime.tv_sec = tfs_inode->ctime;
  inode->i_mtime.tv_sec = tfs_inode->mtime;
  inode->i_blocks = tfs_inode->blocks;

  for (i = 0; i < TFS_DATA_BLOCKS_PER_INODE; ++i)
    ti->data_blocks[i] = tfs_inode->data_blocks[i];

  ti->root_indirect_data_block = tfs_inode->root_indirect_data_block;
  ti->cached_next_slot = 0;

  //TODO: implement setattr
  if (S_ISREG(inode->i_mode))
    {
      printk("TFS: its a regular inode\n");
      inode->i_op = &tfs_file_inode_operations;
      inode->i_fop = &tfs_file_operations;
    }
  else if (S_ISDIR(inode->i_mode))
    {
      printk("TFS: its a directory inode\n");
      inode->i_op = &tfs_dir_inode_operations;
      inode->i_fop = &tfs_dir_operations;
    }

  inode->i_mapping->a_ops = &tfs_aops;

  brelse(bh);
  unlock_new_inode(inode);

  printk("TFS: tfs_inode_get successful\n");

  return inode;

 err:
  printk("TFS: tfs_inode_get error : %d\n", ret);
  unlock_new_inode(inode);
  iput(inode);

  if (bh)
    brelse(bh);
  return ERR_PTR(ret);
}

int tfs_sync_inode(struct inode *inode)
{
  struct writeback_control wbc = 
  {
    .sync_mode = WB_SYNC_ALL,
    .nr_to_write = 0
  };

  printk("TFS: tfs_sync_inode: %u\n", (unsigned int) inode->i_ino);

  return sync_inode(inode, &wbc);
}

#define TFS_BLOCK_FOUND 1
#define TFS_BLOCK_NOT_FOUND 2

int tfs_getblocks(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create)
{
  int i;
  int count = 0;
  struct tfs_inode_info *ti = TFS_INODE(inode);
  sector_t blocknum;
  unsigned seq;
  size_t bsize = 0;
  int status;
  int err = 0;
  struct tfs_alloc_inode_info tainfo;
  unsigned blkbits = inode->i_blkbits;
  unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
  sector_t last_block_in_file = (i_size_read(inode) + blocks_per_page) >> blkbits;

  printk("TFS: tfs_getblocks - block=%u, req size=%u\n", (unsigned)iblock, bh_result->b_size);

  if (iblock < TFS_DATA_BLOCKS_PER_INODE)
    {
      if (iblock > last_block_in_file)
	{
	  if (!create)
	    return -EINVAL;
	  else 
	    goto alloc_directblock;
	}

      count = 0;
      blocknum = iblock;
      while (blocknum < TFS_DATA_BLOCKS_PER_INODE &&
	     count < (bh_result->b_size >> inode->i_blkbits) &&
	     ti->data_blocks[blocknum] &&
	     (!count || ti->data_blocks[blocknum] == (ti->data_blocks[blocknum - 1] + 1)))
	{
	  ++blocknum;
	  ++count;
	}

      if (!count)
	{
	  if (!create)
	      return -EINVAL;
	  else
	    goto alloc_directblock;
	}

      map_bh(bh_result, inode->i_sb, ti->data_blocks[iblock]);
      bh_result->b_size = count << inode->i_blkbits;

      printk("TFS: mapped: data block=%u, size=%u\n", (unsigned) ti->data_blocks[iblock], bh_result->b_size);

      return 0;

alloc_directblock:
      tfs_init_alloc_inode_info(tainfo);

      err = alloc_datablock_bitmap(inode->i_sb, &tainfo);
      if (err)
	goto error_alloc;
      
      ti->data_blocks[iblock] = tainfo.data_block;
      inode->i_blocks++;
      mark_inode_dirty(inode);

      map_bh(bh_result, inode->i_sb, tainfo.data_block);
      bh_result->b_size = 1 << inode->i_blkbits;

      printk("TFS: mapped data block=%u, size=%u\n", (unsigned) tainfo.data_block, bh_result->b_size);

      tfs_release_inode_info_blocks(&tainfo);
    }
  else
    {
      sector_t first_rounded_logical_block = iblock - (iblock % TFS_BLK_PER_GRP);
      sector_t first_rounded_relative_block = (iblock - TFS_DATA_BLOCKS_PER_INODE) - ((iblock - TFS_DATA_BLOCKS_PER_INODE) % TFS_BLK_PER_GRP);
      u32 rounded_block_index;
      sector_t rid_block, indirect_block, block;
      u32 indirect_block_index, block_index;
      struct buffer_head *rid_bh = NULL, *id_bh = NULL;

      printk("TFS: first rounded block=%u, relative block=%u, blocknum=%u\n", (unsigned) first_rounded_logical_block, (unsigned) first_rounded_relative_block, (unsigned) (iblock - TFS_DATA_BLOCKS_PER_INODE));

      if (iblock > last_block_in_file)
	{
	  if (!create)
	    return -EINVAL;
	  else 
	    goto alloc_indirectblock;
	}

      for (i = 0; i < TFS_BLK_GRP; ++i)
	{
	  sector_t *pblock = &ti->cached_data_blocks[i][0];
	  count = 0;
	  blocknum = iblock - TFS_DATA_BLOCKS_PER_INODE;
	  printk("TFS: cached_first_logical_block[%d]=%u, first block=%u\n", i, (unsigned) ti->cached_first_logical_blocks[i], (unsigned) pblock[blocknum - first_rounded_relative_block]);
	  seq = read_seqbegin(&ti->cached_block_seqlocks[i]);
	  if (first_rounded_logical_block == ti->cached_first_logical_blocks[i] && pblock[blocknum - first_rounded_relative_block])
	    {
	      status = TFS_BLOCK_FOUND;
	      while (count < (bh_result->b_size >> inode->i_blkbits) &&
		     (blocknum - first_rounded_relative_block) < TFS_BLK_PER_GRP &&
		     pblock[blocknum - first_rounded_relative_block] &&
		     (!count || (pblock[blocknum - first_rounded_relative_block] == pblock[blocknum - first_rounded_relative_block - 1] + 1)))
		{
		  ++blocknum;
		  ++count;
		}
	      bsize = count << inode->i_blkbits;
	    }
	  else
	    {
	      status = TFS_BLOCK_NOT_FOUND;
	    }

	  if (read_seqretry(&ti->cached_block_seqlocks[i], seq) && status == TFS_BLOCK_FOUND)
	    break;

	  if (status == TFS_BLOCK_FOUND)
	    {
	      map_bh(bh_result, inode->i_sb, pblock[iblock - TFS_DATA_BLOCKS_PER_INODE - first_rounded_relative_block]);
	      bh_result->b_size = bsize;

	      printk("TFS: mapped: data block=%u, size=%u\n", (unsigned) bh_result->b_blocknr, bsize);
	      return 0;
	    }
	}

alloc_indirectblock:
      
      if (create && !ti->root_indirect_data_block)
	{
	  tfs_init_alloc_inode_info(tainfo);
	  err = alloc_datablock_bitmap(inode->i_sb, &tainfo);
	  if (err)
	    {
	      printk("TFS: error allocating root indirect data block: %d\n", err);
	      goto error_alloc;
	    }

	  printk("TFS: allocated root indirect data block: %u\n", tainfo.data_block);
	  ti->root_indirect_data_block = tainfo.data_block;
	  inode->i_blocks++;
	  mark_inode_dirty(inode);
	  tfs_release_inode_info_blocks(&tainfo);
	}
      rid_block = ti->root_indirect_data_block;

      if (!rid_block)
	{
	  printk("TFS: root indirect data block cannot be 0");
	  return -EINVAL;
	}
      printk("TFS: root indirect block: %u\n", (unsigned) rid_block);

      rid_bh = sb_bread(inode->i_sb, rid_block);
      if (!rid_bh)
	{
	  printk("TFS: error reading root indirect data block: %u\n", (unsigned) rid_block);
	  return -EIO;
	}

      indirect_block_index = (iblock - TFS_DATA_BLOCKS_PER_INODE) / (TFS_BLOCK_SIZE / sizeof(u32));
      if (indirect_block_index >= TFS_BLOCK_SIZE / sizeof(u32))
	{
	  printk("TFS: invalid indirect block index: %u\n", indirect_block_index);
	  brelse(rid_bh);
	  return -EINVAL;
	}

      printk("TFS: indirect_block_index: %u\n", indirect_block_index);

      indirect_block = (sector_t) *((u32 *) rid_bh->b_data + indirect_block_index);
      if (create && !indirect_block)
	{
	  tfs_init_alloc_inode_info(tainfo);
	  err = alloc_datablock_bitmap(inode->i_sb, &tainfo);
	  if (err)
	    {
	      printk("TFS: error allocating indirect data block: %d\n", err);
	      brelse(rid_bh);
	      goto error_alloc;
	    }

	  printk("TFS: allocated indirect data block: %u\n", tainfo.data_block);
	  *((u32 *) rid_bh->b_data + indirect_block_index) = indirect_block = tainfo.data_block;
	  mark_buffer_dirty(rid_bh);
	  inode->i_blocks++;
	  mark_inode_dirty(inode);
	  tfs_release_inode_info_blocks(&tainfo);
	}
      brelse(rid_bh);
      if (!indirect_block)
	{
	  printk("TFS: indirect block cannot be 0\n");
	  return -EINVAL;
	}

      printk("TFS: indirect block: %u\n", (unsigned) indirect_block);

      id_bh = sb_bread(inode->i_sb, indirect_block);
      if (!id_bh)
	{
	  printk("TFS: error reading indirect block: %u\n", (unsigned) indirect_block);
	  return -EIO;
	}
      block_index = (iblock - TFS_DATA_BLOCKS_PER_INODE) % (TFS_BLOCK_SIZE / sizeof(u32));
      if (block_index >= (TFS_BLOCK_SIZE / sizeof(u32)))
	{
	  printk("TFS: invalid block index: %u\n", block_index);
	  brelse(id_bh);
	  return -EINVAL;
	}

      printk("TFS: block_index: %u\n", block_index);

      block = (sector_t) *((u32 *) id_bh->b_data + block_index);
      if (create && !block)
	{
	  tfs_init_alloc_inode_info(tainfo);
	  err = alloc_datablock_bitmap(inode->i_sb, &tainfo);
	  if (err)
	    {
	      printk("TFS: error allocating data block: %d\n", err);
	      brelse(id_bh);
	      goto error_alloc;
	    }

	  printk("TFS: allocated data block: %u\n", tainfo.data_block);
	  *((u32 *) id_bh->b_data + block_index) = block = tainfo.data_block;
	  mark_buffer_dirty(id_bh);
	  inode->i_blocks++;
	  mark_inode_dirty(inode);
	  tfs_release_inode_info_blocks(&tainfo);
	}
      
      if (!block)
	{
	  printk("TFS: block cannot be 0\n");
	  brelse(id_bh);
	  return -EINVAL;
	}

      printk("TFS: data block: %u\n", (unsigned) block);

      map_bh(bh_result, inode->i_sb, block);
      bh_result->b_size = 1 << inode->i_blkbits;
      printk("TFS: mapped data block=%u, size=%u\n", (unsigned) block, bh_result->b_size);

      mutex_lock(&ti->cached_block_mutex);

      for (i = 0; i < TFS_BLK_GRP; ++i)
	{
	  if (first_rounded_logical_block == ti->cached_first_logical_blocks[i])
	    {
	      ti->cached_next_slot = i;
	      break;
	    }
	}

      if (ti->cached_next_slot == TFS_BLK_GRP)
	ti->cached_next_slot = 0;

      rounded_block_index = first_rounded_relative_block % (TFS_BLOCK_SIZE / sizeof(u32));
      printk("TFS: rounded block index: %u, next slot: %u\n", rounded_block_index, ti->cached_next_slot);

      write_seqlock(&ti->cached_block_seqlocks[ti->cached_next_slot]);
      ti->cached_first_logical_blocks[ti->cached_next_slot] = first_rounded_logical_block;

      for (i = 0; i < TFS_BLK_PER_GRP; ++i)
	{
	  ti->cached_data_blocks[ti->cached_next_slot][i] = (sector_t) *(((u32 *) id_bh->b_data) + rounded_block_index + i);
	  printk("TFS: cached data blocks[%d]=%u\n", i, (unsigned) ti->cached_data_blocks[ti->cached_next_slot][i]);
	}

      write_sequnlock(&ti->cached_block_seqlocks[ti->cached_next_slot]);
      ti->cached_next_slot++;

      mutex_unlock(&ti->cached_block_mutex);

      brelse(id_bh);
    }

  return 0;
error_alloc:
  tfs_error_inode_info(&tainfo);
  return err;
}


static int tfs_readpages(struct file *file, struct address_space *mapping, struct list_head *pages, unsigned nr_pages)
{
  printk("TFS: tfs_readpages: %u\n", (unsigned int) mapping->host->i_ino);
  return mpage_readpages(mapping, pages, nr_pages, tfs_getblocks);
}

static int tfs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
  printk("TFS: tfs_writepages: %u, %u\n", (unsigned int) mapping->host->i_ino, (unsigned) wbc->nr_to_write);
  return mpage_writepages(mapping, wbc, tfs_getblocks);
}

static int tfs_readpage(struct file *file, struct page *page)
{
  printk("TFS: tfs_readpage: %u\n", (unsigned int) page->mapping->host->i_ino);
  return mpage_readpage(page, tfs_getblocks);
}

static int tfs_writepage(struct page *page, struct writeback_control *wbc)
{
  printk("TFS: tfs_writepage: %u\n", (unsigned int) page->mapping->host->i_ino);

  return mpage_writepage(page, tfs_getblocks, wbc);
}

static sector_t tfs_bmap(struct address_space *mapping, sector_t block)
{
  printk("TFS: tfs_bmap: %u\n", (unsigned int) mapping->host->i_ino);

  return generic_block_bmap(mapping, block, tfs_getblocks);
}

int __tfs_write_begin(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata)
{
  printk("TFS: __tfs_write_begin: %u\n", (unsigned int) mapping->host->i_ino);

  return block_write_begin(file, mapping, pos, len, flags, pagep, fsdata, tfs_getblocks);
}


int tfs_write_begin(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata)
{
  printk("TFS: tfs_write_begin: %u\n", (unsigned int) mapping->host->i_ino);

  *pagep = NULL;
  return __tfs_write_begin(file, mapping, pos, len, flags, pagep, fsdata);
}

int tfs_write_end(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct page *page, void *fsdata)
{
  printk("TFS: tfs_write_end: %u\n", (unsigned int) mapping->host->i_ino);

  return generic_write_end(file, mapping, pos, len, copied, page, fsdata);
}


int tfs_commit_write(struct page *page, loff_t pos, unsigned len)
{
  struct address_space *mapping = page->mapping;
  struct inode *inode = mapping->host;
  unsigned int copied;
  int err = 0;

  printk("tfs_commit_write: %u\n", (unsigned int) inode->i_ino);

  copied = block_write_end(NULL, mapping, pos, len, len, page, NULL);

  if (pos + copied > inode->i_size)
    {
      i_size_write(inode, pos + copied);
      mark_inode_dirty(inode);
    }

  if (IS_DIRSYNC(inode))
      err = write_one_page(page, 1);
  else
    unlock_page(page);

  return err;
}

loff_t tfs_llseek(struct file *file, loff_t offset, int origin)
{
  loff_t ret;
  struct inode *inode = file->f_mapping->host;

  printk("TFS: tfs_llseek: %u\n", (unsigned int) inode->i_ino);

  mutex_lock(&file->f_dentry->d_inode->i_mutex);

  switch(origin)
    {
    case SEEK_SET: 
      if (offset > inode->i_size)
	{
	  ret = -EINVAL;
	  goto unlock_llseek_mutex;
	}
      break;
    case SEEK_END:
      if ((offset + inode->i_size) > inode->i_size)
	{
	  ret = -EINVAL;
	  goto unlock_llseek_mutex;
	}
      offset += inode->i_size;
      break;
    case SEEK_CUR:
      if ((offset + file->f_pos) > inode->i_size)
	{
	  ret = -EINVAL;
	  goto unlock_llseek_mutex;
	}
      offset += file->f_pos;
      break;
    }

  if (offset < 0 || offset > inode->i_sb->s_maxbytes)
    {
      ret = -EINVAL;
      goto unlock_llseek_mutex;
    }

  if (offset != file->f_pos)
    {
      file->f_pos = offset;
      file->f_version = 0;
    }

unlock_llseek_mutex:
  mutex_unlock(&file->f_dentry->d_inode->i_mutex);

  return ret;
}

int tfs_fsync(struct file *file, struct dentry *dentry, int datasync)
{
  struct inode *inode = dentry->d_inode;

  printk("TFS: tfs_fsync: %u\n", (unsigned int) inode->i_ino);

  if (!(inode->i_state & I_DIRTY))
    return 0;

  if (datasync && !(inode->i_state & I_DIRTY_DATASYNC))
    return 0;

  tfs_sync_inode(dentry->d_inode);
  return 0;
}

static const struct address_space_operations tfs_aops =
  {
    .readpage = tfs_readpage,
    .writepage = tfs_writepage,
    .readpages = tfs_readpages,
    .writepages = tfs_writepages,
    .bmap = tfs_bmap,
    .sync_page = block_sync_page,
    .write_begin = tfs_write_begin,
    .write_end = tfs_write_end
  };
