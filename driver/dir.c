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

static struct dentry *tfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
  struct page *page;
  int i, j;
  int numofpage;
  struct tfs_dentry *td;
  struct inode *inode;

  numofpage = (dir->i_size - 1 + PAGE_CACHE_SIZE) >> PAGE_CACHE_SHIFT;

  printk("TFS: tfs_lookup: %u\n", numofpage);

  for (i = 0; i < numofpage; ++i)
    {
      page = read_mapping_page(dir->i_mapping, i, NULL);
      if (IS_ERR(page))
	return ERR_PTR(-EIO);

      kmap(page);
      for (j = 0, td = (struct tfs_dentry *) page_address(page); j < (PAGE_CACHE_SIZE / sizeof(struct tfs_dentry)); ++j, ++td)
	{
	  if (!td->inode || td->len != dentry->d_name.len)
	    continue;

	  if (!memcmp(td->name, dentry->d_name.name, td->len))
	    {
	      int ino = td->inode;

	      kunmap(page);
	      page_cache_release(page);

	      inode = tfs_inode_get(dir->i_sb, ino);
	      if (IS_ERR(inode))
		return ERR_CAST(inode);

	      return d_splice_alias(inode, dentry);
	    }
	}

      kunmap(page);
      page_cache_release(page);
    }

  return d_splice_alias(NULL, dentry);
}


static int tfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
  struct inode *inode = file->f_path.dentry->d_inode;
  int npages = (inode->i_size - 1 + PAGE_CACHE_SIZE) >> PAGE_CACHE_SHIFT;
  int cpage = file->f_pos >> PAGE_CACHE_SHIFT;
  int offset = file->f_pos & ~PAGE_CACHE_MASK;
  int j, ret;
  struct tfs_dentry *td;
  char *addr;

  printk("TFS: tfs_readdir: %u, %u, %u\n", npages, cpage, offset);

  if (file->f_pos > inode->i_size - sizeof(struct tfs_dentry))
    return 0;

  for ( ; cpage < npages; ++cpage)
    {
      struct page *page = read_mapping_page(inode->i_mapping, cpage, NULL);
      if (IS_ERR(page))
	  return -EIO;

      kmap(page);
      addr = (char *)page_address(page) + offset;
      for (j = 0, td = (struct tfs_dentry *) addr; j <= (PAGE_CACHE_SIZE / sizeof(struct tfs_dentry)) - sizeof(struct tfs_dentry) && file->f_pos < inode->i_size; ++j, ++td)
	{
	  file->f_pos += sizeof(struct tfs_dentry);
	  if (td->type == DT_UNKNOWN)
	    continue;
	  offset = (char *) td  - addr;
	  ret = filldir(dirent, td->name, td->len, (cpage << PAGE_CACHE_SHIFT) | offset, td->inode, td->type);
	  if (ret)
	    {
	      kunmap(page);
	      page_cache_release(page);

	      return 0;
	    }
	}

      kunmap(page);
      page_cache_release(page);

      offset = 0;
    }

  return 0;
}

int tfs_new_default_dentry(struct inode *dir, struct inode *inode)
{
  struct page *page;
  struct tfs_dentry *td;
  unsigned long blocksize = inode->i_sb->s_blocksize;
  int err;
  char *addr;

  page = grab_cache_page(inode->i_mapping, 0);
  if (!page)
    {
      return -EIO;
    }

  err = __tfs_write_begin(NULL, inode->i_mapping, 0, blocksize, 0, &page, NULL);
  if (err)
    {
      unlock_page(page);
      goto err;
    }

  kmap(page);
  addr = (char *) page_address(page);
  memset(addr, 0, blocksize);

  td = (struct tfs_dentry *) addr;
  memset(td, 0, sizeof(*td));
  td->type = DT_DIR;
  td->inode = inode->i_ino;
  td->len = 1;
  strncpy(td->name, ".", 1);

  ++td;
  memset(td, 0, sizeof(*td));
  td->type = DT_DIR;
  td->inode = dir->i_ino;
  td->len = 2;
  strncpy(td->name, "..", 2);

  kunmap(page);
  
  err = tfs_commit_write(page, 0, blocksize);
  if (err)
    {
      goto err;
    }

  page_cache_release(page);

  return 0;

err:
  page_cache_release(page);
  return err;
}

