#include "server.h"

#include <sys/mman.h>
#include <junzip.h>
#include <zlib.h>

#ifdef LWS_FOP_FLAG_COMPR_ACCEPTABLE_GZIP
#define USE_NEW_FOPS 1
#else
#define USE_NEW_FOPS 0
#endif

struct junzip_mem_handle {
    JZFile handle;
    int fd;
    JZFileHeader *entries;
};

struct open_mem_file {
    struct junzip_mem_handle *handle;
    int index; // in handle->entries table
    char *data;
    long length;
    long position;
};
#if USE_NEW_FOPS
#define lws_filefd_to_mem_file(FD) \
  ((struct open_mem_file*) (FD)->filesystem_priv)
#else
#define MAX_OPEN_MEM_FILES 32
struct open_mem_file open_mem_files[MAX_OPEN_MEM_FILES];
#define lws_filefd_to_mem_file(FD) \
  ((FD) >= 1000 ? &open_mem_files[(FD)-1000] : NULL)
#define mem_file_to_lws_filefd(MEM) (((MEM)-&open_mem_files[0])+1000)
#endif

struct lws_plat_file_ops fops_plat;

struct junzip_mem_handle junzip_handler;

static void
junzip_mem_close(JZFile *zfile)
{
    struct junzip_mem_handle *handle = (struct junzip_mem_handle *) zfile;
    munmap(zfile->start, zfile->length);
    // FIXME free(entries)
    close(handle->fd);
}

JZFile *
init_junzip_handle(struct junzip_mem_handle *handle,
                        char *start, off_t length, int fd)
{
    handle->handle.start = start;
    handle->handle.length = length;
    handle->fd = fd;
    handle->handle.position = 0;
    return &handle->handle;
}

static int zipRecordCallback(JZFile *zip, int idx, JZFileHeader *header) {
    struct junzip_mem_handle *handle = (struct junzip_mem_handle *) zip;
    handle->entries[idx] = *header;
    return 1;
}

static char domterm_resource_prefix[] = "domterm:/";

#if USE_NEW_FOPS
static lws_fop_fd_t
domserver_fops_open(struct lws_plat_file_ops *fops,
                     const char *filename,
                     lws_filepos_t *filelen, lws_fop_flags_t *flags)
#else
static lws_filefd_type
domserver_fops_open(struct lws *wsi, const char *filename,
                    unsigned long *filelen, int flags)
#endif
{
    lws_filefd_type n;
#if USE_NEW_FOPS
    lws_fop_fd_t fop_fd;
#endif
    if (strncmp(filename, domterm_resource_prefix,
                sizeof(domterm_resource_prefix)-1) == 0) {
        JZFile *zip = &junzip_handler.handle;
        int numEntries = zip->numEntries;
        const char *entry_name = filename+sizeof(domterm_resource_prefix)-1;
        int fnlength = strlen(entry_name);
        for (int i = 0; i < numEntries; i++) {
            JZFileHeader *entry = &junzip_handler.entries[i];
            if (fnlength == entry->fileNameLength
                && memcmp(entry_name, zip->start + entry->fileNameStart,
                          fnlength) == 0) {
                uint32_t uncompressedSize = entry->uncompressedSize;
                struct open_mem_file *mem;
#if USE_NEW_FOPS
                fop_fd = xmalloc(sizeof(*fop_fd)+sizeof(struct open_mem_file));
                mem = (struct open_mem_file*) (fop_fd+1);
                fop_fd->filesystem_priv = mem;
                fop_fd->fops = fops;
#else
                int j = MAX_OPEN_MEM_FILES;
                for (;;) {
                    if (--j == 0) {
                        errno = EMFILE;
                        return LWS_INVALID_FILE;
                    }
                    mem = &open_mem_files[j];
                    if (mem->handle == NULL)
                        break;
                }
#endif
                mem->handle = &junzip_handler;
                mem->index = i;
                mem->position = 0;
                size_t offset = entry->offset;
                offset += ZIP_LOCAL_FILE_HEADER_LENGTH;
                offset += entry->fileNameLength + entry->extraFieldLength;
                unsigned long rsize;
                int sentCompressed = 0;
#ifdef LWS_FOP_FLAG_COMPR_ACCEPTABLE_GZIP
                if ((*flags & LWS_FOP_FLAG_COMPR_ACCEPTABLE_GZIP) != 0
                    && entry->compressionMethod == 8) {
                    uint32_t compressedSize = entry->compressedSize;
                    sentCompressed = 1;
                    rsize = 18 + compressedSize;
                    char *data = xmalloc(rsize);
                    mem->data = data;
                    unsigned char *ptr = data;
                    // write 10-bytes header FIXME
                    *ptr++ = 31; *ptr++ = 139; // ID1, ID2
                    *ptr++ = 8; // Compression Method Deflate
                    *ptr++ = 0; // Flags
                    for (int k = 4; --k >= 0; ) *ptr++ = 0; // MTIME = 0
                    *ptr++ = 0; // Extra Flags. should get from generalPurposeBitFlag
                    *ptr++ = 3; // OS=Unix.  Could use versionMadeBy
                    memcpy(ptr, zip->start + offset, compressedSize);
                    ptr += compressedSize;
                    // write 8-byte footer
                    uint32_t val = entry->crc32;
                    for (int k = 4; --k >= 0; ) {
                      *ptr++ = val & 0xFF; val >>= 8;
                    }
                    val = uncompressedSize;
                    for (int k = 4; --k >= 0; ) {
                      *ptr++ = val & 0xFF; val >>= 8;
                    }
                    *flags |= LWS_FOP_FLAG_COMPR_IS_GZIP;
                }
#endif
                if (! sentCompressed) {
                    zf_seek_set(zip, offset);
                    rsize = uncompressedSize;
                    char *data = xmalloc(rsize);
                    mem->data = data;
                    if (jzReadData(&junzip_handler.handle,
                                   entry, data) != Z_OK) {
                      fprintf(stderr, "Couldn't read file data!");
                      free(data);
#if USE_NEW_FOPS
                      return NULL;
#else
                      return LWS_INVALID_FILE;
#endif
                    }
                }
                mem->length = rsize;
                *filelen = rsize;
#if USE_NEW_FOPS
                return fop_fd;
#else
                return mem_file_to_lws_filefd(mem);
#endif
            }
        }
        errno = EMFILE;
#if USE_NEW_FOPS
        return NULL;
#else
        return LWS_INVALID_FILE;
#endif
    }

    /* call through to original platform implementation */
#if USE_NEW_FOPS
    return fops_plat.open(fops, filename, filelen, flags);
#else
    return fops_plat.open(wsi, filename, filelen, flags);
#endif
}

