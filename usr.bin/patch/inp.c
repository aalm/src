/*	$OpenBSD: inp.c,v 1.23 2003/08/05 18:13:43 otto Exp $	*/

#ifndef lint
static const char     rcsid[] = "$OpenBSD: inp.c,v 1.23 2003/08/05 18:13:43 otto Exp $";
#endif /* not lint */

#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <ctype.h>
#include <libgen.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "util.h"
#include "pch.h"
#include "inp.h"


/* Input-file-with-indexable-lines abstract type */

static off_t	i_size;		/* size of the input file */
static char	*i_womp;	/* plan a buffer for entire file */
static char	**i_ptr;	/* pointers to lines in i_womp */

static int	tifd = -1;	/* plan b virtual string array */
static char	*tibuf[2];	/* plan b buffers */
static LINENUM	tiline[2] = {-1, -1};	/* 1st line in each buffer */
static LINENUM	lines_per_buf;	/* how many lines per buffer */
static int	tireclen;	/* length of records in tmp file */

static bool	rev_in_string(const char *);

/* returns false if insufficient memory */
static bool	plan_a(const char *);

static void	plan_b(const char *);

/* New patch--prepare to edit another file. */

void
re_input(void)
{
	if (using_plan_a) {
		i_size = 0;
		free(i_ptr);
		i_ptr = NULL;
		if (i_womp != NULL) {
			munmap(i_womp, i_size);
			i_womp = NULL;
		}
	} else {
		using_plan_a = true;	/* maybe the next one is smaller */
		close(tifd);
		tifd = -1;
		free(tibuf[0]);
		free(tibuf[1]);
		tibuf[0] = tibuf[1] = NULL;
		tiline[0] = tiline[1] = -1;
		tireclen = 0;
	}
}

/* Constuct the line index, somehow or other. */

void
scan_input(const char *filename)
{
	if (!plan_a(filename))
		plan_b(filename);
	if (verbose) {
		say("Patching file %s using Plan %s...\n", filename,
		    (using_plan_a ? "A" : "B"));
	}
}

/* Try keeping everything in memory. */

