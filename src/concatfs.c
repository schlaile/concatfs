/*
  FUSE: Filesystem in Userspace

  Copyright 2015 Peter Schlaile (peter at schlaile dot de)

  Files with the string "-concat-" anywhere in the filename are considered 
  concatenation description special files.

  They contain a file list, which, when mounted as a fuse file system
  will turn these files into concatenations of the contents of the
  contained files.

  e.g.

  file1.MTS
  file2.MTS
  file3.MTS

  bigmovie-concat-file.MTS

  contents of bigmovie-concat-file.MTS:

  file1.MTS
  file2.MTS
  file3.MTS

  on seperate lines. Empty lines or lines, which do not resolve to a file where
  a stat call succeeds, are ignored.

  gcc -Wall concatfs.c `pkg-config fuse --cflags --libs` -o concatfs
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdbool.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define OFF_T_MAX ((((off_t)1 << (sizeof(off_t) * 8 - 2)) - 1) * 2 + 1)

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CLAMP(x, m, M) (MIN((M), MAX((x), (m))))

static char src_dir[PATH_MAX];

struct chunk {
	struct chunk * next;

	int fd;
	off_t startOffset;
	off_t fsize;
};

struct concat_file {
	struct concat_file * next;
	struct chunk * chunks;

	int fd;
	off_t fsize;
	int refcount;
};

static struct concat_file * open_files = 0;
static pthread_mutex_t  the_lock;

static void lock()
{
	pthread_mutex_lock(&the_lock);
}

static void unlock()
{
	pthread_mutex_unlock(&the_lock);
}

static struct concat_file * open_files_find(int fd)
{
	struct concat_file * cf;

	lock();

	for (cf = open_files; cf; cf = cf->next) {
		if (cf->fd == fd) {
			unlock();
			return cf;
		}
	}
	
	unlock();

	return 0;
}

static void open_files_push_front(struct concat_file * cf)
{
	lock();

	cf->next = open_files;
	open_files = cf;

	unlock();
}

static struct concat_file * open_files_erase(int fd)
{
	struct concat_file * rv = 0;
	struct concat_file * p;

	lock();

	if (open_files && open_files->fd == fd) {
		rv = open_files;
		open_files = rv->next;
	} else {
		for (p = open_files; p; p = p->next) {
			if (p->next && p->next->fd == fd) {
				break;
			}
		}

		if (p) {
			rv = p->next;
			p->next = p->next->next;
		}
	}

	if (rv) {
		rv->next = 0;
	}

	unlock();

	return rv;
}


// tries to parse lines formatted like [path]:[start offset]:[length], with both [start offset] and :[length] being optional
// regex equivalent: ^(?<path>[^:]+)(:(?<startOffset>\d+)?(:(?<length>\d+)?)?)?$
static bool try_parse_line_offsets(char* line, off_t* startOffset, off_t* length)
{
	struct stat stbuf;
	bool statOk = false;
	char* offsetsLineStart;
	char t;
	off_t s = -1;
	off_t l = OFF_T_MAX;

	if (line == NULL || line[0] == '\0') return false;

	// find the point between path and offsets
	offsetsLineStart = strchr(line, ':');

	// cut the string into "two" strings (path \0 offsets) if needed, and stat() the path
	if (offsetsLineStart != NULL)
	{
		t = *offsetsLineStart;
		*offsetsLineStart = '\0';
	}
	statOk = (stat(line, &stbuf) == 0);
	/*if (offsetsLineStart != NULL)
	{
		// restore the string
		*offsetsLineStart = t;
	}*/
	if (!statOk) return false;
	if (stbuf.st_size < 1) return false; // can't really use files with 0 size

	// try to parse the numbers
	if (offsetsLineStart != NULL)
	{
		const char* lengthLineStart;

		// read the start offset
		sscanf(offsetsLineStart + 1, "%jd", &s);
		// find the length number and parse it
		lengthLineStart = strchr(offsetsLineStart + 1, ':');
		if (lengthLineStart != NULL)
		{
			sscanf(lengthLineStart + 1, "%jd", &l);
		}
	}

	s = CLAMP(s, 0, stbuf.st_size - 1);
	l = CLAMP(l, 1, stbuf.st_size - s);

	*startOffset = s;
	*length = l;
	return true;
}

static struct concat_file * open_concat_file(int fd, const char * path)
{
	struct concat_file * rv = 0;
	char bpath[PATH_MAX+1];
	char fpath[PATH_MAX+1];
	char * base_dir;
	struct chunk * c = 0;
	
	FILE * fp;

	if (fd >= 0) {
		fp = fdopen(dup(fd), "r");
	} else {
		fp = fopen(path, "r");
	}

	if (!fp) {
		return 0;
	}

	rv = (struct concat_file *) calloc(sizeof(struct concat_file), 1);
	strncpy(bpath, path, sizeof(bpath));

	base_dir = dirname(bpath);

