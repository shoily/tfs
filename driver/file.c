#include "tfs_module.h"

void tfs_truncate(struct inode *inode)
{
  printk("TFS: tfs_truncate: %u\n", (unsigned int) inode->i_ino);

  block_truncate_page(inode->i_mapping, inode->i_size, tfs_getblocks);
  inode->i_mtime = CURRENT_TIME_SEC;
  mark_inode_dirty(inode);
  if (inode_needs_sync(inode))
    {
      tfs_sync_inode(inode);
    }
}

struct file_operations tfs_file_operations =
  {
    .read = do_sync_read,
    .write = do_sync_write,
    .aio_read = generic_file_aio_read,
    .aio_write = generic_file_aio_write,
    .llseek = tfs_llseek,
    .fsync = tfs_fsync
  };

struct inode_operations tfs_file_inode_operations =
  {
    .truncate = tfs_truncate
  };
