#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#ifdef USE_ZOPFLI
#include "zopfli/zopfli.h"
#include "zopfli/zlib_container.h"
#else
#include <zlib.h>
#endif
#include "squashfs.h"


//#define BLOCK_SIZE 131072 // 128KB
//#define FRAGMENT_SIZE BLOCK_SIZE
#define MDB_SIZE 8192
#define MAX_FRAGMENTS_PER_BLOCK 512
#define INITIAL_BLOCK_CAPACITY 100
#define INITIAL_DIR_ENTRIES 16


typedef struct nodeitem
{
    int type;
    union 
    {
        struct
        {
            const wchar_t* path; // file or symbol link
            uint64_t size;
        };
        struct
        {
            struct stringtable* paths; // dir
            uint32_t parentnode;
            struct {
                uint32_t node;
                uint16_t type;
                //uint16_t offset;
            } *childs;
        };
    };
    uint32_t nodenum;
} nodeitem;

typedef struct bytevec
{
    void* data;
    uint32_t align;
    size_t size;
    size_t cap;
} bytevec;

typedef struct stringtable
{
    char* data;
    int* indexes;
    size_t count;
    size_t size;
    size_t cap;
} stringtable;

size_t g_BLOCK_SIZE = 128*1024;
int g_opkfd;
int g_parent_inode = 0;
size_t g_block_offset;
uint64_t g_data_size;
nodeitem* g_nodes;
int g_nodesize = 0;
struct squashfs_super_block sb;

void save_data_blocks();
char* unicode_to_utf8(const wchar_t* source);
void add_to_stringtable(stringtable* table, const char* str);
void free_stringtable(stringtable* table);
void append_bytevec(bytevec* vec, const void* buf, size_t len);
void* alloc_bytevec(bytevec* vec, size_t len);
size_t compress_to_file(void* block, size_t blocksize, bool ismeta);
uint64_t compress_meta_blocks(void* buf, size_t len, bool withoffsets);

long generate_inode_num()
{
    static LONG inode_num = 0;
    return InterlockedIncrement(&inode_num);
}

nodeitem* new_nodeitem(int type, const wchar_t* path, uint64_t size)
{
    if (g_nodesize % 16 == 0) {
        g_nodes = (nodeitem*)realloc(g_nodes, sizeof(nodeitem) * (g_nodesize + 16));
    }
    nodeitem* item = &g_nodes[g_nodesize++];
    item->type = type;
    item->path = path;
    item->size = size; // parent + subs?
    return item;
}

void free_nodes()
{
    for (int i = 0; i < g_nodesize; i++) {
        nodeitem* item = &g_nodes[i];
        if (item->path) {
            if (item->type == SQUASHFS_DIR_TYPE) {
                free_stringtable(item->paths);
                free(item->childs);
            } else {
                free((void*)item->path);
            }
        }
    }
    free(g_nodes);
}

void append_bytevec(bytevec* vec, const void* buf, size_t len)
{
    if (vec->size + len > vec->cap) {
        size_t newcap = ((vec->size + len + vec->align - 1) / vec->align) * vec->align;
        void* newdata = realloc(vec->data, newcap);
        if (newdata == NULL) {
            perror("reallocation failed in bytevec");
            return;
        }
        vec->data = newdata;
        vec->cap = newcap;
    }
    memcpy((char*)vec->data + vec->size, buf, len);
    vec->size += len;
}

void* alloc_bytevec(bytevec* vec, size_t len)
{
    if (vec->size + len > vec->cap) {
        size_t newcap = ((vec->size + len + vec->align - 1) / vec->align) * vec->align;
        void* newdata = realloc(vec->data, newcap);
        if (newdata == NULL) {
            perror("reallocation failed in bytevec");
            return NULL;
        }
        vec->data = newdata;
        vec->cap = newcap;
        memset((char*)vec->data + vec->size, 0, newcap - vec->size);
    }
    void* ptr = (char*)vec->data + vec->size;
    vec->size += len;
    return ptr;
}

