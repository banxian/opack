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

#ifdef _VERBOSE
#define verbose(fmt,...) printf(fmt, ##__VA_ARGS__)
#else
#define verbose(fmt,...)
#endif

typedef struct nodeitem
{
    int type;
    union {
        struct {
            const wchar_t* path; // file or symbol link
            uint64_t size;
        };
        struct {
            struct stringtable* paths; // dir
            union
            {
                uint32_t parentnodenum;
                uint32_t* parentnode;
            };
            struct {
                uint32_t node;
                uint16_t type;
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
    bool wide;
} stringtable;

size_t g_BLOCK_SIZE = 128*1024;
bool g_notailends = true;
int g_opkfd;
int g_root_inode;
size_t g_block_offset;
uint64_t g_data_size;
nodeitem* g_nodes;
int g_nodesize = 0;
struct squashfs_super_block sb;

void save_data_blocks();
char* unicode_to_utf8(const wchar_t* source);
void add_to_stringtable(stringtable* table, const void* str);
const void* pop_from_stringtable(stringtable* table); // revoke last item
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

long generate_inode_num2()
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

void add_to_stringtable(stringtable* table, const void* str)
{
    if (table->count % 16 == 0) {
        int* newindexes = (int*)realloc(table->indexes, sizeof(int) * (table->count + 16));
        if (newindexes == NULL) {
            perror("reallocation failed in bytevec");
            return;
        }
        table->indexes = newindexes;
    }
    size_t len = table->wide?((wcslen(str) + 1) * 2):(strlen(str) + 1);
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

const void* pop_from_stringtable(stringtable* table)
{
    int lastidx = table->indexes[--table->count];
    table->size = lastidx;
    return &table->data[lastidx];
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
int accept_directory(const wchar_t* folder, uint32_t parent)
{
    WIN32_FIND_DATAW ffd;
    wchar_t* findpattern = (wchar_t*)malloc(2 * (wcslen(folder) + sizeof "\\*"));
    swprintf(findpattern, L"%s\\*", folder); // wcscpy+wcscat可能减少代码大小?
    HANDLE hFind = FindFirstFileW(findpattern, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        wprintf(L"Enum %s failed!\n", findpattern);
        free(findpattern);
        return -1;
    }
    free(findpattern);
    sortbundle* sortlist = NULL;
    int sortcount = 0;
    do {
        if (sortcount % 16 == 0) {
            sortlist = (sortbundle*)realloc(sortlist, sizeof(sortbundle) * (sortcount + 16));
        }
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(ffd.cFileName, L".") && wcscmp(ffd.cFileName, L"..")) {
                sortbundle* item = &sortlist[sortcount++];
                memcpy(item->cFileName, ffd.cFileName, sizeof(ffd.cFileName));
                item->inode_type = SQUASHFS_DIR_TYPE;
                //item->inode_num = sortcount;
            }
        } else if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT && ffd.dwReserved0 == IO_REPARSE_TAG_SYMLINK) {
            sortbundle* item = &sortlist[sortcount++];
            memcpy(item->cFileName, ffd.cFileName, sizeof(ffd.cFileName));
            item->inode_type = SQUASHFS_SYMLINK_TYPE;
            item->inode_num = generate_inode_num2();
        } else {
            LARGE_INTEGER filesize = {ffd.nFileSizeLow, ffd.nFileSizeHigh};
            g_data_size += filesize.QuadPart;
            sortbundle* item = &sortlist[sortcount++];
            memcpy(item->cFileName, ffd.cFileName, sizeof(ffd.cFileName));
            item->inode_type = SQUASHFS_REG_TYPE;
            item->inode_num = generate_inode_num2();
            item->nFileSize = filesize.QuadPart;
        }
    } while (FindNextFileW(hFind, &ffd) != 0);
    FindClose(hFind);
    // asc=2E6, desc=2EA, qsort+desc=2E8
    qsort(sortlist, sortcount, sizeof(sortbundle), (int(*)(const void*,const void*))wcscmp);
    stringtable* strtab = (stringtable*)calloc(1, sizeof(stringtable));
    uint32_t cur_dir_index = g_nodesize;
    // 先加入gnodes列表, 后面再分配ID
    nodeitem* diritem = new_nodeitem(SQUASHFS_DIR_TYPE, NULL, 0); // prevent reallocation
    diritem->parentnodenum = parent;
    diritem->paths = strtab;
    for (int i = 0; i < sortcount; i++) {
        sortbundle* sitem = &sortlist[i];
        //wprintf(L"%s\n", sitem->cFileName);
        wchar_t* newpath = (wchar_t*)malloc(2 * (wcslen(folder) + wcslen(sitem->cFileName) + 2));
        swprintf(newpath, L"%s\\%s", folder, sitem->cFileName);
        if (sitem->inode_type == SQUASHFS_REG_TYPE) {
            nodeitem* nitem = new_nodeitem(SQUASHFS_REG_TYPE, newpath, sitem->nFileSize);
            nitem->nodenum = sitem->inode_num;
        } else if (sitem->inode_type == SQUASHFS_DIR_TYPE) {
            //wprintf(L"Enter %s, parent idx %d\n", newpath, cur_dir_index);
            sitem->inode_num = accept_directory(newpath, cur_dir_index);
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
                        char* dest = utf8;
                        while (*dest) {
                            if (*dest == '\\') {
                                *dest = '/'; // 替换路径分隔符
                            }
                            dest++;
                        }
                        nodeitem* nitem = new_nodeitem(SQUASHFS_SYMLINK_TYPE, (wchar_t*)utf8, strlen(utf8));
                        nitem->nodenum = sitem->inode_num;
                    }
                }
                CloseHandle(hFile);
            }
        }
        // 短名称, 后面生成目录列表用
        char* utf8 = unicode_to_utf8(sitem->cFileName);
        add_to_stringtable(strtab, utf8);
        free(utf8);
    }
    // 在所有子目录循环结束后才生成自己ID
    int cur_dir_inode = generate_inode_num();
