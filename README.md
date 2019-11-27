Trivial File System

I'm writing a Linux based filesystem driver which is in active development stage. I've compiled and tested it in 2.6.28 kernel. Block size is now 1KB. It supports two level of indirect block. The root indirect block contains 256 entries (1024/4) of block number. Each of the second level indirect block contains 256 entries. First 4 blocks are embedded in the inode descriptor. So maximum file is as of now is 64MB+4KB. For larger file size, block size can be made 4KB by changing the constant (TFS_BLOCK_SIZE) in tfs.h. You need updated file system (myfs) for that. With 4K block size maximum file size is 4GB.

As of now, users can perform the following operations -
1. mount
2. unmount
3. ls
4. cat (both reading and writing to file)
5. mkdir
6. create file
7. ln (hard link)

I'm attaching a loop mountable filesystem image named 'myfs'. mkfs source code is not uploaded yet.

Instruction for compiling and mounting filesystem -
1. Go to driver directory and type 'make' to compile.
2. Type 'sudo insmod tfs.ko'.
3. Type 'sudo mount -t tfs myfs /mnt/dir -o loop' (here '/mnt/dir' is directory to mount the fs)
4. Access the file system in /mnt/dir directory.