	fpath[PATH_MAX] = 0;
	bpath[PATH_MAX] = 0;

	rv->fd = fd;
	rv->refcount = 1;

	while (fgets(fpath, sizeof(fpath), fp)) {
		char tpath[PATH_MAX];
		struct chunk * c_n;
		off_t startOffset = 0;
		off_t length = 0;

		fpath[strlen(fpath) - 1] = 0;

		if (fpath[0] == '/') {
			strncpy(tpath, fpath, sizeof(tpath));
		} else {
			snprintf(tpath, sizeof(tpath), "%s/%s",base_dir, fpath);
		}

		if (!try_parse_line_offsets(tpath, &startOffset, &length)) continue;
		rv->fsize += length;

		if (fd >= 0) {
			c_n = (struct chunk *) calloc(sizeof(struct chunk), 1);

			c_n->startOffset = startOffset;
			c_n->fsize = length;
			c_n->fd = open(tpath, O_RDONLY);

			if (c) {
				c->next = c_n;
			} else {
				rv->chunks = c_n;
			}
			c = c_n;
		}
	}
	fclose(fp);	
	return rv;
}


static void close_concat_file(struct concat_file * cf)
{
	struct chunk * c;

	if (!cf) {
		return;
	}

	for (c = cf->chunks; c;) {
		struct chunk * t;

		close(c->fd);
		
		t = c;

		c = c->next;

		free(t);
	}

	close(cf->fd);
	
	free(cf);
}

static off_t get_concat_file_size(const char * path)
{
	struct concat_file * c = open_concat_file(-1, path);
	off_t rv;

	if (!c) {
		return 0;
	}

	rv = c->fsize;

	close_concat_file(c);

	return rv;
}

static int read_concat_file(int fd, void *buf, size_t count, off_t offset)
{
	struct concat_file * cf = open_files_find(fd);
	struct chunk * c;
	ssize_t bytes_read = 0;

	if (!cf) {
		return -EINVAL;
	}

	if (offset > cf->fsize) {
		return 0;
	}
	
	c = cf->chunks;

	for (; c && offset > c->fsize; c = c->next) {
		offset -= c->fsize;
	}

	for (; c && count > c->fsize - offset; c = c->next) {
		ssize_t rv;
		rv = pread(c->fd, buf, c->fsize - offset, offset + c->startOffset);

		if (rv == c->fsize - offset) {
			buf += rv;
			offset = 0;
			count -= rv;
			bytes_read += rv;
		} else if (rv > 0) {
			bytes_read += rv;
			return bytes_read;
		} else {
			return -errno;
		}
	}
	
	if (c && count > 0) {
		ssize_t rv;
		rv = pread(c->fd, buf, count, offset + c->startOffset);

		if (rv < 0) {
			return -errno;
		}

		bytes_read += rv;
	}

	return bytes_read;
}

static int is_concatfs_file(const char * path)
{
	char fpath[PATH_MAX];

	strncpy(fpath, path, sizeof(fpath));

	return (strstr(basename(fpath), "-concat-") != 0);
}

static int concatfs_readlink(const char *path, char *link, size_t size)
{
	int rv = 0;
	char fpath[PATH_MAX];

	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);
    
	rv = readlink(fpath, link, size - 1);
	if (rv < 0) {
		rv = -errno;
	} else {
		link[rv] = '\0';
		rv = 0;
	}
	
	return rv;
}
static int concatfs_getattr(const char *path, struct stat *stbuf)
{
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	memset(stbuf, 0, sizeof(struct stat));

	if (lstat(fpath, stbuf) != 0)
		return -errno;
	
	if (is_concatfs_file(path)) {
		stbuf->st_size = get_concat_file_size(fpath);
	} 

	return 0;
}

static int concatfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			    off_t offset, struct fuse_file_info *fi)
{
	int retstat = 0;
	DIR *dp;
	struct dirent *de;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	dp = opendir(fpath);

	if (!dp) {
		return -errno;
	}

	de = readdir(dp);
	if (de == 0) {
		closedir(dp);
		return -errno;
	}
	
	do {
		if (filler(buf, de->d_name, NULL, 0) != 0) {
			closedir(dp);
			return -ENOMEM;
		}
	} while ((de = readdir(dp)) != NULL);
	
	closedir(dp);

	return retstat;
}

static int concatfs_open(const char *path, struct fuse_file_info *fi)
{
	int fd;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	fd = open(fpath, fi->flags);

	if (fd < 0) {
		return -errno;
	}

	fi->fh = fd;

	if (is_concatfs_file(path)) {
		open_files_push_front(open_concat_file(fd, fpath));
	}

	return 0;
}

static int concatfs_release(const char * path, struct fuse_file_info * fi)
{
	if (is_concatfs_file(path)) {
		close_concat_file(open_files_erase(fi->fh));
	} else {
		close(fi->fh);
	}

	return 0;
}

