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
  if (inode)
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

int tfs_getblocks(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create)
{
  int i;
  int count = 0;
  struct tfs_inode_info *ti = TFS_INODE(inode);

  printk("TFS: tfs_getblocks - %u, %u, %u\n", (unsigned)iblock, bh_result->b_size, inode->i_blkbits);

  if (iblock >= TFS_DATA_BLOCKS_PER_INODE)
    return -EINVAL;

  for (i = iblock; i < TFS_DATA_BLOCKS_PER_INODE && i < (bh_result->b_size >> inode->i_blkbits); ++i, ++count);

  map_bh(bh_result, inode->i_sb, ti->data_blocks[iblock]);
  bh_result->b_size = count << inode->i_blkbits;

  printk("TFS: mapped %u, %u\n", bh_result->b_size, ti->data_blocks[iblock]);

  return 0;
}


static int tfs_readpages(struct file *file, struct address_space *mapping, struct list_head *pages, unsigned nr_pages)
{
  printk("TFS: tfs_readpages: %u\n", (unsigned int) mapping->host->i_ino);
  return mpage_readpages(mapping, pages, nr_pages, tfs_getblocks);
}

static int tfs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
  printk("TFS: tfs_writepages: %u\n", (unsigned int) mapping->host->i_ino);
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
