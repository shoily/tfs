#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mount.h>
#include <linux/seq_file.h>

#include "tfs_module.h"

MODULE_AUTHOR("Shoily Obaidur Rahman - shoily@gmail.com");
MODULE_DESCRIPTION("Trivial Filesystem");
MODULE_LICENSE("GPL");

static struct super_operations tfs_sops;
static struct kmem_cache *tfs_inode_cachep;

static int tfs_fill_super(struct super_block *sb, void *data, int silent)
{
  struct tfs_super_block *tfs_sb;
  struct tfs_sb_info *si = NULL;
  struct buffer_head *bh = NULL;
  struct inode *root_inode = NULL;
  int blocksize;
  int ret;

  printk("TSF: tfs_fill_super\n");

  blocksize = sb_min_blocksize(sb, TFS_BLOCK_SIZE);

  if (blocksize != TFS_BLOCK_SIZE)
    {
      printk("TFS: unable set block size to %d\n", TFS_BLOCK_SIZE);
      return -1;
    }

  if (!(bh = sb_bread(sb, TFS_SUPER_BLOCK)))
    {
      printk("TFS: unable to read super block\n");
      return -1;
    }

  si = kzalloc(sizeof(struct tfs_sb_info), GFP_KERNEL);
  if (!si)
    {
      printk("TFS: unable to allocate memory for super block structure\n");
      ret = -ENOMEM;
      goto err_sb;
    }

  tfs_sb = (struct tfs_super_block *) ((char *) bh->b_data);
  
  if (tfs_sb->magic != TFS_MAGIC)
    {
      printk("TFS: Magic does not match - not a TFS file system\n");
      ret = -EINVAL;
      goto err_sb;
    }

  printk("TFS: magic number: %x\n", tfs_sb->magic);

  si->super_block = tfs_sb;
  si->bh = bh;

  tfs_sb->mnt_count++;

  mark_buffer_dirty(bh);
  sync_dirty_buffer(bh);

  sb->s_magic = tfs_sb->magic;
  sb->s_maxbytes = tfs_sb->size;
  sb->s_fs_info = si;
  sb->s_op = &tfs_sops;

  root_inode = tfs_inode_get(sb, TFS_ROOT_DIR_INODE);
  if (IS_ERR(root_inode))
    {
      printk("TFS: error getting root inode\n");
      ret = -EINVAL;
      goto err_sb;
    }

  sb->s_root = d_alloc_root(root_inode);
  if (!sb->s_root)
    {
      printk("TFS: error allocating root dentry\n");
      iput(root_inode);
      ret = -ENOMEM;
      goto err_sb;
    }

  mutex_init(&si->inode_bitmap_mutex);
  mutex_init(&si->data_bitmap_mutex);

  printk("TFS: tfs_fill_super successful\n");

  return 0;

err_sb:
  if (si)
    kfree(si);
  if (bh)
    brelse(bh);
    
  return ret;
}

static int tfs_get_sb(struct file_system_type *fs_type, int flags, const char *dev_name, void *data, struct vfsmount *mnt)
{
  return get_sb_bdev(fs_type, flags, dev_name, data, tfs_fill_super, mnt);
}

static void init_once(void *foo)
{
  struct tfs_inode_info *ti = (struct tfs_inode_info *) foo;
  int i;

  mutex_init(&ti->cached_block_mutex);
  for (i = 0; i < TFS_BLK_GRP; ++i)
    seqlock_init(&ti->cached_block_seqlocks[i]);

  inode_init_once(&ti->inode);
}

static void tfs_sync_super(struct super_block *sb)
{
  struct tfs_sb_info *si = sb->s_fs_info;

  mark_buffer_dirty(si->bh);
  sync_dirty_buffer(si->bh);
  sb->s_dirt = 0;
}

static struct inode *tfs_alloc_inode(struct super_block *sb)
{
  struct tfs_inode_info *ti = (struct tfs_inode_info *) kmem_cache_alloc(tfs_inode_cachep, GFP_KERNEL);
  printk("TFS: tfs_alloc_inode: %p\n", ti);

  if (!ti)
    return NULL;

  return &ti->inode;
}

void tfs_destroy_inode(struct inode *inode)
{
  struct tfs_inode_info *ti;

  printk("TFS: tfs_destroy_inode: %u\n", (unsigned int) inode->i_ino);

  ti = TFS_INODE(inode);
  kmem_cache_free(tfs_inode_cachep, ti);
}

