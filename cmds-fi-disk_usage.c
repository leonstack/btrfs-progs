/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdarg.h>

#include "utils.h"
#include "kerncompat.h"
#include "ctree.h"

#include "commands.h"

#include "version.h"

#define DF_HUMAN_UNIT		(1<<0)

/* to store the information about the chunks */
struct chunk_info {
	u64	type;
	u64	size;
	u64	devid;
};

/* to store information about the disks */
struct disk_info {
	u64	devid;
	char	path[BTRFS_DEVICE_PATH_NAME_MAX];
	u64	size;
};

/* to store the tmp strings */
static void **strings_to_free;
static int count_string_to_free;

static void add_strings_to_free(char *s)
{
	int  size;

	size = sizeof(void *) * ++count_string_to_free;
	strings_to_free = realloc(strings_to_free, size);

	/* if we don't have enough memory, we have more serius
	   problem than that a wrong handling of not enough memory */
	if (!strings_to_free) {
		fprintf(stderr, "add_string_to_free(): Not enough memory\n");
		strings_to_free = 0;
		count_string_to_free = 0;
	}

	strings_to_free[count_string_to_free-1] = s;
}

static void free_strings_to_free()
{
	int	i;
	for (i = 0 ; i < count_string_to_free ; i++)
		free(strings_to_free[i]);

	free(strings_to_free);

	strings_to_free = 0;
	count_string_to_free = 0;
}

static int cmd_info_add_info(struct chunk_info **info_ptr,
			int *info_count,
			struct btrfs_chunk *chunk)
{

	u64	type = btrfs_stack_chunk_type(chunk);
	u64	size = btrfs_stack_chunk_length(chunk);
	int	num_stripes = btrfs_stack_chunk_num_stripes(chunk);
	int	sub_stripes = btrfs_stack_chunk_sub_stripes(chunk);
	int	j;

	for (j = 0 ; j < num_stripes ; j++) {
		int i;
		struct chunk_info *p = 0;
		struct btrfs_stripe *stripe;
		u64    devid;

		stripe = btrfs_stripe_nr(chunk, j);
		devid = btrfs_stack_stripe_devid(stripe);

		for (i = 0 ; i < *info_count ; i++)
			if ((*info_ptr)[i].type == type &&
			    (*info_ptr)[i].devid == devid) {
				p = (*info_ptr) + i;
				break;
			}

		if (!p) {
			int size = sizeof(struct btrfs_chunk) * (*info_count+1);
			struct chunk_info *res = realloc(*info_ptr, size);

			if (!res) {
				fprintf(stderr, "ERROR: not enough memory\n");
				return -1;
			}

			*info_ptr = res;
			p = res + *info_count;
			(*info_count)++;

			p->devid = devid;
			p->type = type;
			p->size = 0;
		}

		if (type & (BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_DUP))
			p->size += size;
		else if (type & BTRFS_BLOCK_GROUP_RAID10)
			p->size += size / (num_stripes / sub_stripes);
		else
			p->size += size / num_stripes;

	}

	return 0;

}

static void btrfs_flags2description(u64 flags, char **description)
{
	if (flags & BTRFS_BLOCK_GROUP_DATA) {
		if (flags & BTRFS_BLOCK_GROUP_METADATA)
			*description = "Data+Metadata";
		else
			*description = "Data";
	} else if (flags & BTRFS_BLOCK_GROUP_SYSTEM) {
		*description = "System";
	} else if (flags & BTRFS_BLOCK_GROUP_METADATA) {
		*description = "Metadata";
	} else {
		*description = "Unknown";
	}
}

static void btrfs_flags2profile(u64 flags, char **profile)
{
	if (flags & BTRFS_BLOCK_GROUP_RAID0) {
		*profile = "RAID0";
	} else if (flags & BTRFS_BLOCK_GROUP_RAID1) {
		*profile = "RAID1";
	} else if (flags & BTRFS_BLOCK_GROUP_DUP) {
		*profile = "DUP";
	} else if (flags & BTRFS_BLOCK_GROUP_RAID10) {
		*profile = "RAID10";
	} else {
		*profile = "Single";
	}
}