void add_to_stringtable(stringtable* table, const char* str)
{
    if (table->count % 16 == 0) {
        int* newindexes = (int*)realloc(table->indexes, sizeof(int) * (table->count + 16));
        if (newindexes == NULL) {
            perror("reallocation failed in bytevec");
            return;
        }
        table->indexes = newindexes;
    }
    size_t len = strlen(str) + 1;
    if (table->size + len > table->cap) {
        size_t newcap = ((table->size + len + 255) / 256) * 256;
        char* newdata = (char*)realloc(table->data, newcap);
        if (newdata == NULL) {
            perror("reallocation failed in bytevec");
            return;
        }
        table->data = newdata;
        table->cap = newcap;
    }
    memcpy(table->data + table->size, str, len);
    table->indexes[table->count] = table->size;
    table->size += len;
    table->count++;
}

void free_stringtable(stringtable* table)
{
    if (table->data) {
        free(table->data);
    }
    if (table->indexes) {
        free(table->indexes);
    }
}

typedef struct _REPARSE_DATA_BUFFER {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    struct {
        USHORT SubstituteNameOffset;
        USHORT SubstituteNameLength;
        USHORT PrintNameOffset;
        USHORT PrintNameLength;
        ULONG Flags;
        WCHAR PathBuffer[1];
    } SymbolicLinkReparseBuffer;
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

typedef struct sortbundle
{
    WCHAR cFileName[MAX_PATH];
    char inode_type;
    uint32_t inode_num;
    uint64_t nFileSize;
} sortbundle;

// TODO: 递归构建目录列表, 最后遍历扫描目录混合列表, 再qsort扁平列表
void accept_directory(const wchar_t* folder)
{
    WIN32_FIND_DATAW ffd;
    wchar_t* findpattern = (wchar_t*)malloc(2 * (wcslen(folder) + sizeof "\\*"));
    swprintf(findpattern, L"%s\\*", folder); // wcscpy+wcscat可能减少代码大小?
    HANDLE hFind = FindFirstFileW(findpattern, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        wprintf(L"Enum %s failed!\n", findpattern);
        free(findpattern);
        return;
    }
    free(findpattern);
    sortbundle* sortlist = NULL;
    int sortcount = 0;
    int old_parent = g_parent_inode;
    int cur_dir_inode = generate_inode_num();
    g_parent_inode = cur_dir_inode;
    do {
        if (sortcount % 16 == 0) {
            sortlist = (sortbundle*)realloc(sortlist, sizeof(sortbundle) * (sortcount + 16));
        }
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(ffd.cFileName, L".") && wcscmp(ffd.cFileName, L"..")) {
                sortbundle* item = &sortlist[sortcount++];
                memcpy(item->cFileName, ffd.cFileName, sizeof(ffd.cFileName));
                item->inode_type = SQUASHFS_DIR_TYPE;
                item->inode_num = generate_inode_num();
            }
        } else if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT && ffd.dwReserved0 == IO_REPARSE_TAG_SYMLINK) {
            sortbundle* item = &sortlist[sortcount++];
            memcpy(item->cFileName, ffd.cFileName, sizeof(ffd.cFileName));
            item->inode_type = SQUASHFS_SYMLINK_TYPE;
            item->inode_num = generate_inode_num();
        } else {
            LARGE_INTEGER filesize = {ffd.nFileSizeLow, ffd.nFileSizeHigh};
            g_data_size += filesize.QuadPart;
            sortbundle* item = &sortlist[sortcount++];
            memcpy(item->cFileName, ffd.cFileName, sizeof(ffd.cFileName));
            item->inode_type = SQUASHFS_REG_TYPE;
            item->inode_num = generate_inode_num();
            item->nFileSize = filesize.QuadPart;
        }
    } while (FindNextFileW(hFind, &ffd) != 0);
    FindClose(hFind);
    qsort(sortlist, sortcount, sizeof(sortbundle), (int(*)(const void*,const void*))wcscmp);
    stringtable* strtab = (stringtable*)calloc(1, sizeof(stringtable));
    for (int i = 0; i < sortcount; i++) {
        sortbundle* sitem = &sortlist[i];
        //wprintf(L"%s\n", sitem->cFileName);
        wchar_t* newpath = (wchar_t*)malloc(2 * (wcslen(folder) + wcslen(sitem->cFileName) + 2));
        swprintf(newpath, L"%s\\%s", folder, sitem->cFileName);
        if (sitem->inode_type == SQUASHFS_REG_TYPE) {
            nodeitem* nitem = new_nodeitem(SQUASHFS_REG_TYPE, newpath, sitem->nFileSize);
            nitem->nodenum = sitem->inode_num;
        } else if (sitem->inode_type == SQUASHFS_DIR_TYPE) {
            accept_directory(newpath);
            free(newpath); // diritem不使用newpath, 在此free, 别的newpath在main里free_nodes来free
        } else {
            HANDLE hFile = CreateFileW(newpath, 0, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                char buffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
                DWORD bytesReturned;
                if (DeviceIoControl(hFile, FSCTL_GET_REPARSE_POINT, NULL, 0, buffer, sizeof(buffer), &bytesReturned, NULL)) {
                    REPARSE_DATA_BUFFER* reparseData = (REPARSE_DATA_BUFFER*)buffer;
                    if (reparseData->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
                        WCHAR* linkTarget = reparseData->SymbolicLinkReparseBuffer.PathBuffer +
                            (reparseData->SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(WCHAR));
                        DWORD linkTargetLength = reparseData->SymbolicLinkReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
                        char* utf8 = unicode_to_utf8(linkTarget);
                        nodeitem* nitem = new_nodeitem(SQUASHFS_SYMLINK_TYPE, (wchar_t*)unicode_to_utf8(linkTarget), strlen(utf8));
                    }
                }
                CloseHandle(hFile);
            }
        }
        // 短名称
        char* utf8 = unicode_to_utf8(sitem->cFileName);
        add_to_stringtable(strtab, utf8);
        free(utf8);
    }
    // add self to nodelist
    nodeitem* diritem = new_nodeitem(SQUASHFS_DIR_TYPE, NULL, 0);
    diritem->nodenum = cur_dir_inode;
    diritem->parentnode = old_parent;
    diritem->paths = strtab; // used later in fragment table generation