static int concatfs_read(const char *path, char *buf, size_t size, off_t offset,
			 struct fuse_file_info *fi)
{
	int rv = 0;

	if (is_concatfs_file(path)) {
		return read_concat_file(fi->fh, buf, size, offset);
	} else {
		rv = pread(fi->fh, buf, size, offset);
		if (rv < 0) {
			return -errno;
		}
	}

	return rv;
}

static int concatfs_write(
	const char *path, const char *buf, size_t size, off_t offset,
	struct fuse_file_info *fi)
{
	int rv = 0;

	if (is_concatfs_file(path)) {
		return -EINVAL;
	} else {
		rv = pwrite(fi->fh, buf, size, offset);
		if (rv < 0) {
			return -errno;
		}
	}

	return rv;
}

static int concatfs_mknod(const char *path, mode_t mode, dev_t dev)
{
	int rv;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	rv = mknod(fpath, mode, dev);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_mkdir(const char *path, mode_t mode)
{
	int rv;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	rv = mkdir(fpath, mode);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_unlink(const char *path)
{
	int rv;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	rv = unlink(fpath);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_rmdir(const char *path)
{
	int rv;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	rv = rmdir(fpath);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_symlink(const char *path, const char * link)
{
	int rv;
	char flink[PATH_MAX];
    
	snprintf(flink, sizeof(flink), "%s/%s", src_dir, path);

	rv = symlink(path, flink);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_rename(const char *path, const char *topath)
{
	int rv;
	char fpath[PATH_MAX];
	char ftopath[PATH_MAX];

	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);
	snprintf(ftopath, sizeof(ftopath), "%s/%s", src_dir, topath);
	
	rv = rename(fpath, ftopath);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_link(const char *path, const char *topath)
{
	int rv;
	char fpath[PATH_MAX];
	char ftopath[PATH_MAX];

	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);
	snprintf(ftopath, sizeof(ftopath), "%s/%s", src_dir, topath);
	
	rv = link(fpath, ftopath);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_chmod(const char *path, mode_t mode)
{
	int rv;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	rv = chmod(fpath, mode);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_chown(const char *path, uid_t uid, gid_t gid)
{
	int rv;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	rv = chown(fpath, uid, gid);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_truncate(const char *path, off_t nsize)
{
	int rv;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	rv = truncate(fpath, nsize);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_utime(const char *path, struct utimbuf * buf)
{
	int rv;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	rv = utime(fpath, buf);
	if (rv < 0) {
		return -errno;
	}
	return rv;
}

static int concatfs_access(const char *path, int mask)
{
	int rv;
	char fpath[PATH_MAX];
   
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);
    
	rv = access(fpath, mask);
    
	if (rv < 0) {
		return -errno;
	}
    
	return rv;
}

static int concatfs_create(
	const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int fd = 0;
	char fpath[PATH_MAX];
   
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);
    
	fd = creat(fpath, mode);
    
	if (fd < 0) {
		return -errno;
	}

	fi->fh = fd;
    
	return 0;
}


static struct fuse_operations concatfs_oper = {
	.getattr	= concatfs_getattr,
	.readlink       = concatfs_readlink,
	.mknod          = concatfs_mknod,
	.mkdir          = concatfs_mkdir,
	.unlink         = concatfs_unlink,
	.rmdir          = concatfs_rmdir,
	.symlink        = concatfs_symlink,
	.rename         = concatfs_rename,
	.link           = concatfs_link,
	.chmod          = concatfs_chmod,
	.chown          = concatfs_chown,
	.truncate       = concatfs_truncate,
	.utime          = concatfs_utime,
	.open		= concatfs_open,
	.read		= concatfs_read,
	.write          = concatfs_write,
	.release        = concatfs_release,
	.readdir	= concatfs_readdir,
	.access         = concatfs_access,
	.create         = concatfs_create,
};

static void usage()
{
	fprintf(stderr, "Usage: poc_concatfs src-dir fuse-mount-options...\n");
	exit(-1);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		usage();
	}

	if ((getuid() == 0) || (geteuid() == 0)) {
		fprintf(stderr, 
			"WARNING! concatfs does *no* file access checking "
			"right now and therefore is *dangerous* to use "
			"as root!");
	}

	if (argv[1][0] == '/') {
		strncpy(src_dir, argv[1], sizeof(src_dir));
	} else {
		char cwd[PATH_MAX];

		getcwd(cwd, sizeof(cwd));

		snprintf(src_dir, sizeof(src_dir), "%s/%s",
			 cwd, argv[1]);
	}

	pthread_mutex_init(&the_lock, NULL);

	char ** argv_ = (char**) calloc(argc, sizeof(char*));

	argv_[0] = argv[0];

	memcpy(argv_ + 1, argv + 2, (argc - 2) * sizeof(char*));

	return fuse_main(argc - 1, argv_, &concatfs_oper, NULL);
}
