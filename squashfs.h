#ifndef SQUASHFS_H
#define SQUASHFS_H

#include <stdint.h>

#define SQUASHFS_MAGIC 0x73717368
#define SQUASHFS_MAJOR 4
#define SQUASHFS_MINOR 0
#define SQUASHFS_DEVBLK_SIZE 0x20000
#define SQUASHFS_COMPRESSED_BIT 0x80000000

#define SQUASHFS_UNCOMPRESSED_INODES    0x0001
#define SQUASHFS_UNCOMPRESSED_DATA	    0x0002
#define SQUASHFS_CHECK                  0x0004
#define SQUASHFS_UNCOMPRESSED_FRAGMENTS 0x0008
#define SQUASHFS_NO_FRAGMENTS           0x0010
#define SQUASHFS_ALWAYS_FRAGMENTS       0x0020
#define SQUASHFS_DUPLICATES             0x0040
#define SQUASHFS_EXPORTABLE             0x0080
#define SQUASHFS_UNCOMPRESSED_XATTRS    0x0100
#define SQUASHFS_NO_XATTRS              0x0200
#define SQUASHFS_COMPRESSOR_OPTIONS     0x0400

#define SQUASHFS_DIR_TYPE       1
#define SQUASHFS_REG_TYPE       2
#define SQUASHFS_SYMLINK_TYPE   3
#define SQUASHFS_BLKDEV_TYPE    4
#define SQUASHFS_CHRDEV_TYPE    5
#define SQUASHFS_FIFO_TYPE      6
#define SQUASHFS_SOCKET_TYPE    7
#define SQUASHFS_LDIR_TYPE      8
#define SQUASHFS_LREG_TYPE      9
#define SQUASHFS_LSYMLINK_TYPE  10
#define SQUASHFS_LBLKDEV_TYPE   11
#define SQUASHFS_LCHRDEV_TYPE   12
#define SQUASHFS_LFIFO_TYPE     13
#define SQUASHFS_LSOCKET_TYPE   14

#define SQUASHFS_DIR_INODE_NUMBER 1

#define ZLIB_COMPRESSION	1
#define LZMA_COMPRESSION	2
#define LZO_COMPRESSION		3
#define XZ_COMPRESSION		4
#define LZ4_COMPRESSION		5
#define ZSTD_COMPRESSION	6

struct squashfs_super_block {
    uint32_t s_magic;
    uint32_t inodes;
    uint32_t mkfs_time;
    uint32_t block_size;
    uint32_t fragments;
    uint16_t compression;
    uint16_t block_log;
    uint16_t flags;
    uint16_t no_ids;
    uint16_t s_major;
    uint16_t s_minor;
    uint64_t root_inode;
    uint64_t bytes_used;
    uint64_t id_table_start;
    uint64_t xattr_id_table_start;
    uint64_t inode_table_start;
    uint64_t directory_table_start;
    uint64_t fragment_table_start;
    uint64_t lookup_table_start;
};

struct squashfs_dir_index {
    uint32_t index;
    uint32_t start_block;
    uint32_t size;
    uint8_t name[0];
};

struct squashfs_inode_header {
    uint16_t inode_type;
    uint16_t mode;
    uint16_t uid;
    uint16_t guid;
    uint32_t mtime;
    uint32_t inode_number;
};

struct squashfs_symlink_inode {
    struct squashfs_inode_header header;
    uint32_t nlink; // 连接到该符号链接的hardlink
    uint32_t symlink_size;
    uint8_t symlink[0];
};

struct squashfs_reg_inode {
    struct squashfs_inode_header header;
    uint32_t start_block;
    uint32_t fragment;
    uint32_t offset;
    uint32_t file_size;
    uint32_t blocks[0];
};

struct squashfs_lreg_inode {
    struct squashfs_inode_header header;
    uint64_t start_block;
    uint64_t file_size;
    uint64_t sparse;
    uint32_t nlink;
    uint32_t fragment;
    uint32_t offset;
    uint32_t xattr;
    uint32_t blocks[0];
};

struct squashfs_dir_inode {
    struct squashfs_inode_header header;
    uint32_t start_block; // dir header的metablock, 压缩后起始位置
    uint32_t nlink;
    uint16_t file_size;
    uint16_t offset; // dir header的明文偏移
    uint32_t parent_inode;
};

struct squashfs_dir_entry {
    uint16_t offset; // inode偏移, 基于header指定的metablock明文
    int16_t inode_number;
    uint16_t type;
    uint16_t size;
    char name[0];
};

struct squashfs_dir_header {
    uint32_t count; // 不可大于256个
    uint32_t start_block; // 明文的metablock的相对位置
    uint32_t inode_number; // 可选, 它加entry的inode number得到目标inode number
};

struct squashfs_fragment_entry {
    uint64_t start_block;
    uint32_t size;
    uint32_t unused;
};

#endif // SQUASHFS_H