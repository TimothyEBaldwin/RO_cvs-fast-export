/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
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

#include <limits.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/resource.h>

#include "cvs.h"

/*
 * This code is somewhat complex because the natural order of operations
 * generated by the file-traversal operations in the rest of the code is
 * not even remotely like the canonical order generated by git-fast-export.
 * We want to emulate the latter in order to make regression-testing and
 * comparisons with other tools as easy as possible.
 */

struct mark {
    int external;
    bool emitted;
};
static struct mark *markmap;
static int seqno, mark;
static char blobdir[PATH_MAX];
static int export_total_commits;

static void save_status_end(void)
{
    if (!progress)
	return;
    else {
	time_t elapsed = time(NULL) - start_time;
	struct rusage rusage;

	(void)getrusage(RUSAGE_SELF, &rusage);
	progress_end("100%%, %d commits in %zdsec (%d commits/sec) using %ldKb.",
		     export_total_commits,
		     elapsed,
		     (int)(export_total_commits / elapsed),
		     rusage.ru_maxrss);
    }
}


void export_init(void)
{
    char *tmp = getenv("TMPDIR");
    if (tmp == NULL) 
    	tmp = "/tmp";
    seqno = mark = 0;
    snprintf(blobdir, sizeof(blobdir), "%s/cvs-fast-export-XXXXXXXXXX", tmp);
    if (mkdtemp(blobdir) == NULL)
	fatal_error("temp dir creation failed\n");
}

