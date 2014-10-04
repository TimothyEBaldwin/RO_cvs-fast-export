/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as 
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 */

/*
 * Pack a collection of files into a space-efficient representation in
 * which directories are coalesced.
 */

#include "cvs.h"

#define REV_DIR_HASH	288361

typedef struct _rev_dir_hash {
    struct _rev_dir_hash    *next;
    unsigned long	    hash;
    rev_dir		    dir;
} rev_dir_hash;

static rev_dir_hash	*buckets[REV_DIR_HASH];

static 
unsigned long hash_files(rev_file **files, int nfiles)
/* hash a file list so we can recognize it cheaply */
{
    unsigned long   h = 0;
    int		    i;

    for (i = 0; i < nfiles; i++)
	h = ((h << 1) | (h >> (sizeof(h) * 8 - 1))) ^ (unsigned long) files[i];
    return h;
}

static rev_dir *
rev_pack_dir(rev_file **files, int nfiles)
/* pack a collection of file revisions for soace effeciency */
{
    unsigned long   hash = hash_files(files, nfiles);
    rev_dir_hash    **bucket = &buckets[hash % REV_DIR_HASH];
    rev_dir_hash    *h;

    /* avoid packing a file list if we've done it before */ 
    for (h = *bucket; h; h = h->next) {
	if (h->hash == hash && h->dir.nfiles == nfiles &&
	    !memcmp(files, h->dir.files, nfiles * sizeof(rev_file *)))
	{
	    return &h->dir;
	}
    }
    h = xmalloc(sizeof(rev_dir_hash) + nfiles * sizeof(rev_file *),
		 __func__);
    h->next = *bucket;
    *bucket = h;
    h->hash = hash;
    h->dir.nfiles = nfiles;
    memcpy(h->dir.files, files, nfiles * sizeof(rev_file *));
    return &h->dir;
}

static int compare_rev_file(const void *a, const void *b)
{
    rev_file **ap = (rev_file **)a;
    rev_file **bp = (rev_file **)b;

#ifdef ORDERDEBUG
    fprintf(stderr, "Comparing %s with %s\n", 
	    (*ap)->file_name, (*bp)->file_name);
#endif /* ORDERDEBUG */
    return strcmp((*ap)->file_name, (*bp)->file_name);
}

/* entry points begin here */

static int	    sds = 0;
static rev_dir **rds = NULL;

void
rev_free_dirs(void)
{
    unsigned long   hash;

    for (hash = 0; hash < REV_DIR_HASH; hash++) {
	rev_dir_hash    **bucket = &buckets[hash];
	rev_dir_hash	*h;

	while ((h = *bucket)) {
	    *bucket = h->next;
	    free(h);
	}
    }
    if (rds) {
	free(rds);
	rds = NULL;
	sds = 0;
    }
}

rev_dir **
rev_pack_files(rev_file **files, int nfiles, int *ndr)
{
    char    *dir = 0;
    char    *slash;
    int	    dirlen = 0;
    int	    i;
    int	    start = 0;
    int	    nds = 0;
    rev_dir *rd;
    
    if (!rds)
	rds = xmalloc((sds = 16) * sizeof(rev_dir *), __func__);
 
#ifdef ORDERDEBUG
    fputs("Packing:\n", stderr);
    {
	rev_file **s;

	for (s = files; s < files + nfiles; s++)
	    fprintf(stderr, "rev_file: %s\n", (*s)->file_name);
    }
#endif /* ORDERDEBUG */

    /*
     * The purpose of this sort is to rearrange the files in
     * directory-path order so we get the longesr possible 
     * runs of common directory prefixes, and thus maximum 
     * space-saving effect out of the next step.  This reduces
     * working-set size at the expense of the sort runtime.
     */
    qsort(files, nfiles, sizeof(rev_file *), compare_rev_file);

    /* pull out directories */
    for (i = 0; i < nfiles; i++) {
	if (!dir || strncmp(files[i]->file_name, dir, dirlen) != 0)
	{
	    if (i > start) {
		rd = rev_pack_dir(files + start, i - start);
		if (nds == sds) {
		    rds = xrealloc(rds, (sds *= 2) * sizeof(rev_dir *),
		    		__func__);
		}
		rds[nds++] = rd;
	    }
	    start = i;
	    dir = files[i]->file_name;
	    slash = strrchr(dir, '/');
	    if (slash)
		dirlen = slash - dir;
	    else
		dirlen = 0;
	}
    }
    rd = rev_pack_dir(files + start, nfiles - start);
    if (nds == sds) {
	/* coverity[sizecheck] Coverity has a bug here */
	rds = xrealloc(rds, (sds *= 2) * sizeof(rev_dir *), __func__);
    }
    rds[nds++] = rd;
    
    *ndr = nds;
    return rds;
}

// end
