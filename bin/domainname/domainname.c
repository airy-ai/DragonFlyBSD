/*
 * Copyright (c) 1988, 1993
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
 * 3. Neither the name of the University nor the names of its contributors
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
 * @(#) Copyright (c) 1988, 1993 The Regents of the University of California.  All rights reserved.
 * @(#)hostname.c	8.1 (Berkeley) 5/31/93
 * $FreeBSD: src/bin/domainname/domainname.c,v 1.12.2.1 2001/08/01 02:32:16 obrien Exp $
 * $DragonFly: src/bin/domainname/domainname.c,v 1.7 2005/03/01 23:12:37 liamfoy Exp $
 */

#include <sys/param.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void	usage(void);

int
main(int argc, char **argv)
{
	int ch;
	char domainname[MAXHOSTNAMELEN];

	while ((ch = getopt(argc, argv, "")) != -1) {
		switch (ch) {
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 1)
		usage();

	if (argv[0] != NULL) {
		size_t len = strlen(argv[0]);
		if (len > MAXHOSTNAMELEN)
			errx(1, "domain name too long");

		if (setdomainname(*argv, len))
			err(1, "setdomainname");
	} else {
		if (getdomainname(domainname, sizeof(domainname)))
			err(1, "getdomainname");
		domainname[MAXHOSTNAMELEN-1] = '\0';
		printf("%s\n", domainname);
	}
	exit(0);
}

static void
usage(void)
{
	fprintf(stderr, "usage: domainname [ypdomain]\n");
	exit(1);
}