static int
#if USE_NEW_FOPS
domserver_fops_close(lws_fop_fd_t fd)
#else
domserver_fops_close(struct lws *wsi, lws_filefd_type fd)
#endif
{
    struct open_mem_file *mem = lws_filefd_to_mem_file(fd);
    if (mem != NULL) {
        mem->handle = NULL;
        free(mem->data);
        mem->data = NULL;
        mem->length = 0;
        mem->index = 0;
        return 0;
    }
#if USE_NEW_FOPS
    return fops_plat.close(fd);
#else
    return fops_plat.close(wsi, fd);
#endif
}

#if USE_NEW_FOPS
static lws_fileofs_t
domserver_fops_seek_cur(lws_fop_fd_t fd,
                        lws_fileofs_t offset_from_cur_pos)
#else
static unsigned long
domserver_fops_seek_cur(struct lws *wsi, lws_filefd_type fd,
                        long offset_from_cur_pos)
#endif
{
    struct open_mem_file *mem = lws_filefd_to_mem_file(fd);
#if !USE_NEW_FOPS
    if (mem != NULL) {
#endif
    long new_position = mem->position + offset_from_cur_pos;
    if (new_position < 0 || new_position > mem->length)
        return (off_t) (-1);
    mem->position = new_position;
    return new_position;
#if !USE_NEW_FOPS
    }
    return fops_plat.seek_cur(wsi, fd, offset_from_cur_pos);
#endif
}

static int
#if USE_NEW_FOPS
domserver_fops_read(lws_fop_fd_t fd, lws_filepos_t *amount,
                    uint8_t *buf, lws_filepos_t len)
#else
domserver_fops_read(struct lws *wsi, lws_filefd_type fd, unsigned long *amount,
                    unsigned char *buf, unsigned long len)
#endif
{
    struct open_mem_file *mem = lws_filefd_to_mem_file(fd);
#if !USE_NEW_FOPS
    if (mem != NULL) {
#endif
    unsigned long avail = mem->length - mem->position;
    if (len > avail)
        len = avail;
    memcpy(buf, mem->data + mem->position, len);
    mem->position += len;
    *amount = len;
    return 0;
#if !USE_NEW_FOPS
    }
    return fops_plat.read(wsi, fd, amount, buf, len);
#endif
}

void
initialize_resource_map(struct lws_context *context,
                        const char *domterm_jar_path)
{
    int open_mode = O_RDONLY;
    struct stat statbuf;
#if O_CLOEXEC
    open_mode |= O_CLOEXEC;
#endif
    int fd = open(domterm_jar_path, open_mode);
    if (fd < 0 || fstat(fd, &statbuf) != 0) {
        fprintf(stderr, "domterm: failed to open '%s'\n", domterm_jar_path);
        exit(-1);
    }
    off_t jarsize = statbuf.st_size;
    void *jardata = mmap(NULL, jarsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (jardata == MAP_FAILED) {
        fprintf(stderr, "domterm: failed to map '%s'\n", domterm_jar_path);
        exit(-1);
    }
    struct junzip_mem_handle *mzip = &junzip_handler;
    JZFile *zip = init_junzip_handle(mzip, jardata, jarsize, fd);

    if(jzReadEndRecord(zip)) {
        fprintf(stderr, "Couldn't read ZIP file end record.");
        exit(-1);
    }
    int numEntries = zip->numEntries;
    mzip->entries = xmalloc(numEntries * sizeof(JZFileHeader));
    if(jzReadCentralDirectory(zip, zipRecordCallback)) {
        printf("Couldn't read ZIP file central record.");
    }

    /* stash original platform fops */
    fops_plat = *(lws_get_fops(context));
    /* override the active fops */
    lws_get_fops(context)->open = domserver_fops_open;
    lws_get_fops(context)->close = domserver_fops_close;
    lws_get_fops(context)->seek_cur = domserver_fops_seek_cur;
    lws_get_fops(context)->read = domserver_fops_read;
}