int tfs_find_dentry(struct inode *dir, struct dentry *dentry, struct tfs_alloc_inode_info *tai)
{
  int npages = (dir->i_size + PAGE_CACHE_SIZE -1) >> PAGE_CACHE_SHIFT;
  int i, lastbyte_in_page;
  char *addr;
  struct page *page;
  struct tfs_dentry *td;

  tai->slot_page = -1;
  tai->slot_idx = 0;

  if (dentry->d_name.len > TFS_DENTRY_NAME_LEN)
    return -E2BIG;

  for (i = 0; i < npages; ++i)
    {
      page = read_mapping_page(dir->i_mapping, i, NULL);
      if (IS_ERR(page))
	continue;

      kmap(page);
      if (i != (npages - 1) || dir->i_size == PAGE_CACHE_SIZE)
	lastbyte_in_page = PAGE_CACHE_SIZE;
      else
	lastbyte_in_page = dir->i_size % PAGE_CACHE_SIZE;

      addr = page_address(page);
      for (td = (struct tfs_dentry *) addr; (char *) td < (addr + lastbyte_in_page); ++td)
	{
	  if (!td->inode)
	    {
	      if (tai->slot_page == -1)
		{
		  tai->slot_page = i;
		  tai->slot_idx = (char *) td - addr;
		}
	      continue;
	    }

	  if (td->len == dentry->d_name.len &&
	      !memcmp(td->name, dentry->d_name.name, td->len))
	    {
	      tai->slot_page = i;
	      tai->slot_idx = (char *) td - addr;
	      kunmap(page);
	      page_cache_release(page);
	      
	      return -EEXIST;
	    }
	}
      
      kunmap(page);
      page_cache_release(page);
    }

  if (tai->slot_page == -1)
    {
      return -ENOSPC;
    }

  return 0;
}

int tfs_set_link(struct inode *dir, struct inode *inode, struct dentry *dentry, int slot_page, int slot_idx)
{
  struct page *page;
  struct tfs_dentry *td;
  int err;

  err = tfs_write_begin(NULL, dir->i_mapping, (slot_page << PAGE_CACHE_SHIFT) | slot_idx, sizeof(struct tfs_dentry), 0, &page, NULL);
  if (err)
    return err;

  kmap(page);

  td = (struct tfs_dentry *) (page_address(page) + slot_idx);
  memset(td, 0, sizeof(struct tfs_dentry));
  if (S_ISDIR(inode->i_mode))
    td->type = DT_DIR;
  else if (S_ISREG(inode->i_mode))
    td->type = DT_REG;
  else if (S_ISFIFO(inode->i_mode))
    td->type = DT_FIFO;
  else if (S_ISCHR(inode->i_mode))
    td->type = DT_CHR;
  else if (S_ISBLK(inode->i_mode))
    td->type = DT_BLK;
  else if (S_ISLNK(inode->i_mode))
    td->type = DT_LNK;
  else if (S_ISSOCK(inode->i_mode))
    td->type = DT_SOCK;

  td->inode = inode->i_ino;
  td->len = dentry->d_name.len;
  strncpy(td->name, dentry->d_name.name, td->len);

  kunmap(page);

  err = tfs_commit_write(page, (slot_page << PAGE_CACHE_SHIFT) | slot_idx, sizeof(struct tfs_dentry));
  page_cache_release(page);

  if (!err)
    {
      dir->i_ctime = dir->i_mtime = CURRENT_TIME_SEC;
      mark_inode_dirty(dir);
    }
  
  return err;
}

int tfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
  struct inode *inode_new = NULL;
  struct tfs_alloc_inode_info tai;
  int err = -EIO;

  printk("TFS: tfs_mkdir: %u\n", (unsigned int) dir->i_ino);

  tfs_init_alloc_inode_info(tai);

  err = tfs_find_dentry(dir, dentry, &tai);
  if (err)
    {
      printk("TFS: error in tfs_find_dentry: %d\n", err);
      return err;
    }

  printk("TFS: tfs_find_dentry successful\n");

  inode_new = tfs_new_inode(dir, &tai, S_IFDIR | mode);
  if (!inode_new)
    {
      printk("TFS: error tfs_new_inode: %d\n", tai.err);
      return tai.err;
    }

  printk("TFS: tfs_new_inode successful\n");

  inode_inc_link_count(dir);

  err = tfs_new_default_dentry(dir, inode_new);
  if (err)
    {
      printk("TFS: error tfs_new_default_dentry: %d\n", err);
      goto err;
    }

  printk("TFS: tfs_new_default_dentry successful\n");

  err = tfs_set_link(dir, inode_new, dentry, tai.slot_page, tai.slot_idx);
  if (err)
    {
      printk("TFS: error tfs_set_link: %d\n", err);
      goto err_dec_inode;
    }

  printk("TFS: tfs_set_link successful\n");

  
  inode_inc_link_count(inode_new);
  d_instantiate(dentry, inode_new);
  tfs_release_inode_info_blocks(&tai);

  return 0;

err:
  inode_dec_link_count(dir);
  iput(inode_new);
  tfs_error_inode_info(&tai);

  return err;

err_dec_inode:
  inode_dec_link_count(inode_new);
  goto err;
}

int tfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
  struct tfs_alloc_inode_info tai;
  struct inode *inode_new = NULL;
  int err;

  printk("TFS: tfs_create: %u\n", (unsigned int) dir->i_ino);

  tfs_init_alloc_inode_info(tai);

  err = tfs_find_dentry(dir, dentry, &tai);
  if (err)
    {
      printk("TFS: error tfs_create in tfs_find_dentry\n");
      return -EIO;
    }

  inode_new = tfs_new_inode(dir, &tai, S_IFREG | mode);
  if (!inode_new)
    {
      printk("TFS: error in tfs_new_inode\n");
      goto err;
    }

  err = tfs_set_link(dir, inode_new, dentry, tai.slot_page, tai.slot_idx);
  if (err)
    {
      printk("TFS: error in tfs_set_link\n");
      goto err_dec_link;
    }

  d_instantiate(dentry, inode_new);
  tfs_release_inode_info_blocks(&tai);

  return 0;
 
err:
  tfs_error_inode_info(&tai);
  iput(inode_new);
  return err;

err_dec_link:
  inode_dec_link_count(inode_new);
  goto err;
}

int tfs_link(struct dentry *source_dentry, struct inode *dir, struct dentry *dentry)
{
  struct tfs_alloc_inode_info tai;
  struct inode *inode = source_dentry->d_inode;
  int err;

  printk("TFS: tfs_link: %u\n", (unsigned int) inode->i_ino);

  tfs_init_alloc_inode_info(tai);

  err = tfs_find_dentry(dir, source_dentry, &tai);
  if (err != -EEXIST)
    {
      printk("TFS: Could not find source dentry: %d\n", err);
      return err;
    }

  err = tfs_find_dentry(dir, dentry, &tai);
  if (err)
    {
      printk("TFS: link may already exist: %d\n", err);
      return err;
    }

  err = tfs_set_link(dir, inode, dentry, tai.slot_page, tai.slot_idx);
  if (err)
    {
      printk("TFS: error in tfs_set_link: %d\n", err);
      return err;
    }

  inode->i_ctime = CURRENT_TIME_SEC;
  inode_inc_link_count(inode);
  atomic_inc(&inode->i_count);
  mark_inode_dirty(inode);
  d_instantiate(dentry, inode);

  return 0;
}

struct inode_operations tfs_dir_inode_operations =
  {
    .create = tfs_create,
    .mkdir = tfs_mkdir,
    .lookup = tfs_lookup,
    .link = tfs_link
  };

struct file_operations tfs_dir_operations =
  {
    .llseek = tfs_llseek,
    .read = generic_read_dir,
    .readdir = tfs_readdir,
    .fsync = tfs_fsync
  };