#ifdef __cplusplus
    diritem->childs = (decltype(diritem->childs))malloc(sizeof(*diritem->childs) * sortcount);
#else
    diritem->childs = malloc(sizeof(*diritem->childs) * sortcount);
#endif
    for (int i = 0; i < sortcount; i++) {
        diritem->childs[i].node = sortlist[i].inode_num;
        diritem->childs[i].type = sortlist[i].inode_type;
    }
    free(sortlist);
}

int wmain(int argc, wchar_t ** argv)
{
    if (argc != 3) {
        printf("Usage: opack <input_directory> <output_file>\n");
        return 1;
    }
    g_opkfd = _wopen(argv[2], O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, _S_IWRITE);
    if (g_opkfd == -1) {
        perror("Error opening output file");
        return 1;
    }

    // 跳过superblock
    _lseek(g_opkfd, sizeof(struct squashfs_super_block), SEEK_SET);
    g_block_offset = sizeof(struct squashfs_super_block);

    // 首先扫描目录
    accept_directory(argv[1]);
    save_data_blocks();

    free_nodes();

    sb.s_magic = SQUASHFS_MAGIC; // 'sqsh';
    sb.s_major = SQUASHFS_MAJOR;
    sb.s_minor = SQUASHFS_MINOR;
    sb.flags = SQUASHFS_DUPLICATES; // 0x40
    sb.block_size = g_BLOCK_SIZE;
    sb.block_log = 17;
    sb.compression = ZLIB_COMPRESSION; // 1
    sb.no_ids = 1;
    sb.lookup_table_start = -1;
    sb.xattr_id_table_start = -1;
    sb.bytes_used = g_block_offset;
    _lseek(g_opkfd, 0, SEEK_SET);
    _write(g_opkfd, &sb, sizeof(sb)); // 更新superblock

    _chsize(g_opkfd, ((g_block_offset + 4095) / 4096) * 4096); // 按照4K对齐

    _close(g_opkfd);
}

