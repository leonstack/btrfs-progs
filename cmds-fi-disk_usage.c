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

#include "utils.h"
#include "kerncompat.h"
#include "ctree.h"

#include "commands.h"

#include "version.h"

#define DF_HUMAN_UNIT		(1<<0)

/* to store the information about the chunk */
struct chunk_info {
	u64	type;
	u64	size;
	u64	devid;
	int	processed:1;
};

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