static int tfs_write_inode(struct inode *inode, int wait)
{
  struct tfs_inode_info *tinfo = TFS_INODE(inode);
  int ino = inode->i_ino;
  unsigned int block, offset, shift;
  struct super_block *sb = inode->i_sb;
  struct tfs_sb_info *si = sb->s_fs_info;
  struct tfs_inode *ti;
  struct buffer_head *bh;
  int i;

  printk("TFS: tfs_write_inode: %u, %d\n", (unsigned int) inode->i_ino, wait);

  shift = (ino << TFS_INODE_SIZE_BITS);
  block = si->super_block->inode_table_block_start + (shift >> TFS_BLOCK_SIZE_BITS);
  offset = shift % TFS_BLOCK_SIZE;
  if (!(bh = sb_bread(sb, block)))
    {
      return -EIO;
    }

  ti = (struct tfs_inode *) (bh->b_data + offset);

  ti->mode = inode->i_mode;
  ti->uid = inode->i_uid;
  ti->gid = inode->i_gid;
  ti->ctime = inode->i_ctime.tv_sec;
  ti->mtime = inode->i_mtime.tv_sec;
  ti->atime = inode->i_atime.tv_sec;
  ti->hard_link_count = inode->i_nlink;
  ti->size = inode->i_size;
  ti->blocks = inode->i_blocks;
  memset(ti->pad, 0, sizeof(ti->pad));
  for (i = 0; i < TFS_DATA_BLOCKS_PER_INODE; ++i)
    ti->data_blocks[i] = tinfo->data_blocks[i];
  ti->root_indirect_data_block = tinfo->root_indirect_data_block;

  mark_buffer_dirty(bh);
  if (wait)
    {
      sync_dirty_buffer(bh);
      if (buffer_req(bh) && !buffer_uptodate(bh))
	{
	  printk("TFS: error syncing buffer\n");
	  brelse(bh);
	  return -EIO;
	}
    }

  brelse(bh);

  return 0;
}

static void tfs_put_super(struct super_block *sb)
{
  struct tfs_sb_info *si;

  printk("TFS: tfs_put_super\n");

  si = sb->s_fs_info;
  sb->s_fs_info = NULL;

  mark_buffer_dirty(si->bh);
  sync_dirty_buffer(si->bh);

  brelse(si->bh);
  mutex_destroy(&si->inode_bitmap_mutex);
  mutex_destroy(&si->data_bitmap_mutex);
  kfree(si);
}

static void tfs_write_super(struct super_block *sb)
{
  printk("TFS: tfs_write_super\n");

  tfs_sync_super(sb);
}

static void tfs_delete_inode(struct inode *inode)
{
  printk("TFS: tfs_delete_inode\n");
}

static void tfs_clear_inode(struct inode *inode)
{
  printk("TFS: tfs_clear_inode\n");
}

static int tfs_statfs(struct dentry *dentry, struct kstatfs *statfs)
{
  printk("TFS: tfs_statfs\n");
  return 0;
}

static int tfs_show_options(struct seq_file *seqfile, struct vfsmount *mnt)
{
  struct super_block *sb = mnt->mnt_sb;
  struct tfs_sb_info *si = sb->s_fs_info;
  struct tfs_super_block *tsb = si->super_block;

  seq_printf(seqfile, "TFS: inode bitmap blocks=%u\n", (unsigned int) tsb->inode_bitmap_blocks);

  return 0;
}


static struct super_operations tfs_sops =
  {
    .alloc_inode = tfs_alloc_inode,
    .destroy_inode = tfs_destroy_inode,
    .write_inode = tfs_write_inode,
    .delete_inode = tfs_delete_inode,
    .put_super = tfs_put_super,
    .write_super = tfs_write_super,
    .statfs = tfs_statfs,
    .clear_inode = tfs_clear_inode,
    .show_options = tfs_show_options
  };


static struct file_system_type tfs_type =
  {
    .owner = THIS_MODULE,
    .name = "tfs",
    .get_sb = tfs_get_sb,
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV
  };

static __init int init_tfs(void)
{
  int ret;

  printk("TFS: THIS_MODULE, module_core: %p, %p\n", THIS_MODULE, THIS_MODULE->module_core);

  tfs_inode_cachep = kmem_cache_create("tfs_inode_cache", sizeof(struct tfs_inode_info), 0, SLAB_RECLAIM_ACCOUNT, init_once);
  if (!tfs_inode_cachep)
    {
      ret = -ENOMEM;
      goto out;
    }

  ret = register_filesystem(&tfs_type);

 out:
  return ret;
}

static void __exit exit_tfs(void)
{
  unregister_filesystem(&tfs_type);
  kmem_cache_destroy(tfs_inode_cachep);
}


module_init(init_tfs)
module_exit(exit_tfs)