#ifdef _VERBOSE
    wprintf(L"assign %s #%d\n", folder, cur_dir_inode);
#endif
    diritem = &g_nodes[cur_dir_index]; // 重找回当前目录node
    diritem->nodenum = cur_dir_inode;
#ifdef __cplusplus
    diritem->childs = (decltype(diritem->childs))malloc(sizeof(*diritem->childs) * sortcount); // VC2010有限度支持decltype
#else
    diritem->childs = malloc(sizeof(*diritem->childs) * sortcount);
#endif
    // 目录内容查表用
    for (int i = 0; i < sortcount; i++) {
        diritem->childs[i].node = sortlist[i].inode_num; // dir正式ID, 其他临时ID
        diritem->childs[i].type = sortlist[i].inode_type;
    }
    free(sortlist);
    return cur_dir_inode;
}

void regenerate_inode_num()
{
    int* lookuptable = (int*)malloc(sizeof(int) * g_nodesize);
    for (int i = 0; i < g_nodesize; i++) {
        nodeitem* node = &g_nodes[i];
        if (node->type == SQUASHFS_REG_TYPE || node->type == SQUASHFS_SYMLINK_TYPE) {
            //long realnodenum = generate_inode_num(); // 重新排序
            long realnodenum = node->nodenum + g_root_inode; // 兼容扫描时顺序
            lookuptable[node->nodenum] = realnodenum;
            node->nodenum = realnodenum;
        }
        // fix parent
        if (node->type == SQUASHFS_DIR_TYPE) {
            int parentidx = node->parentnodenum;
            if (parentidx == -1) {
                node->parentnodenum = 0;
            } else {
                node->parentnodenum = g_nodes[parentidx].nodenum;
            }
        }
    }
    for (int i = 0; i < g_nodesize; i++) {
        nodeitem* node = &g_nodes[i];
        // fix childs
        if (node->type == SQUASHFS_DIR_TYPE) {
            for (int j = 0; j < node->paths->count; j++) {
                if (node->childs[j].type != SQUASHFS_DIR_TYPE) {
                    node->childs[j].node = lookuptable[node->childs[j].node];
                }
            }
        }
    }
    free(lookuptable);
}

