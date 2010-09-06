#ifndef _TFS_ALLOC_H
#define _TFS_ALLOC_H

#include "tfs_module.h"

struct tfs_alloc_inode_info
{
  struct buffer_head *inode_bitmap_bh, *inode_table_bh, *data_bitmap_bh;
  unsigned long *inode_bitmap_data, *datablock_bitmap_data;
  unsigned int inode_index, datablock_index;
  unsigned int ino, data_block;
  int slot_page, slot_idx;
  int err;
};

#define tfs_init_alloc_inode_info(tai) memset(&tai, 0, sizeof(tai))
void tfs_release_inode_info_blocks(struct tfs_alloc_inode_info *tainfo);
void tfs_error_inode_info(struct tfs_alloc_inode_info *tainfo);
struct inode *tfs_new_inode(struct inode *dir, struct tfs_alloc_inode_info *tainfo, int mode);
int alloc_inode_bitmap(struct super_block *sb, struct tfs_alloc_inode_info *tainfo);
int alloc_datablock_bitmap(struct super_block *sb, struct tfs_alloc_inode_info *tainfo);

#endif

