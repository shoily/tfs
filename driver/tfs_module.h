#ifndef _TFS_MODULE_H
#define _TFS_MODULE_H


#include "tfs.h"

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mutex.h>
#include <linux/seqlock.h>

#define TFS_BLK_GRP 2
#define TFS_BLK_PER_GRP 4

struct tfs_sb_info
{
  struct tfs_super_block *super_block;
  struct buffer_head *bh;
  struct mutex inode_bitmap_mutex;
  struct mutex data_bitmap_mutex;
};

struct tfs_inode_info
{
  sector_t data_blocks[TFS_DATA_BLOCKS_PER_INODE];
  sector_t cached_first_logical_blocks[TFS_BLK_GRP];
  sector_t cached_data_blocks[TFS_BLK_GRP][TFS_BLK_PER_GRP];
  seqlock_t cached_block_seqlocks[TFS_BLK_GRP];
  sector_t root_indirect_data_block;
  int cached_next_slot;
  struct mutex cached_block_mutex;
  struct inode inode;
};

#define TFS_INODE(vfs_inode) container_of(vfs_inode, struct tfs_inode_info, inode)

struct inode *tfs_inode_get(struct super_block *sb, int ino);
void tfs_destroy_inode(struct inode *inode);
int tfs_write_begin(struct file *file, struct address_space *mapping,
		    loff_t pos, unsigned len, unsigned flags, 
		    struct page **pagep, void **fsdata);
int __tfs_write_begin(struct file *file, struct address_space *mapping,
		      loff_t pos, unsigned len, unsigned flags,
		      struct page **pagep, void **fsdata);
int tfs_commit_write(struct page *page, loff_t pos, unsigned len);
loff_t tfs_llseek(struct file *file, loff_t offset, int origin);
int tfs_fsync(struct file *file, struct dentry *dentry, int datasync);
int tfs_getblocks(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create);
int tfs_sync_inode(struct inode *inode);

#endif