int wmain(int argc, wchar_t ** argv)
{
    if (argc != 3) {
        printf("Usage: opack <input_directory> <output_file>\n");
        return 1;
    }

    // 首先扫描目录
    g_root_inode = accept_directory(argv[1], -1);
    if (g_root_inode < 0) {
        return 1;
    }
    regenerate_inode_num();
#ifdef _VERBOSE
    for (int i = 0; i < g_nodesize; i++) {
        nodeitem* node = &g_nodes[i];
        if (node->type == SQUASHFS_REG_TYPE) {
            printf("file %S #%d, size %d\n", node->path, node->nodenum, node->size);
        }
        if (node->type == SQUASHFS_SYMLINK_TYPE) {
            printf("link #%d, target %s\n", node->nodenum, (char*)node->path);
        }
        if (node->type == SQUASHFS_DIR_TYPE) {
            printf("dir #%d, parent %d\n", node->nodenum, node->parentnodenum);
            //int parentidx = node->parentnodenum;
            //if (parentidx == -1) {
            //    printf("dir #%d, parent %d\n", node->nodenum, parentidx);
            //} else {
            //    printf("dir #%d, parent %d = #%d\n", node->nodenum, parentidx, g_nodes[parentidx].nodenum);
            //}
            const char* names[] = {"dir ", "file", "link"};
            for (int j = 0; j < node->paths->count; j++) {
                printf("  %s, #%d\n", names[node->childs[j].type - 1], node->childs[j].node);
            }
        }
    }
#endif
    g_opkfd = _wopen(argv[2], O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, _S_IWRITE);
    if (g_opkfd == -1) {
        perror("Error opening output file");
        return 1;
    }

    // 跳过superblock
    _lseek(g_opkfd, sizeof(struct squashfs_super_block), SEEK_SET);
    g_block_offset = sizeof(struct squashfs_super_block);

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
    bool compressed;
#ifdef USE_ZOPFLI
    ZopfliOptions options;
    ZopfliInitOptions(&options);
    options.numiterations = 15;
    unsigned char* zblock = NULL;
    size_t zsize = 0;
    ZopfliZlibCompress(&options, (unsigned char*)block, blocksize, &zblock, &zsize);
    compressed = zsize < blocksize;
#else
    uLong zsize = compressBound(blocksize);
    void* zblock = malloc(zsize);
    int ret = compress2(zblock, &zsize, block, blocksize, Z_BEST_COMPRESSION); //compressData(block, g_BLOCK_SIZE, &zblock, &zsize);
    compressed = ret == Z_OK && zsize < blocksize;
#endif
    if (compressed) {
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
        g_block_offset += _write(g_opkfd, offsets, sizeof(uint64_t) * blockcnt);
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
        bool compressed;
#ifdef USE_ZOPFLI
        ZopfliOptions options;
        ZopfliInitOptions(&options);
        options.numiterations = 15;
        unsigned char* zblock = NULL;
        size_t zsize = 0;
        offsets[i] = dest->size;
        ZopfliZlibCompress(&options, block, blocksize, &zblock, &zsize);
        compressed = zsize < blocksize;
#else
        uLong zsize = compressBound(blocksize);
        void* zblock = malloc(zsize);
        offsets[i] = dest->size;
        int ret = compress2(zblock, &zsize, block, blocksize, Z_BEST_COMPRESSION);
        compressed = ret == Z_OK && zsize < blocksize;
#endif
        if (compressed) {
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
    bool compressed;
#ifdef USE_ZOPFLI
    ZopfliOptions options;
    ZopfliInitOptions(&options);
    options.numiterations = 15;
    unsigned char* zblock = NULL;
    size_t zsize = 0;
    ZopfliZlibCompress(&options, (unsigned char*)task->block, task->blocksize, &zblock, &zsize);
    compressed = zsize < task->blocksize;
#else
    uLong zsize = compressBound(task->blocksize);
    void* zblock = malloc(zsize);
    int ret = compress2(zblock, &zsize, task->block, task->blocksize, Z_BEST_COMPRESSION);
    compressed = ret == Z_OK && zsize < task->blocksize;
#endif
    if (compressed) {
        task->zblock = zblock;
        task->zsize = zsize;
    } else {
        free(zblock);
        task->zblock = NULL;
        task->zsize = 0;
    }
    return 0;
}

uint16_t* pre_caculate_inode_offsets()
{
    uint16_t* nodeoffsets = (uint16_t*)malloc(sizeof(uint16_t) * g_nodesize); // 给dir entry查表用
    size_t nodevsize = 0;
    for (int i = 0; i < g_nodesize; i++) {
        nodeitem* item = &g_nodes[i];
        switch (item->type) {
        case SQUASHFS_REG_TYPE:
            nodeoffsets[item->nodenum] = (uint16_t)nodevsize;
            nodevsize += sizeof(struct squashfs_reg_inode) + (item->size / g_BLOCK_SIZE) * sizeof(uint32_t);
            break;
        case SQUASHFS_SYMLINK_TYPE:
            nodeoffsets[item->nodenum] = (uint16_t)nodevsize;
            nodevsize += sizeof(struct squashfs_symlink_inode) + (size_t)item->size;
            break;
        case SQUASHFS_DIR_TYPE:
            verbose("set dir #%d offset to 0x%X\n", item->nodenum, nodevsize);
            nodeoffsets[item->nodenum] = (uint16_t)nodevsize;
            nodevsize += sizeof(struct squashfs_dir_inode);
            break;
        }
    }
    return nodeoffsets;
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
    //uint16_t* nodeoffsets = pre_caculate_inode_offsets(); // 给dir entry查表用 (非倒置树将无法运行中排序)
    uint16_t* nodeoffsets = (uint16_t*)malloc(sizeof(uint16_t) * g_nodesize); // 给dir entry查表用(倒置树, 运行中排序)
    for (int i = g_nodesize - 1; i >= 0; i--) {
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
            if (blockcnt && g_notailends && item->size % g_BLOCK_SIZE) {
                blockcnt++;
            }
            nodeoffsets[item->nodenum] = (uint16_t)inodetable.size;
            //verbose("set file #%d offset to 0x%X\n", item->nodenum, inodetable.size);
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
                size_t leftsize = item->size;
                for (size_t j = 0; j < blockcnt; j += num_cores) {
                    size_t runcnt = min(num_cores, blockcnt - j);
                    _read(fd, blocks, g_BLOCK_SIZE * runcnt);
                    for (size_t k = 0; k < runcnt; k++, leftsize -= g_BLOCK_SIZE) {
                        tasks[k].block = blocks + k * g_BLOCK_SIZE;
                        tasks[k].blocksize = min(g_BLOCK_SIZE, leftsize);
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
                            g_block_offset += _write(g_opkfd, tasks[k].block, tasks[k].blocksize);
                        }
                        inode->blocks[j + k] = (tasks[k].zblock) ? tasks[k].zsize : (tasks[k].blocksize | (1 << 24));
                    }
                }
                free(blocks);
                free(threads);
                free(tasks);
            }
            // notailends下只存size小于BLOCK_SIZE的
            size_t fragtail = item->size % g_BLOCK_SIZE;
            if (fragtail && (!g_notailends || (g_notailends && item->size < g_BLOCK_SIZE))) {
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
            nodeoffsets[item->nodenum] = (uint16_t)inodetable.size;
            size_t linklen = (size_t)item->size; // utf8 count
            struct squashfs_symlink_inode* inode = (struct squashfs_symlink_inode*)alloc_bytevec(&inodetable, sizeof(struct squashfs_symlink_inode) + linklen);
            inode->header.inode_type = SQUASHFS_SYMLINK_TYPE;
            inode->header.inode_number = item->nodenum;
            inode->symlink_size = linklen;
            memcpy(inode->symlink, item->path, linklen); // utf8 content
        }
        // 根目录node的偏移写入superblock
        if (item->nodenum == g_root_inode) {
            sb.root_inode = inodetable.size;
        }
        if (item->type == SQUASHFS_DIR_TYPE) {
            size_t metablockpos = inodetable.size;
            //verbose("set dir #%d offset to 0x%X\n", item->nodenum, inodetable.size);
            nodeoffsets[item->nodenum] = (uint16_t)inodetable.size;
            struct squashfs_dir_inode* inode = (struct squashfs_dir_inode*)alloc_bytevec(&inodetable, sizeof(struct squashfs_dir_inode));
            inode->header.inode_type = SQUASHFS_DIR_TYPE;
            inode->header.inode_number = item->nodenum;
            if (item->paths->count) {
                inode->file_size = sizeof(struct squashfs_dir_header) + sizeof(struct squashfs_dir_entry) * item->paths->count + (item->paths->size - item->paths->count) + 3;
            } else {
                inode->file_size = 3; // 不生成squashfs_dir_header
            }
            inode->nlink = 2; // historical . ..
            inode->parent_inode = item->parentnodenum;
            // 仅为非空目录生成目录内容列表(header+entries)
            // 依赖于nodeoffsets排序已完成, 如未做倒金字塔排序, 需要预先完整遍历
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
                    memcpy(entry->name, filename, namelen); // utf8
                    entry->type = item->childs[j].type;
                    entry->size = namelen - 1;
                    entry->inode_number = item->childs[j].node - startnode; // node差值不能超过+-32768, 超过要拆header
                    //verbose("node \"%s\" #%d offset 0x%X\n", filename, item->childs[j].node, nodeoffsets[item->childs[j].node]);
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
    g_block_offset += _write(g_opkfd, zdirtable->data, zdirtable->size);
    free(zdirtable->data);
    free(zdirtable);
    // save fragment table
    if (fragtable.data) {
        sb.fragments = fragcnt;
        sb.fragment_table_start = compress_meta_blocks(fragtable.data, fragtable.size, true);
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