size_t compress_to_file(void* block, size_t blocksize, bool ismeta)
{
#ifdef USE_ZOPFLI
    ZopfliOptions options;
    ZopfliInitOptions(&options);
    options.numiterations = 15;
    unsigned char* zblock = NULL;
    size_t zsize = 0;
    ZopfliZlibCompress(&options, (unsigned char*)block, blocksize, &zblock, &zsize);
    if (zblock && zsize < blocksize) {
#else
    uLong zsize = compressBound(blocksize);
    void* zblock = malloc(zsize);
    int ret = compress2(zblock, &zsize, block, blocksize, Z_BEST_COMPRESSION); //compressData(block, g_BLOCK_SIZE, &zblock, &zsize);
    if (ret == Z_OK && zsize < blocksize) {
#endif
        if (ismeta) {
            _write(g_opkfd, &zsize, sizeof(uint16_t)); // little endian
        }
        _write(g_opkfd, zblock, zsize);
        g_block_offset += zsize + (ismeta?2:0);
    } else {
        if (ismeta) {
            // max 0x2000
            uint16_t size = (uint16_t)blocksize | 0x8000;
            _write(g_opkfd, &size, sizeof(uint16_t));
        }
        _write(g_opkfd, block, blocksize);
        g_block_offset += blocksize + (ismeta?2:0);
        zsize = blocksize;
    }
    free(zblock);
    return zsize;
}

uint64_t compress_meta_blocks(void* buf, size_t len, bool withoffsets)
{
    uint64_t* offsets = 0;
    int blockcnt = (len + MDB_SIZE - 1) / MDB_SIZE;
    if (withoffsets) {
        offsets = (uint64_t*)malloc(sizeof(uint64_t) * blockcnt);
    }
    for (int i = 0; i < blockcnt; i++) {
        size_t blocksize = len >= MDB_SIZE ? MDB_SIZE : len;
        if (withoffsets) {
            offsets[i] = g_block_offset;
        }
        compress_to_file((char*)buf + i * MDB_SIZE, blocksize, true);
        len -= blocksize;
    }
    uint64_t offsetsoffset = g_block_offset;
    if (withoffsets) {
        _write(g_opkfd, offsets, sizeof(uint64_t) * blockcnt);
        g_block_offset += sizeof(uint64_t) * blockcnt;
        free(offsets);
    }
    return offsetsoffset;
}

bytevec* pre_compress_meta_blocks(bytevec* src, uint32_t* offsets)
{
    bytevec* dest = (bytevec*)calloc(1, sizeof(bytevec));
    dest->align = MDB_SIZE;
    int blockcnt = (src->size + MDB_SIZE - 1) / MDB_SIZE;
    unsigned char* block = (unsigned char*)src->data;
    int len = src->size;
    for (int i = 0; i < blockcnt; i++, block += MDB_SIZE) {
        size_t blocksize = len >= MDB_SIZE ? MDB_SIZE : len;
#ifdef USE_ZOPFLI
        ZopfliOptions options;
        ZopfliInitOptions(&options);
        options.numiterations = 15;
        unsigned char* zblock = NULL;
        size_t zsize = 0;
        offsets[i] = dest->size;
        ZopfliZlibCompress(&options, block, blocksize, &zblock, &zsize);
        if (zblock && zsize < blocksize) {
#else
        uLong zsize = compressBound(blocksize);
        void* zblock = malloc(zsize);
        offsets[i] = dest->size;
        int ret = compress2(zblock, &zsize, block, blocksize, Z_BEST_COMPRESSION);
        if (ret == Z_OK && zsize < blocksize) {
#endif
            *(uint16_t*)alloc_bytevec(dest, sizeof(uint16_t)) = (uint16_t)zsize; // little endian
            append_bytevec(dest, zblock, zsize);
        } else {
            // max 0x2000
            *(uint16_t*)alloc_bytevec(dest, sizeof(uint16_t)) = (uint16_t)blocksize | 0x8000;
            append_bytevec(dest, block, blocksize);
        }
        free(zblock);

        len -= blocksize;
    }

    return dest;
}

typedef struct compresstask
{
    void* block;
    size_t blocksize;
    void* zblock;
    size_t zsize;
} compresstask;

unsigned __stdcall compresstask_proc(void* arg)
{
    compresstask* task = (compresstask*)arg;
#ifdef USE_ZOPFLI
    ZopfliOptions options;
    ZopfliInitOptions(&options);
    options.numiterations = 15;
    unsigned char* zblock = NULL;
    size_t zsize = 0;
    ZopfliZlibCompress(&options, (unsigned char*)task->block, task->blocksize, &zblock, &zsize);
    if (zblock && zsize < task->blocksize) {
#else
    uLong zsize = compressBound(blocksize);
    void* zblock = malloc(zsize);
    int ret = compress2(zblock, &zsize, block, blocksize, Z_BEST_COMPRESSION);
    if (ret == Z_OK && zsize < blocksize) {
#endif
        task->zblock = zblock;
        task->zsize = zsize;
    } else {
        free(zblock);
        task->zblock = NULL;
        task->zsize = 0;
    }
    return 0;
}

void save_data_blocks()
{
    bytevec fragblocks = {NULL, g_BLOCK_SIZE};
    bytevec inodetable = {NULL, MDB_SIZE};
    bytevec dirtable = {NULL, MDB_SIZE};
    bytevec fixuptable = {NULL, 64};
    typedef struct fixpair {
        uint32_t index;
        uint32_t* pstart_block;
    } fixpair;
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    uint32_t num_cores = sysInfo.dwNumberOfProcessors;
    uint16_t* nodeoffsets = (uint16_t*)malloc(sizeof(uint16_t) * g_nodesize); // 给dir entry用
    for (int i = 0; i < g_nodesize; i++) {
        nodeitem* item = &g_nodes[i];
        if (item->type == SQUASHFS_REG_TYPE) {
#ifdef _INC_CRTDEFS
            int fd = _wopen(item->path, O_RDONLY | O_BINARY, 0); // WDK
#else
            int fd = _wopen(item->path, O_RDONLY | O_BINARY); // posix
#endif
            if (fd == -1) {
                free((void*)item->path);
                item->type = 0;
                continue;
            }
            size_t blockcnt = item->size / g_BLOCK_SIZE; // 512T
            nodeoffsets[item->nodenum] = inodetable.size;
            struct squashfs_reg_inode* inode = (struct squashfs_reg_inode*)alloc_bytevec(&inodetable, sizeof(struct squashfs_reg_inode) + blockcnt * sizeof(uint32_t));
            inode->header.inode_type = SQUASHFS_REG_TYPE;
            inode->header.inode_number = item->nodenum;
            inode->start_block = g_block_offset; // 写入当前文件之前的ftell
            inode->file_size = item->size; // 大于4G要用lreg
            // TODO: 多线程压缩, 顺序写入(WaitMultipleObject)
            if (blockcnt) {
                compresstask* tasks = (compresstask*)malloc(sizeof(compresstask)*num_cores);
                HANDLE* threads = (HANDLE*)malloc(sizeof(HANDLE)*num_cores);
                char* blocks = (char*)malloc(g_BLOCK_SIZE * num_cores);
                wprintf(L"Compressing %s", item->path);
                printf(", %u block\n", blockcnt);
                for (size_t j = 0; j < blockcnt; j += num_cores) {
                    size_t runcnt = min(num_cores, blockcnt - j);
                    _read(fd, blocks, g_BLOCK_SIZE * runcnt);
                    for (size_t k = 0; k < runcnt; k++) {
                        tasks[k].block = blocks + k * g_BLOCK_SIZE;
                        tasks[k].blocksize = g_BLOCK_SIZE;
                        threads[k] = (HANDLE)_beginthreadex(NULL, 0, compresstask_proc, &tasks[k], 0, NULL);
                    }
                    //WaitForMultipleObjects(runcnt, threads, TRUE, INFINITE);
                    for (size_t k = 0; k < runcnt; k++) {
                        WaitForSingleObject(threads[k], INFINITE);
                        CloseHandle(threads[k]);
                        if (tasks[k].zblock) {
                            g_block_offset += _write(g_opkfd, tasks[k].zblock, tasks[k].zsize);
                            free(tasks[k].zblock);
                        } else {
                            g_block_offset += _write(g_opkfd, tasks[k].block, g_BLOCK_SIZE);
                        }
                        inode->blocks[j + k] = (tasks[k].zblock) ? tasks[k].zsize : (g_BLOCK_SIZE | (1 << 24));
                    }
                }
                free(blocks);
                free(threads);
                free(tasks);
            }
            size_t fragtail = item->size % g_BLOCK_SIZE;
            if (fragtail) {
                wprintf(L"Append %s, %u", item->path, fragtail);
                printf(" to fragments.\n");
                void* block = malloc(g_BLOCK_SIZE);
                _read(fd, block, fragtail);
                inode->fragment = fragblocks.size / g_BLOCK_SIZE;
                inode->offset = fragblocks.size % g_BLOCK_SIZE;
                append_bytevec(&fragblocks, block, fragtail);
                free(block);
            } else {
                inode->fragment = -1;
                //inode->offset = 0;
            }
            _close(fd);
        }
        if (item->type == SQUASHFS_SYMLINK_TYPE) {
            nodeoffsets[item->nodenum] = inodetable.size;
            size_t linklen = (size_t)item->size; // utf8 count
            struct squashfs_symlink_inode* inode = (struct squashfs_symlink_inode*)alloc_bytevec(&inodetable, sizeof(struct squashfs_symlink_inode) + linklen);
            inode->header.inode_type = SQUASHFS_SYMLINK_TYPE;
            inode->header.inode_number = item->nodenum;
            inode->symlink_size = linklen;
            memcpy(inode->symlink, item->path, linklen); // utf8 content
        }
        if (i == g_nodesize - 1) {
            sb.root_inode = inodetable.size;
        }
        if (item->type == SQUASHFS_DIR_TYPE) {
            size_t metablockpos = inodetable.size;
            nodeoffsets[item->nodenum] = inodetable.size;
            struct squashfs_dir_inode* inode = (struct squashfs_dir_inode*)alloc_bytevec(&inodetable, sizeof(struct squashfs_dir_inode));
            inode->header.inode_type = SQUASHFS_DIR_TYPE;
            inode->header.inode_number = item->nodenum;
            inode->file_size = sizeof(struct squashfs_dir_header) + sizeof(struct squashfs_dir_entry) * item->paths->count + (item->paths->size - item->paths->count) + 3;
            inode->nlink = 2; // historical . ..
            // 仅为非空目录生成目录内容列表(header+entries)
            if (item->paths->count) {
                //inode->start_block = (dirtable.size / MDB_SIZE) * MDB_SIZE; // 需要是dir header所在metablock压缩后偏移
                fixpair* fix = (fixpair*)alloc_bytevec(&fixuptable, sizeof(fixpair));
                fix->index = dirtable.size / MDB_SIZE;
                fix->pstart_block = &inode->start_block;
                inode->offset = dirtable.size % MDB_SIZE;
                // prepare directory header and entries
                struct squashfs_dir_header* header = (struct squashfs_dir_header*)alloc_bytevec(&dirtable, sizeof(struct squashfs_dir_header));
                uint32_t startnode = item->childs[0].node;
                header->count = item->paths->count - 1; // TODO: 每个header最多256记录, 多了要拆多个header
                header->inode_number = startnode; // 并非目录inode
                header->start_block = (metablockpos / MDB_SIZE) * MDB_SIZE; // 明文偏移, 不需压缩后回写?
                for (size_t j = 0; j <= header->count; j++) {
                    const char* filename = &item->paths->data[item->paths->indexes[j]];
                    size_t namelen = strlen(filename);
                    struct squashfs_dir_entry* entry = (struct squashfs_dir_entry*)alloc_bytevec(&dirtable, sizeof(struct squashfs_dir_entry) + namelen);
                    memcpy(entry->name, filename, namelen);
                    entry->type = item->childs[j].type;
                    entry->size = namelen - 1;
                    entry->inode_number = item->childs[j].node - startnode; // node差值不能超过+-32768, 超过要拆header
                    entry->offset = nodeoffsets[item->childs[j].node]; // 对应的node加入时的inodevec.size, 能在accept directory时候放在g_nodes里吗
                }
            }
        }
    }
    free(nodeoffsets);
    // 将囤积的碎片写入磁盘
    // TODO: 跟压缩datablock逻辑合并
    size_t fragsize = fragblocks.size;
    size_t fragcnt = (fragblocks.size + g_BLOCK_SIZE - 1) / g_BLOCK_SIZE;
    bytevec fragtable = { NULL, MDB_SIZE };
    if (fragsize) {
        printf("Compressing %d fragments\n", fragcnt);
        for (size_t j = 0; j < fragcnt; j++) {
            struct squashfs_fragment_entry* entry = (struct squashfs_fragment_entry*)alloc_bytevec(&fragtable, sizeof(struct squashfs_fragment_entry));
            size_t blocksize = fragsize >=g_BLOCK_SIZE?g_BLOCK_SIZE:fragsize;
            entry->start_block = g_block_offset;
            entry->size = compress_to_file((char*)fragblocks.data + j * g_BLOCK_SIZE, blocksize, false);
            fragsize -= blocksize;
        }
        free(fragblocks.data);
    }
    // 预压缩directory table, 再更新dir inode的start_block
    uint32_t* zdirtablestarts = (uint32_t*)malloc(sizeof(uint32_t)*(dirtable.size + MDB_SIZE - 1)/MDB_SIZE);
    bytevec* zdirtable = pre_compress_meta_blocks(&dirtable, zdirtablestarts);
    free(dirtable.data); // dirtable用不到了
    fixpair* fix = (fixpair*)fixuptable.data;
    for (int i = fixuptable.size / sizeof(fixpair); i > 0; i--) {
        *fix->pstart_block = zdirtablestarts[fix->index]; // 将压缩后block头部位置回写
    }
    free(zdirtablestarts);
    free(fixuptable.data);
    // save node table
    sb.inodes = g_nodesize;
    sb.inode_table_start = g_block_offset;
    compress_meta_blocks(inodetable.data, inodetable.size, false);
    free(inodetable.data);
    // save directory table
    sb.directory_table_start = g_block_offset;
    //compress_meta_blocks(dirtable.data, dirtable.size, false);
    //free(dirtable.data);
    _write(g_opkfd, zdirtable->data, zdirtable->size);
    g_block_offset += zdirtable->size;
    free(zdirtable->data);
    free(zdirtable);
    // save fragment table
    if (fragtable.data) {
        sb.fragments = fragcnt;
        sb.fragment_table_start = compress_meta_blocks(fragtable.data, fragtable.size, true);
        //compress_meta_blocks(fragtab.data, fragtab.size, true);
        //sb.fragment_table_start = g_block_offset - fragcnt * sizeof(uint64_t);
        free(fragtable.data);
    } else {
        sb.fragments = -1;
        //sb.fragment_table_start = 0;
    }
    // save dummy IDs table
    uint32_t ID = 0;
    sb.id_table_start = compress_meta_blocks(&ID, sizeof(ID), true);
}

char* unicode_to_utf8(const wchar_t* source)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, source, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)malloc(len + 1);
    WideCharToMultiByte(CP_UTF8, 0, source, -1, utf8, len, NULL, NULL);
    return utf8;
}