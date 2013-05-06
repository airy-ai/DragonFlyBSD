/*
 * Copyright (c) 1980, 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * @(#) Copyright (c) 1980, 1988, 1993 The Regents of the University of California.  All rights reserved.
 * @(#)from.c	8.1 (Berkeley) 6/6/93
 * $FreeBSD: src/usr.bin/from/from.c,v 1.8.2.1 2000/10/06 08:19:19 ru Exp $
 * $DragonFly: src/usr.bin/from/from.c,v 1.5 2005/04/22 15:36:39 liamfoy Exp $
 */

#include <sys/types.h>

#include <ctype.h>
#include <err.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <paths.h>
#include <string.h>
#include <unistd.h>

static int	match(char *, char *);
static void	usage(void);

int
main(int argc, char **argv)
{
	struct passwd *pwd;
	int ch, count, newline;
	const char *file;
	char *sender, *p;
#if MAXPATHLEN > BUFSIZ
	char buf[MAXPATHLEN];
#else
	char buf[BUFSIZ];
#endif

	file = sender = NULL;
	count = -1;
	while ((ch = getopt(argc, argv, "cf:s:")) != -1)
		switch (ch) {
		case 'c':
			count = 0;
			break;
		case 'f':
			file = optarg;
			break;
		case 's':
			sender = optarg;
			for (p = sender; *p; ++p)
				*p = tolower(*p);
			break;
		case '?':
		default:
			usage();
		}
	argv += optind;

	if (file == NULL) {
		if (*argv) {
			snprintf(buf, sizeof(buf), "%s/%s", _PATH_MAILDIR, *argv);
			file  = buf;
		} else {
			if ((file = getenv("MAIL")) == NULL) {
				if (!(pwd = getpwuid(getuid())))
					errx(1, "no password file entry for you");
				file = pwd->pw_name;
				snprintf(buf, sizeof(buf),
				    "%s/%s", _PATH_MAILDIR, file);
				file = buf;
			}
		}
	}

	/* read from stdin */
	if (strcmp(file, "-") == 0) {
	} 
	else if (freopen(file, "r", stdin) == NULL) {
		err(1, "can't read %s", file);
	}
	for (newline = 1; fgets(buf, sizeof(buf), stdin);) {
		if (*buf == '\n') {
			newline = 1;
			continue;
		}
		if (newline && !strncmp(buf, "From ", 5) &&
		    (!sender || match(buf + 5, sender))) {
			if (count != -1)
				count++;
			else
				printf("%s", buf);
		}
		newline = 0;
	}
	if (count != -1)
		printf("There %s %d message%s in your incoming mailbox.\n",
		    count == 1 ? "is" : "are", count, count == 1 ? "" : "s"); 
	exit(0);
}

static void
usage(void)
{
	fprintf(stderr, "usage: from [-c] [-f file] [-s sender] [user]\n");
	exit(1);
}

static int
match(char *line, char *sender)
{
	char ch, pch, first, *p, *t;

	for (first = *sender++;;) {
		if (isspace(ch = *line))
			return(0);
		++line;
		ch = tolower(ch);
		if (ch != first)
			continue;
		for (p = sender, t = line;;) {
			if (!(pch = *p++))
				return(1);
			ch = tolower(*t++);
			if (ch != pch)
				break;
		}
	}
	/* NOTREACHED */
}