static bool
plan_a(const char *filename)
{
	int		ifd, statfailed;
	char		*p, *s, lbuf[MAXLINELEN];
	LINENUM		iline;
	struct stat	filestat;
	off_t		i;
	ptrdiff_t	sz;

#ifdef DEBUGGING
	if (debug & 8)
		return false;
#endif

	if (filename == NULL || *filename == '\0')
		return false;

	statfailed = stat(filename, &filestat);
	if (statfailed && ok_to_create_file) {
		if (verbose)
			say("(Creating file %s...)\n", filename);

		/*
		 * in check_patch case, we still display `Creating file' even
		 * though we're not. The rule is that -C should be as similar
		 * to normal patch behavior as possible
		 */
		if (check_only)
			return true;
		makedirs(filename, true);
		close(creat(filename, 0666));
		statfailed = stat(filename, &filestat);
	}
	if (statfailed && check_only)
		fatal("%s not found, -C mode, can't probe further\n", filename);
	/* For nonexistent or read-only files, look for RCS or SCCS versions.  */
	if (statfailed ||
	    /* No one can write to it.  */
	    (filestat.st_mode & 0222) == 0 ||
	    /* I can't write to it.  */
	    ((filestat.st_mode & 0022) == 0 && filestat.st_uid != getuid())) {
		char	*cs = NULL, *filebase, *filedir;
		struct stat	cstat;

		filebase = basename(filename);
		filedir = dirname(filename);

		/* Leave room in lbuf for the diff command.  */
		s = lbuf + 20;

#define try(f, a1, a2, a3) \
	(snprintf(s, sizeof lbuf - 20, f, a1, a2, a3), stat(s, &cstat) == 0)

		if (try("%s/RCS/%s%s", filedir, filebase, RCSSUFFIX) ||
		    try("%s/RCS/%s%s", filedir, filebase, "") ||
		    try("%s/%s%s", filedir, filebase, RCSSUFFIX)) {
			snprintf(buf, sizeof buf, CHECKOUT, filename);
			snprintf(lbuf, sizeof lbuf, RCSDIFF, filename);
			cs = "RCS";
		} else if (try("%s/SCCS/%s%s", filedir, SCCSPREFIX, filebase) ||
		    try("%s/%s%s", filedir, SCCSPREFIX, filebase)) {
			snprintf(buf, sizeof buf, GET, s);
			snprintf(lbuf, sizeof lbuf, SCCSDIFF, s, filename);
			cs = "SCCS";
		} else if (statfailed)
			fatal("can't find %s\n", filename);
		/*
		 * else we can't write to it but it's not under a version
		 * control system, so just proceed.
		 */
		if (cs) {
			if (!statfailed) {
				if ((filestat.st_mode & 0222) != 0)
					/* The owner can write to it.  */
					fatal("file %s seems to be locked "
					    "by somebody else under %s\n",
					    filename, cs);
				/*
				 * It might be checked out unlocked.  See if
				 * it's safe to check out the default version
				 * locked.
				 */
				if (verbose)
					say("Comparing file %s to default "
					    "%s version...\n",
					    filename, cs);
				if (system(lbuf))
					fatal("can't check out file %s: "
					    "differs from default %s version\n",
					    filename, cs);
			}
			if (verbose)
				say("Checking out file %s from %s...\n",
				    filename, cs);
			if (system(buf) || stat(filename, &filestat))
				fatal("can't check out file %s from %s\n",
				    filename, cs);
		}
	}
	filemode = filestat.st_mode;
	if (!S_ISREG(filemode))
		fatal("%s is not a normal file--can't patch\n", filename);
	i_size = filestat.st_size;
	if (out_of_mem) {
		set_hunkmax();	/* make sure dynamic arrays are allocated */
		out_of_mem = false;
		return false;	/* force plan b because plan a bombed */
	}
	if (i_size > SIZE_MAX) {
		say("block too large to mmap\n");
 		return false;
	}
	if ((ifd = open(filename, O_RDONLY)) < 0)
		pfatal("can't open file %s", filename);

	i_womp = mmap(NULL, i_size, PROT_READ, MAP_FILE, ifd, 0);
	if (i_womp == MAP_FAILED) {
		perror("mmap failed");
		i_womp = NULL;
		return false;
	}

	close(ifd);

	/* count the lines in the buffer so we know how many pointers we need */
	iline = 0;

	/* test for NUL too, to maintain the behavior of the original code */
	for (i = 0; i < i_size && i_womp[i] != '\0'; i++) {
		if (i_womp[i] == '\n')
 			iline++;
	}
	if (i_size > 0 && i_womp[i_size - 1] != '\n')
		iline++;


	i_ptr = (char **) malloc((iline + 2) * sizeof(char *));

 	if (i_ptr == NULL) {	/* shucks, it was a near thing */
		munmap(i_womp, i_size);
		i_womp = NULL;
 		return false;
 	}

	/* now scan the buffer and build pointer array */
	iline = 1;
	i_ptr[iline] = i_womp;
	/* test for NUL too, to maintain the behavior of the original code */
	for (s = i_womp, i = 0; i < i_size && *s != '\0'; s++, i++) {
		if (*s == '\n')
			i_ptr[++iline] = s + 1;	/* these are NOT NUL terminated */
	}
	/* if the last line contains no EOL, append one */
	if (i_size > 0 && i_womp[i_size - 1] != '\n') {
		/* fix last line */
		sz = s - i_ptr[iline];
		p = malloc(sz + 1);
		if (p == NULL) {
			free(i_ptr);
			i_ptr = NULL;
			munmap(i_womp, i_size);
			i_womp = NULL;
			return false;
		}
	
		memcpy(p, i_ptr[iline], sz);
		p[sz] = '\n';
		i_ptr[iline] = p;
		/* count the extra line and make it point to some valid mem */
		i_ptr[++iline] = "";
	}

	input_lines = iline - 1;

	/* now check for revision, if any */

	if (revision != NULL) {
		if (!rev_in_string(i_womp)) {
			if (force) {
				if (verbose)
					say("Warning: this file doesn't appear "
					    "to be the %s version--patching anyway.\n",
					    revision);
			} else if (batch) {
				fatal("this file doesn't appear to be the "
				    "%s version--aborting.\n",
				    revision);
			} else {
				ask("This file doesn't appear to be the "
				    "%s version--patch anyway? [n] ",
				    revision);
				if (*buf != 'y')
					fatal("aborted\n");
			}
		} else if (verbose)
			say("Good.  This file appears to be the %s version.\n",
			    revision);
	}
	return true;		/* plan a will work */
}

