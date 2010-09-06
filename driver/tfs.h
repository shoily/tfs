#ifndef _TFS_H
#define _TFS_H

#include <linux/types.h>

#define TFS_MAGIC 0x1234
#define TFS_MAX_MNT_COUNT 100

#define TFS_SUPER_BLOCK 1
#define TFS_BLOCK_SIZE 1024
#define TFS_BLOCK_SIZE_BITS 10
#define TFS_INODE_SIZE 64
#define TFS_INODE_SIZE_BITS 6
#define TFS_INODE_PER_BLOCK (TFS_BLOCK_SIZE / TFS_INODE_SIZE)
#define TFS_DATA_BLOCKS_PER_INODE 4

#define TFS_ROOT_DIR_INODE 1
#define TFS_TMP_DIR_INODE 2

#ifndef __KERNEL__
#define u32 __u32
#endif


struct tfs_super_block
{
  u32 magic;

  u32 inode_bitmap_blocks;
  u32 data_bitmap_blocks;
  u32 inode_table_entries;
  u32 inode_table_blocks;
  u32 data_blocks_per_inode;
  u32 size;
  u32 mnt_count;
  u32 max_mnt_count;

  u32 inode_bitmap_block_start;
  u32 data_bitmap_block_start;
  u32 inode_table_block_start;

  u32 root_dir_data_block_start;
  u32 tmp_dir_data_block_start;
  u32 reserve_data_block_start;
  u32 data_block_start;
};

struct tfs_inode
{
  u32 mode;
  u32 uid;
  u32 gid;
  u32 ctime;
  u32 mtime;
  u32 atime;
  u32 hard_link_count;
  u32 size;
  u32 blocks;
  u32 data_blocks[TFS_DATA_BLOCKS_PER_INODE];
  u32 root_indirect_data_block;
  char pad[8];
};

#define TFS_DENTRY_NAME_LEN 20

struct tfs_dentry
{
  u32 type;
  u32 inode;
  u32 len;
  char name[TFS_DENTRY_NAME_LEN];
};

#endif