static char *df_pretty_sizes(u64 size, int mode)
{
	char *s;

	if (mode & DF_HUMAN_UNIT) {
		s = pretty_sizes(size);
		if (!s)
			return NULL;
	} else {
		s = malloc(20);
		if (!s)
			return NULL;
		sprintf(s, "%llu", size);
	}

	add_strings_to_free(s);
	return s;
}

static int cmp_chunk_block_group(u64 f1, u64 f2)
{

	u64 mask;

	if ((f1 & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
		(f2 & BTRFS_BLOCK_GROUP_TYPE_MASK))
			mask = BTRFS_BLOCK_GROUP_PROFILE_MASK;
	else if (f2 & BTRFS_BLOCK_GROUP_SYSTEM)
		return -1;
	else if (f1 & BTRFS_BLOCK_GROUP_SYSTEM)
		return +1;
	else
			mask = BTRFS_BLOCK_GROUP_TYPE_MASK;

	if ((f1 & mask) > (f2 & mask))
		return +1;
	else if ((f1 & mask) < (f2 & mask))
		return -1;
	else
		return 0;
}

static int cmp_btrfs_ioctl_space_info(const void *a, const void *b)
{
	return cmp_chunk_block_group(
		((struct btrfs_ioctl_space_info *)a)->flags,
		((struct btrfs_ioctl_space_info *)b)->flags);
}

static struct btrfs_ioctl_space_args *load_space_info(int fd, char *path)
{
	struct btrfs_ioctl_space_args *sargs = 0, *sargs_orig = 0;
	int e, ret, count;

	sargs_orig = sargs = malloc(sizeof(struct btrfs_ioctl_space_args));
	if (!sargs) {
		fprintf(stderr, "ERROR: not enough memory\n");
		return NULL;
	}

	sargs->space_slots = 0;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;
	if (ret) {
		fprintf(stderr,
			"ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));
		free(sargs);
		return NULL;
	}
	if (!sargs->total_spaces) {
		free(sargs);
		printf("No chunks found\n");
		return NULL;
	}

	count = sargs->total_spaces;

	sargs = realloc(sargs, sizeof(struct btrfs_ioctl_space_args) +
			(count * sizeof(struct btrfs_ioctl_space_info)));
	if (!sargs) {
		free(sargs_orig);
		fprintf(stderr, "ERROR: not enough memory\n");
		return NULL;
	}

	sargs->space_slots = count;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;

	if (ret) {
		fprintf(stderr,
			"ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));
		free(sargs);
		return NULL;
	}

	qsort(&(sargs->spaces), count, sizeof(struct btrfs_ioctl_space_info),
		cmp_btrfs_ioctl_space_info);

	return sargs;
}

static int _cmd_disk_free(int fd, char *path, int mode)
{
	struct btrfs_ioctl_space_args *sargs = 0;
	int i;
	int ret = 0;
	int e, width;
	u64 total_disk;		/* filesystem size == sum of
				   disks sizes */
	u64 total_chunks;	/* sum of chunks sizes on disk(s) */
	u64 total_used;		/* logical space used */
	u64 total_free;		/* logical space un-used */
	double K;

	if ((sargs = load_space_info(fd, path)) == NULL) {
		ret = -1;
		goto exit;
	}

	total_disk = disk_size(path);
	e = errno;
	if (total_disk == 0) {
		fprintf(stderr,
			"ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));

		ret = 19;
		goto exit;
	}

	total_chunks = total_used = total_free = 0;

	for (i = 0; i < sargs->total_spaces; i++) {
		int  ratio = 1;
		u64  allocated;

		u64 flags = sargs->spaces[i].flags;

		if (flags & BTRFS_BLOCK_GROUP_RAID0)
			ratio = 1;
		else if (flags & BTRFS_BLOCK_GROUP_RAID1)
			ratio = 2;
		else if (flags & BTRFS_BLOCK_GROUP_DUP)
			ratio = 2;
		else if (flags & BTRFS_BLOCK_GROUP_RAID10)
			ratio = 2;
		else
			ratio = 1;

		allocated = sargs->spaces[i].total_bytes * ratio;

		total_chunks += allocated;
		total_used += sargs->spaces[i].used_bytes;
		total_free += (sargs->spaces[i].total_bytes -
					sargs->spaces[i].used_bytes);

	}
	K = ((double)total_used + (double)total_free) /	(double)total_chunks;

	if (mode & DF_HUMAN_UNIT)
		width = 9;
	else
		width = 18;

	printf("Disk size:\t\t%*s\n", width,
		df_pretty_sizes(total_disk, mode));
	printf("Disk allocated:\t\t%*s\n", width,
		df_pretty_sizes(total_chunks, mode));
	printf("Disk unallocated:\t%*s\n", width,
		df_pretty_sizes(total_disk-total_chunks, mode));
	printf("Used:\t\t\t%*s\n", width,
		df_pretty_sizes(total_used, mode));
	printf("Free (Estimated):\t%*s\t(Max: %s, min: %s)\n",
		width,
		df_pretty_sizes((u64)(K*total_disk-total_used), mode),
		df_pretty_sizes(total_disk-total_chunks+total_free, mode),
		df_pretty_sizes((total_disk-total_chunks)/2+total_free, mode));
	printf("Data to disk ratio:\t%*.0f %%\n",
		width-2, K*100);

exit:

	free_strings_to_free();
	if (sargs)
		free(sargs);

	return ret;
}

const char * const cmd_filesystem_df_usage[] = {
	"btrfs filesystem df [-k] <path> [<path>..]",
	"Show space usage information for a mount point(s).",
	"",
	"-b\tSet byte as unit",
	NULL
};

int cmd_filesystem_df(int argc, char **argv)
{

	int	flags = DF_HUMAN_UNIT;
	int	i, more_than_one = 0;

	optind = 1;
	while (1) {
		char	c = getopt(argc, argv, "b");
		if (c < 0)
			break;

		switch (c) {
		case 'b':
			flags &= ~DF_HUMAN_UNIT;
			break;
		default:
			usage(cmd_filesystem_df_usage);
		}
	}

	if (check_argc_min(argc - optind, 1)) {
		usage(cmd_filesystem_df_usage);
		return 21;
	}

	for (i = optind; i < argc ; i++) {
		int r, fd;
		if (more_than_one)
			printf("\n");

		fd = open_file_or_dir(argv[i]);
		if (fd < 0) {
			fprintf(stderr, "ERROR: can't access to '%s'\n",
				argv[1]);
			return 12;
		}
		r = _cmd_disk_free(fd, argv[i], flags);
		close(fd);

		if (r)
			return r;
		more_than_one = 1;

	}

	return 0;
}

static int cmp_chunk_info(const void *a, const void *b)
{
	return cmp_chunk_block_group(
		((struct chunk_info *)a)->type,
		((struct chunk_info *)b)->type);
}

static int load_chunk_info(int fd,
			  struct chunk_info **info_ptr,
			  int *info_count)
{

	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	unsigned long off = 0;
	int i, e;


	memset(&args, 0, sizeof(args));

	/*
	 * there may be more than one ROOT_ITEM key if there are
	 * snapshots pending deletion, we have to loop through
	 * them.
	 */


	sk->tree_id = BTRFS_CHUNK_TREE_OBJECTID;

	sk->min_objectid = 0;
	sk->max_objectid = (u64)-1;
	sk->max_type = 0;
	sk->min_type = (u8)-1;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	while (1) {
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		e = errno;
		if (ret < 0) {
			fprintf(stderr,
				"ERROR: can't perform the search - %s\n",
				strerror(e));
			return 0;
		}
		/* the ioctl returns the number of item it found in nr_items */

		if (sk->nr_items == 0)
			break;

		off = 0;
		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_chunk *item;
			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);

			off += sizeof(*sh);
			item = (struct btrfs_chunk *)(args.buf + off);

			if (cmd_info_add_info(info_ptr, info_count, item)) {
				*info_ptr = 0;
				free(*info_ptr);
				return 100;
			}

			off += sh->len;

			sk->min_objectid = sh->objectid;
			sk->min_type = sh->type;
			sk->min_offset = sh->offset+1;

		}
		if (!sk->min_offset)	/* overflow */
			sk->min_type++;
		else
			continue;

		if (!sk->min_type)
			sk->min_objectid++;
		 else
			continue;

		if (!sk->min_objectid)
			break;
	}

	qsort(*info_ptr, *info_count, sizeof(struct chunk_info),
		cmp_chunk_info);

	return 0;

}

static int cmp_disk_info(const void *a, const void *b)
{
	return strcmp(((struct disk_info *)a)->path,
			((struct disk_info *)b)->path);
}

static int load_disks_info(int fd,
			   struct disk_info **disks_info_ptr,
			   int *disks_info_count)
{

	int ret, i, ndevs;
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args dev_info;
	struct disk_info *info;

	*disks_info_count = 0;
	*disks_info_ptr = 0;

	ret = ioctl(fd, BTRFS_IOC_FS_INFO, &fi_args);
	if (ret < 0) {
		fprintf(stderr, "ERROR: cannot get filesystem info\n");
		return -1;
	}

	info = malloc(sizeof(struct disk_info) * fi_args.num_devices);
	if (!info) {
		fprintf(stderr, "ERROR: not enough memory\n");
		return -1;
	}

	for (i = 0, ndevs = 0 ; i <= fi_args.max_id ; i++) {

		BUG_ON(ndevs >= fi_args.num_devices);
		ret = get_device_info(fd, i, &dev_info);

		if (ret == -ENODEV)
			continue;
		if (ret) {
			fprintf(stderr,
			    "ERROR: cannot get info about device devid=%d\n",
			    i);
			free(info);
			return -1;
		}

		info[ndevs].devid = dev_info.devid;
		strcpy(info[ndevs].path, (char *)dev_info.path);
		info[ndevs].size = get_partition_size((char *)dev_info.path);
		++ndevs;
	}

	BUG_ON(ndevs != fi_args.num_devices);
	qsort(info, fi_args.num_devices,
		sizeof(struct disk_info), cmp_disk_info);

	*disks_info_count = fi_args.num_devices;
	*disks_info_ptr = info;

	return 0;

}

static void print_unused(struct chunk_info *info_ptr,
			  int info_count,
			  struct disk_info *disks_info_ptr,
			  int disks_info_count,
			  int mode)
{
	int i;
	for (i = 0 ; i < disks_info_count ; i++) {

		int	j;
		u64	total = 0;
		char	*s;

		for (j = 0 ; j < info_count ; j++)
			if (info_ptr[j].devid == disks_info_ptr[i].devid)
				total += info_ptr[j].size;

		s = df_pretty_sizes(disks_info_ptr[i].size - total, mode);
		printf("   %s\t%10s\n", disks_info_ptr[i].path, s);

	}

}

static void print_chunk_disks(u64 chunk_type,
				struct chunk_info *chunks_info_ptr,
				int chunks_info_count,
				struct disk_info *disks_info_ptr,
				int disks_info_count,
				int mode)
{
	int i;
	for (i = 0 ; i < disks_info_count ; i++) {

		int	j;
		u64	total = 0;
		char	*s;

		for (j = 0 ; j < chunks_info_count ; j++) {
			if (chunks_info_ptr[j].type != chunk_type)
				continue;
			if (chunks_info_ptr[j].devid != disks_info_ptr[i].devid)
				continue;

			total += chunks_info_ptr[j].size;
		}

		if (total > 0) {
			s = df_pretty_sizes(total, mode);
			printf("   %s\t%10s\n", disks_info_ptr[i].path, s);
		}
	}
}

static char **create_table(int columns, int rows)
{
	char **p = calloc(rows * columns, sizeof(char *));
	if (p)
		add_strings_to_free((char *)p);
	return p;
}

/*
 * If fmt  starts with '<', the text is left aligned; if fmt starts with
 * '>' the text is right aligned. If fmt is equal to '=' the text will
 * be replaced by a '=====' dimensioned in the basis of the column width
 */
static char *vprintf_table(char **p, int num_cols, int column, int row,
			  char *fmt, va_list ap)
{
	int idx =  num_cols*row+column;
	char *msg = calloc(100, sizeof(char));

	if (!msg)
		return NULL;

	add_strings_to_free(msg);
	p[idx] = msg;
	vsnprintf(msg, 99, fmt, ap);

	return msg;
}

static char *printf_table(char **p, int num_cols, int column, int row,
			  char *fmt, ...)
{
	va_list ap;
	char *ret;

	va_start(ap, fmt);
	ret = vprintf_table(p, num_cols, column, row, fmt, ap);
	va_end(ap);

	return ret;
}

static void dump_table(char **p, int ncols, int nrows)
{
	int	sizes[ncols];
	int	i, j;

	for (i = 0 ; i < ncols ; i++) {
		sizes[i] = 0;
		for (j = 0 ; j < nrows ; j++) {
			int idx = i + j*ncols;
			int s;

			if (!p[idx])
				continue;

			s = strlen(p[idx]) - 1;
			if (s < 1 || p[idx][0] == '=')
				continue;

			if (s > sizes[i])
				sizes[i] = s;
		}
	}


	for (j = 0 ; j < nrows ; j++) {
		for (i = 0 ; i < ncols ; i++) {

			int idx = i + j*ncols;

			if (!p[idx] || !strlen(p[idx])) {
				printf("%*s", sizes[i], "");
			} else if (p[idx][0] == '=') {
				int k;
				for (k = 0 ; k < sizes[i] ; k++)
					putchar('=');
			} else {
				printf("%*s",
					p[idx][0] == '<' ? -sizes[i] : sizes[i],
					p[idx]+1);
			}
			if (i != (ncols - 1))
				putchar(' ');
		}
		putchar('\n');
	}

}


static void _cmd_filesystem_disk_usage_tabular(int mode,
					struct btrfs_ioctl_space_args *sargs,
					struct chunk_info *chunks_info_ptr,
					int chunks_info_count,
					struct disk_info *disks_info_ptr,
					int disks_info_count)
{
	int i;
	u64 total_unused = 0;
	char **matrix = 0;
	int  ncols, nrows;

	ncols = sargs->total_spaces + 2;
	nrows = 2 + 1 + disks_info_count + 1 + 2;

	matrix = create_table(ncols, nrows);
	if (!matrix) {
		fprintf(stderr, "ERROR: not enough memory\n");
		return;
	}

	/* header */
	for (i = 0; i < sargs->total_spaces; i++) {
		char *description;

		u64 flags = sargs->spaces[i].flags;
		btrfs_flags2description(flags, &description);

		printf_table(matrix, ncols, 1+i, 0, "<%s", description);
	}

	for (i = 0; i < sargs->total_spaces; i++) {
		char *r_mode;

		u64 flags = sargs->spaces[i].flags;
		btrfs_flags2profile(flags, &r_mode);

		printf_table(matrix, ncols, 1+i, 1, "<%s", r_mode);
	}

	printf_table(matrix, ncols, 1+sargs->total_spaces, 1, "<Unallocated");

	/* body */
	for (i = 0 ; i < disks_info_count ; i++) {
		int k, col;
		char *p;

		u64  total_allocated = 0, unused;

		p = strrchr(disks_info_ptr[i].path, '/');
		if (!p)
			p = disks_info_ptr[i].path;
		else
			p++;

		printf_table(matrix, ncols, 0, i+3, "<%s",
				disks_info_ptr[i].path);

		for (col = 1, k = 0 ; k < sargs->total_spaces ; k++)  {
			u64	flags = sargs->spaces[k].flags;
			int	j;

			for (j = 0 ; j < chunks_info_count ; j++) {
				u64 size = chunks_info_ptr[j].size;

				if (chunks_info_ptr[j].type != flags ||
				    chunks_info_ptr[j].devid !=
					disks_info_ptr[i].devid)
						continue;

				printf_table(matrix, ncols, col, i+3,
					">%s", df_pretty_sizes(size, mode));
				total_allocated += size;
				col++;
				break;

			}
			if (j == chunks_info_count) {
				printf_table(matrix, ncols, col, i+3, ">-");
				col++;
			}
		}

		unused = get_partition_size(disks_info_ptr[i].path) -
				total_allocated;

		printf_table(matrix, ncols, sargs->total_spaces + 1, i + 3,
			       ">%s", df_pretty_sizes(unused, mode));
		total_unused += unused;

	}

	for (i = 0; i <= sargs->total_spaces; i++)
		printf_table(matrix, ncols, i + 1, disks_info_count + 3, "=");


	/* footer */
	printf_table(matrix, ncols, 0, disks_info_count + 4, "<Total");
	for (i = 0; i < sargs->total_spaces; i++)
		printf_table(matrix, ncols, 1 + i, disks_info_count + 4,
			">%s",
			df_pretty_sizes(sargs->spaces[i].total_bytes, mode));

	printf_table(matrix, ncols, sargs->total_spaces+1, disks_info_count+4,
		">%s", df_pretty_sizes(total_unused, mode));

	printf_table(matrix, ncols, 0, disks_info_count+5, "<Used");
	for (i = 0; i < sargs->total_spaces; i++)
		printf_table(matrix, ncols, 1+i, disks_info_count+5,
			">%s",
			df_pretty_sizes(sargs->spaces[i].used_bytes, mode));


	dump_table(matrix, ncols, nrows);

}

static void _cmd_filesystem_disk_usage_linear(int mode,
					struct btrfs_ioctl_space_args *sargs,
					struct chunk_info *info_ptr,
					int info_count,
					struct disk_info *disks_info_ptr,
					int disks_info_count)
{
	int i;

	for (i = 0; i < sargs->total_spaces; i++) {
		char *description;
		char *r_mode;

		u64 flags = sargs->spaces[i].flags;
		btrfs_flags2description(flags, &description);
		btrfs_flags2profile(flags, &r_mode);

		printf("%s,%s: Size:%s, Used:%s\n",
			description,
			r_mode,
			df_pretty_sizes(sargs->spaces[i].total_bytes ,
			    mode),
			df_pretty_sizes(sargs->spaces[i].used_bytes,
					mode));

		print_chunk_disks(flags, info_ptr, info_count,
				disks_info_ptr, disks_info_count,
				mode);
		printf("\n");

	}

	printf("Unallocated:\n");
	print_unused(info_ptr, info_count,
			disks_info_ptr, disks_info_count,
			mode);



}

static int _cmd_filesystem_disk_usage(int fd, char *path, int mode, int tabular)
{
	struct btrfs_ioctl_space_args *sargs = 0;
	int info_count = 0;
	struct chunk_info *info_ptr = 0;
	struct disk_info *disks_info_ptr = 0;
	int disks_info_count = 0;
	int ret = 0;

	if (load_chunk_info(fd, &info_ptr, &info_count) ||
	    load_disks_info(fd, &disks_info_ptr, &disks_info_count)) {
		ret = -1;
		goto exit;
	}

	if ((sargs = load_space_info(fd, path)) == NULL) {
		ret = -1;
		goto exit;
	}

	if (tabular)
		_cmd_filesystem_disk_usage_tabular(mode, sargs,
					info_ptr, info_count,
					disks_info_ptr, disks_info_count);
	else
		_cmd_filesystem_disk_usage_linear(mode, sargs,
					info_ptr, info_count,
					disks_info_ptr, disks_info_count);

exit:

	free_strings_to_free();
	if (sargs)
		free(sargs);
	if (disks_info_ptr)
		free(disks_info_ptr);
	if (info_ptr)
		free(info_ptr);

	return ret;
}

const char * const cmd_filesystem_disk_usage_usage[] = {
	"btrfs filesystem disk-usage [-b][-t] <path> [<path>..]",
	"Show in which disk the chunks are allocated.",
	"",
	"-b\tSet byte as unit",
	"-t\tShow data in tabular format",
	NULL
};

int cmd_filesystem_disk_usage(int argc, char **argv)
{

	int	flags =	DF_HUMAN_UNIT;
	int	i, more_than_one = 0;
	int	tabular = 0;

	optind = 1;
	while (1) {
		char	c = getopt(argc, argv, "bt");
		if (c < 0)
			break;
		switch (c) {
		case 'b':
			flags &= ~DF_HUMAN_UNIT;
			break;
		case 't':
			tabular = 1;
			break;
		default:
			usage(cmd_filesystem_disk_usage_usage);
		}
	}

	if (check_argc_min(argc - optind, 1)) {
		usage(cmd_filesystem_disk_usage_usage);
		return 21;
	}

	for (i = optind; i < argc ; i++) {
		int r, fd;
		if (more_than_one)
			printf("\n");

		fd = open_file_or_dir(argv[i]);
		if (fd < 0) {
			fprintf(stderr, "ERROR: can't access to '%s'\n",
				argv[1]);
			return 12;
		}
		r = _cmd_filesystem_disk_usage(fd, argv[i], flags, tabular);
		close(fd);

		if (r)
			return r;
		more_than_one = 1;

	}

	return 0;
}