/* Keep (virtually) nothing in memory. */

static void
plan_b(const char *filename)
{
	FILE	*ifp;
	int	i = 0, maxlen = 1;
	bool	found_revision = (revision == NULL);

	using_plan_a = false;
	if ((ifp = fopen(filename, "r")) == NULL)
		pfatal("can't open file %s", filename);
	(void) unlink(TMPINNAME);
	if ((tifd = open(TMPINNAME, O_EXCL | O_CREAT | O_WRONLY, 0666)) < 0)
		pfatal("can't open file %s", TMPINNAME);
	while (fgets(buf, sizeof buf, ifp) != NULL) {
		if (revision != NULL && !found_revision && rev_in_string(buf))
			found_revision = true;
		if ((i = strlen(buf)) > maxlen)
			maxlen = i;	/* find longest line */
	}
	if (revision != NULL) {
		if (!found_revision) {
			if (force) {
				if (verbose)
					say("Warning: this file doesn't appear "
					    "to be the %s version--patching anyway.\n",
					    revision);
			} else if (batch) {
				fatal("this file doesn't appear to be the "
				    "%s version--aborting.\n",
				    revision);
			} else {
				ask("This file doesn't appear to be the %s "
				    "version--patch anyway? [n] ",
				    revision);
				if (*buf != 'y')
					fatal("aborted\n");
			}
		} else if (verbose)
			say("Good.  This file appears to be the %s version.\n",
			    revision);
	}
	fseek(ifp, 0L, SEEK_SET);	/* rewind file */
	lines_per_buf = BUFFERSIZE / maxlen;
	tireclen = maxlen;
	tibuf[0] = malloc(BUFFERSIZE + 1);
	if (tibuf[0] == NULL)
		fatal("out of memory\n");
	tibuf[1] = malloc(BUFFERSIZE + 1);
	if (tibuf[1] == NULL)
		fatal("out of memory\n");
	for (i = 1;; i++) {
		if (!(i % lines_per_buf))	/* new block */
			if (write(tifd, tibuf[0], BUFFERSIZE) < BUFFERSIZE)
				pfatal("can't write temp file");
		if (fgets(tibuf[0] + maxlen * (i % lines_per_buf),
		    maxlen + 1, ifp) == NULL) {
			input_lines = i - 1;
			if (i % lines_per_buf)
				if (write(tifd, tibuf[0], BUFFERSIZE) < BUFFERSIZE)
					pfatal("can't write temp file");
			break;
		}
	}
	fclose(ifp);
	close(tifd);
	if ((tifd = open(TMPINNAME, O_RDONLY)) < 0)
		pfatal("can't reopen file %s", TMPINNAME);
}

/*
 * Fetch a line from the input file, \n terminated, not necessarily \0.
 */
char *
ifetch(LINENUM line, int whichbuf)
{
	if (line < 1 || line > input_lines) {
		if (warn_on_invalid_line) {
			say("No such line %ld in input file, ignoring\n", line);
			warn_on_invalid_line = false;
		}
		return NULL;
	}
	if (using_plan_a)
		return i_ptr[line];
	else {
		LINENUM	offline = line % lines_per_buf;
		LINENUM	baseline = line - offline;

		if (tiline[0] == baseline)
			whichbuf = 0;
		else if (tiline[1] == baseline)
			whichbuf = 1;
		else {
			tiline[whichbuf] = baseline;

			lseek(tifd, (off_t) (baseline / lines_per_buf *
			    BUFFERSIZE), SEEK_SET);

			if (read(tifd, tibuf[whichbuf], BUFFERSIZE) < 0)
				pfatal("error reading tmp file %s", TMPINNAME);
		}
		return tibuf[whichbuf] + (tireclen * offline);
	}
}

/*
 * True if the string argument contains the revision number we want.
 */
static bool
rev_in_string(const char *string)
{
	const char	*s;
	int		patlen;

	if (revision == NULL)
		return true;
	patlen = strlen(revision);
	if (strnEQ(string, revision, patlen) && isspace(string[patlen]))
		return true;
	for (s = string; *s; s++) {
		if (isspace(*s) && strnEQ(s + 1, revision, patlen) &&
		    isspace(s[patlen + 1])) {
			return true;
		}
	}
	return false;
}