static char *blobfile(int m)
/* Random-access location of the blob corresponding to the specified serial */
{
    static char path[PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%d", blobdir, m);
    return path;
}

void export_blob(Node *node, void *buf, size_t len)
/* save the blob where it will be available for random access */
{
    FILE *wfp;
    
    node->file->serial = ++seqno;

    wfp = fopen(blobfile(seqno), "w");
    assert(wfp);
    fprintf(wfp, "data %zd\n", len);
    fwrite(buf, len, sizeof(char), wfp);
    fputc('\n', wfp);
    (void)fclose(wfp);
}

static void drop_path_component(char *string, const char *drop)
{
    char *c;
    int  m;
    m = strlen(drop);
    while ((c = strstr (string, drop)) &&
	   (c == string || c[-1] == '/'))
    {
	int l = strlen (c);
	memmove (c, c + m, l - m + 1);
    }
}

static char *export_filename(rev_file *file, int strip)
{
    static char name[PATH_MAX];
    int	    len;
    
    if (strlen (file->name) - strip >= MAXPATHLEN)
	fatal_error("File name %s\n too long\n", file->name);
    strcpy (name, file->name + strip);
	drop_path_component(name, "Attic/");
	drop_path_component(name, "RCS/");
    len = strlen (name);
    if (len > 2 && !strcmp (name + len - 2, ",v"))
	name[len-2] = '\0';

    if (strcmp(name, ".cvsignore") == 0)
    {
	name[1] = 'g';
	name[2] = 'i';
	name[3] = 't';
    }

    return name;
}

void export_wrap(void)
/* clean up after export, removing the blob storage */
{
    (void)fputs("done\n", stdout);
    (void)rmdir(blobdir);
}

static const char *utc_offset_timestamp(const time_t *timep, const char *tz)
{
    static char outbuf[BUFSIZ];
    struct tm *tm;
    char tzbuf[BUFSIZ];
    /* coverity[tainted_data] */
    char *oldtz = getenv("TZ");

    // make a copy in case original is clobbered
    if (oldtz != NULL)
	strncpy(tzbuf, oldtz, sizeof(tzbuf)-1);

    setenv("TZ", tz, 1);
    tzset();  // just in case ...

    tm = localtime(timep);
#ifndef __CYGWIN__
    strftime(outbuf, sizeof(outbuf), "%s %z", tm);
#else
		// Cygdwin doesn't have %s for strftime
    int x = sprintf(outbuf, "%li", *timep);
    strftime(outbuf + x, sizeof(outbuf) - x, " %z", tm);
#endif
    if (oldtz != NULL)
	setenv("TZ", tzbuf, 1);
    else
	unsetenv("TZ");
    tzset();

    return outbuf;
}

struct fileop {
    char op;
    mode_t mode;
    int serial;
    char path[PATH_MAX+1];	/* extra 1 for the sort sentinel */
};

static int fileop_sort(const void *a, const void *b)
/* sort fileops as git fast-export does */
{
    /* As it says, 'Handle files below a directory first, in case they are
     * all deleted and the directory changes to a file or symlink.'
     * Because this doesn't have to handle renames, just sort lexicographically
     * We append a sentinel to make sure "a/b/c" < "a/b" < "a".
     */
    struct fileop *oa = (struct fileop *)a;
    struct fileop *ob = (struct fileop *)b;
    int cmp;

    (void)strcat(oa->path, "/");
    (void)strcat(ob->path, "/");
    cmp = strcmp(oa->path, ob->path);
    oa->path[strlen(oa->path)-1] = '\0';
    ob->path[strlen(ob->path)-1] = '\0';
    return cmp;
}

#define display_date(c, m)	(force_dates ? ((m) * commit_time_window * 2) : (c)->date)

static void compute_parent_links(rev_commit *commit)
/* create reciprocal link pairs between file refs in a commit and its parent */
{
    rev_commit *parent = commit->parent; 
    int ncommit = 0, nparent = 0, maxmatch;
    rev_dir **ddir, **ddir2;
    rev_file **df, **df2;

    /*
     * This is the worst single computational hotspot in the code, accounting
     * for upwards of 30% of the running time.  Unfortunately, we absolutely
     * need these links to generate M and D ops with, and the setup is
     * intrinsically expensive.
     */

    for (ddir = commit->dirs; ddir < commit->dirs + commit->ndirs; ddir++) {
	for (df = (*ddir)->files; df < (*ddir)->files + (*ddir)->nfiles; df++) {
	    (*df)->u.other = NULL;
	    ncommit++;
	}
    }
    for (ddir2 = parent->dirs; ddir2 < parent->dirs + parent->ndirs; ddir2++) {
	for (df2 = (*ddir2)->files; df2 < (*ddir2)->files + (*ddir2)->nfiles; df2++) {
	    (*df2)->u.other = NULL;
	    nparent++;
	}
    }
    maxmatch = (nparent < ncommit) ? nparent : ncommit;
    for (ddir = commit->dirs; ddir < commit->dirs + commit->ndirs; ddir++) {
	for (df = (*ddir)->files; df < (*ddir)->files + (*ddir)->nfiles; df++) {
	    for (ddir2 = parent->dirs; ddir2 < parent->dirs + parent->ndirs; ddir2++) {
		for (df2 = (*ddir2)->files; df2 < (*ddir2)->files + (*ddir2)->nfiles; df2++) {
		    if ((*df)->name == (*df2)->name) {
			(*df)->u.other = *df2;
			(*df2)->u.other = *df;
			if (--maxmatch == 0)
			    return;
			break;
		    }
		}
	    }
	}
    }
}

static void export_commit(rev_commit *commit, char *branch, int strip, bool report)
/* export a commit (and the blobs it is the first to reference) */
{
#define OP_CHUNK	32
    cvs_author *author;
    char *full;
    char *email;
    char *timezone;
    char *revpairs = NULL;
    size_t revpairsize = 0;
    const char *ts;
    time_t ct;
    rev_file	*f;
    int		i, j;
    struct fileop *operations, *op, *op2;
    int noperations;

    if (reposurgeon)
    {
	revpairs = xmalloc((revpairsize = 1024), "revpair allocation");
	revpairs[0] = '\0';
    }

    /*
     * Precompute mutual parent-child pointers.
     */
    if (commit->parent) 
	compute_parent_links(commit);

    noperations = OP_CHUNK;
    op = operations = xmalloc(sizeof(struct fileop) * noperations, "fileop allocation");
    for (i = 0; i < commit->ndirs; i++) {
	rev_dir	*dir = commit->dirs[i];
	
	for (j = 0; j < dir->nfiles; j++) {
	    char *stripped;
	    bool present, changed;
	    f = dir->files[j];
	    stripped = export_filename(f, strip);
	    present = false;
	    changed = false;
	    if (commit->parent) {
		present = (f->u.other != NULL);
		changed = present && (f->serial != f->u.other->serial);
	    }
	    if (!present || changed) {

		op->op = 'M';
		// git fast-import only supports 644 and 755 file modes
		if (f->mode & 0100)
			op->mode = 0755;
		else
			op->mode = 0644;
		op->serial = f->serial;
		(void)strncpy(op->path, stripped, PATH_MAX-1);
		op++;
		if (op == operations + noperations)
		{
		    noperations += OP_CHUNK;
		    operations = xrealloc(operations,
				sizeof(struct fileop) * noperations, __func__);
		    if (operations == NULL) {
			free(operations);	/* pacifies cppcheck */
			exit(1);
		    }
		    // realloc can move operations
		    op = operations + noperations - OP_CHUNK;
		}

		if (revision_map || reposurgeon) {
		    char *fr = stringify_revision(stripped, " ", &f->number);
		    if (report && revision_map)
			fprintf(revision_map, "%s :%d\n", fr, markmap[f->serial].external);
		    if (reposurgeon)
		    {
			if (strlen(revpairs) + strlen(fr) + 2 > revpairsize)
			{
			    revpairsize *= 2;
			    revpairs = xrealloc(revpairs, revpairsize, "revpair allocation");
			}
			strcat(revpairs, fr);
			strcat(revpairs, "\n");
		    }
		}
	    }
	}
    }

    if (commit->parent)
    {
	for (i = 0; i < commit->parent->ndirs; i++) {
	    rev_dir	*dir = commit->parent->dirs[i];

	    for (j = 0; j < dir->nfiles; j++) {
		bool present;
		present = false;
		f = dir->files[j];
		present = (f->u.other != NULL);
		if (!present) {
		    op->op = 'D';
		    (void)strncpy(op->path, 
				  export_filename(f, strip),
				  PATH_MAX-1);
		    op++;
		    if (op == operations + noperations)
		    {
			noperations += OP_CHUNK;
			operations = xrealloc(operations,
					sizeof(struct fileop) * noperations,
					__func__);
			if (operations == NULL) {
			    free(operations);	/* pacifies cppcheck */
			    exit(1);
			}
			// realloc can move operations
			op = operations + noperations - OP_CHUNK;
		    }
		}
	    }
	}
    }

    for (op2 = operations; op2 < op; op2++)
    {
	if (op2->op == 'M' && !markmap[op2->serial].emitted)
	{
	    markmap[op2->serial].external = ++mark;
	    if (report) {
		char *fn = blobfile(op2->serial);
		FILE *rfp = fopen(fn, "r");
		if (rfp)
		{
		    int c;
		    printf("blob\nmark :%d\n", mark);
		    while ((c = fgetc(rfp)) != EOF)
			putchar(c);
		    (void) unlink(fn);
		    markmap[op2->serial].emitted = true;
		    (void)fclose(rfp);
		}
	    }
	}
    }

    qsort((void *)operations, op2 - op, sizeof(struct fileop), fileop_sort); 

    author = fullname(commit->author);
    if (!author) {
	full = commit->author;
	email = commit->author;
	timezone = "UTC";
    } else {
	full = author->full;
	email = author->email;
	timezone = author->timezone ? author->timezone : "UTC";
    }

    if (report)
	printf("commit %s%s\n", branch_prefix, branch);
    markmap[++seqno].external = ++mark;
    if (report)
	printf("mark :%d\n", mark);
    commit->serial = seqno;
    if (report) {
	ct = display_date(commit, mark);
	ts = utc_offset_timestamp(&ct, timezone);
	//printf("author %s <%s> %s\n", full, email, ts);
	printf("committer %s <%s> %s\n", full, email, ts);
	printf("data %zd\n%s\n", strlen(commit->log), commit->log);
	if (commit->parent)
	    printf("from :%d\n", markmap[commit->parent->serial].external);

	for (op2 = operations; op2 < op; op2++)
	{
	    assert(op2->op == 'M' || op2->op == 'D');
	    if (op2->op == 'M')
		printf("M 100%o :%d %s\n", 
		       op2->mode, 
		       markmap[op2->serial].external, 
		       op2->path);
	    if (op2->op == 'D')
		printf("D %s\n", op2->path);
	}
    }
    free(operations);

    if (reposurgeon) 
    {
	if (report)
	    printf("property cvs-revision %zd %s", strlen(revpairs), revpairs);
	free(revpairs);
    }

    if (report)
	printf ("\n");
#undef OP_CHUNK
    }

static int export_ncommit(rev_list *rl)
/* return a count of converted commits */
{
    rev_ref	*h;
    rev_commit	*c;
    int		n = 0;
    
    for (h = rl->heads; h; h = h->next) {
	if (h->tail)
	    continue;
	for (c = h->commit; c; c = c->parent) {
	    n++;
	    if (c->tail)
		break;
	}
    }
    return n;
}

struct commit_seq {
    rev_commit *commit;
    rev_ref *head;
    bool realized;
};

static int sort_by_date(const void *ap, const void *bp)
{
    struct commit_seq *ac = (struct commit_seq *)ap;
    struct commit_seq *bc = (struct commit_seq *)bp;

    return ac->commit->date - bc->commit->date;
}

bool export_commits(rev_list *rl, int strip, time_t fromtime, bool progress)
/* export a revision list as a git fast-import stream in canonical order */
{
    rev_ref *h;
    Tag *t;
    rev_commit *c;
    int n;
    size_t extent;

    export_total_commits = export_ncommit (rl);
    /* the +1 is because mark indices are 1-origin, slot 0 always empty */
    extent = sizeof(struct mark) * (seqno + export_total_commits + 1);
    markmap = (struct mark *)xmalloc(extent, "markmap allocation");
    memset(markmap, '\0', extent);

    progress_begin("Save", export_total_commits);

    if (branchorder) {
	/*
	 * Dump by branch order, not by commit date.  Slightly faster and
	 * less memory-intensive, but (a) incremental dump won't work, and
	 * (b) it's not git-fast-export  canonical form and cannot be 
	 * directly compared to the output of other tools.
	 */
	rev_commit **history;
	int alloc, i;

	for (h = rl->heads; h; h = h->next) {
	    if (!h->tail) {
		// We need to export commits in reverse order; so
		// first of all, we convert the linked-list given by
		// h->commit into the array "history".
		history = NULL;
		alloc = 0;
		for (c=h->commit, n=0; c; c=(c->tail ? NULL : c->parent), n++) {
		    if (n >= alloc) {
			alloc += 1024;
			history = (rev_commit **)xrealloc(history, alloc *sizeof(rev_commit*), "export");
		    }
		    history[n] = c;
		}

		// Now walk the history array in reverse order and export the
		// commits, along with any matching tags.
		for (i=n-1; i>=0; i--) {
		    export_commit (history[i], h->name, strip, true);
		    progress_step();
		    for (t = all_tags; t; t = t->next)
			if (t->commit == history[i])
			    printf("reset refs/tags/%s\nfrom :%d\n\n", t->name, markmap[history[i]->serial].external);
		}

		free(history);
	    }
	}
    }
    else 
    {
	/*
	 * Dump in strict git-fast-export order.
	 *
	 * Commits are in reverse order on per-branch lists.  The branches
	 * have to ship in their current order, otherwise some marks may not 
	 * be resolved.
	 *
	 * Dump them all into a common array necause (a) we're going to
	 * need to ship them back to front, and (b) we'd prefer to ship
	 * them in canonical order by commit date rather than ordered by
	 * branches.
	 *
	 * But there's a hitch; the branches themselves need to be dumped
	 * in forward order, otherwise not all ancestor marks will be defined.
	 * Since the branch commits need to be dumped in reverse, the easiest
	 * way to arrange this is to reverse the branches in the array, fill
	 * the array in forward order, and dump it forward order.
	 */
	struct commit_seq *history, *hp;
	bool sortable;
	int branchbase;

	history = (struct commit_seq *)xcalloc(export_total_commits, 
					       sizeof(struct commit_seq),
					       "export");
	branchbase = 0;
	for (h = rl->heads; h; h = h->next) {
	    if (!h->tail) {
		int i = 0, branchlength = 0;
		for (c = h->commit; c; c = (c->tail ? NULL : c->parent))
		    branchlength++;
		for (c = h->commit; c; c = (c->tail ? NULL : c->parent)) {
		    /* copy commits in reverse order into this branch's span */
		    n = branchbase + branchlength - (i + 1);
		    history[n].commit = c;
		    history[n].head = h;
		    i++;
		}
		branchbase += branchlength;
	    }
	}
 
	/* 
	 * Check that the topo order is consistent with time order.
	 * If so, we can sort commits by date without worrying that
	 * we'll try to ship a mark before it's defined.
	 */
	sortable = true;
	for (hp = history; hp < history + export_total_commits; hp++) {
	    if (hp->commit->parent && hp->commit->parent->date > hp->commit->date) {
		sortable = false;
		announce("some parent commits are younger than children.\n");
		break;
	    }
	}
	if (sortable)
	    qsort((void *)history, 
		  export_total_commits, sizeof(struct commit_seq),
		  sort_by_date);

	for (hp = history; hp < history + export_total_commits; hp++) {
	    bool report = true;
	    if (fromtime > 0) {
		if (fromtime >= display_date(hp->commit, mark+1)) {
		    report = false;
		} else if (!hp->realized) {
		    struct commit_seq *lp;
		    if (hp->commit->parent != NULL && display_date(hp->commit->parent, markmap[hp->commit->parent->serial].external) < fromtime)
			(void)printf("from %s%s^0\n\n", branch_prefix, hp->head->name);
		    for (lp = hp; lp < history + export_total_commits; lp++) {
			if (lp->head == hp->head) {
			    lp->realized = true;
			}
		    }
		}
	    }
	    progress_jump(hp - history);
	    export_commit(hp->commit, hp->head->name, strip, report);
	    for (t = all_tags; t; t = t->next)
		if (t->commit == hp->commit)
		    printf("reset refs/tags/%s\nfrom :%d\n\n", t->name, markmap[hp->commit->serial].external);
	}

	free(history);
    }

    for (h = rl->heads; h; h = h->next) {
	printf("reset %s%s\nfrom :%d\n\n", 
	       branch_prefix, 
	       h->name, 
	       markmap[h->commit->serial].external);
    }
    free(markmap);

    save_status_end(); /* calls progress_end() */

    return true;
}

#define PROGRESS_LEN	20

void load_status (char *name)
{
    int	spot = load_current_file * PROGRESS_LEN / load_total_files;
    int	    s;
    int	    l;

    l = strlen (name);
    if (l > 35) name += l - 35;

    fprintf (STATUS, "\rLoad: %35.35s ", name);
    for (s = 0; s < PROGRESS_LEN + 1; s++)
	putc (s == spot ? '*' : '.', STATUS);
    fprintf (STATUS, " %5d of %5d ", load_current_file, load_total_files);
    fflush (STATUS);
}

void load_status_next (void)
{
    fprintf (STATUS, "\n");
    fflush (STATUS);
}

/* end */
