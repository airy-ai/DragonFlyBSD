/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993, 1995
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
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
 *	@(#)ufs_ihash.c	8.7 (Berkeley) 5/17/95
 * $FreeBSD: src/sys/ufs/ufs/ufs_ihash.c,v 1.20 1999/08/28 00:52:29 peter Exp $
 * $DragonFly: src/sys/vfs/ufs/ufs_ihash.c,v 1.6 2003/08/07 21:17:44 dillon Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/proc.h>

#include "quota.h"
#include "inode.h"
#include "ufs_extern.h"

static MALLOC_DEFINE(M_UFSIHASH, "UFS ihash", "UFS Inode hash tables");
/*
 * Structures associated with inode cacheing.
 */
static LIST_HEAD(ihashhead, inode) *ihashtbl;
static u_long	ihash;		/* size of hash table - 1 */
#define	INOHASH(device, inum)	(&ihashtbl[(minor(device) + (inum)) & ihash])
static struct lwkt_token ufs_ihash_token;

/*
 * Initialize inode hash table.
 */
void
ufs_ihashinit()
{
	ihashtbl = hashinit(desiredvnodes, M_UFSIHASH, &ihash);
	lwkt_inittoken(&ufs_ihash_token);
}

/*
 * Use the device/inum pair to find the incore inode, and return a pointer
 * to it. If it is in core, return it, even if it is locked.
 */
struct vnode *
ufs_ihashlookup(dev, inum)
	dev_t dev;
	ino_t inum;
{
	struct inode *ip;

	lwkt_gettoken(&ufs_ihash_token);
	for (ip = INOHASH(dev, inum)->lh_first; ip; ip = ip->i_hash.le_next)
		if (inum == ip->i_number && dev == ip->i_dev)
			break;
	lwkt_reltoken(&ufs_ihash_token);

	if (ip)
		return (ITOV(ip));
	return (NULLVP);
}

/*
 * Use the device/inum pair to find the incore inode, and return a pointer
 * to it. If it is in core, but locked, wait for it.
 */
struct vnode *
ufs_ihashget(dev_t dev, ino_t inum)
{
	struct thread *td = curthread;	/* XXX */
	struct inode *ip;
	struct vnode *vp;
	int gen;
	int vgen;

	gen = lwkt_gettoken(&ufs_ihash_token);
loop:
	for (ip = INOHASH(dev, inum)->lh_first; ip; ip = ip->i_hash.le_next) {
		if (inum == ip->i_number && dev == ip->i_dev) {
			vp = ITOV(ip);
			vgen = lwkt_gettoken(&vp->v_interlock);
			if (lwkt_gentoken(&ufs_ihash_token, &gen) != 0) {
				lwkt_reltoken(&vp->v_interlock);
				goto loop;
			}
			if (vget(vp, LK_EXCLUSIVE | LK_INTERLOCK, td)) {
				lwkt_gentoken(&ufs_ihash_token, &gen);
				goto loop;
			}
			lwkt_reltoken(&ufs_ihash_token);
			return (vp);
		}
	}
	lwkt_reltoken(&ufs_ihash_token);
	return (NULL);
}

/*
 * Insert the inode into the hash table, and return it locked.
 */
void
ufs_ihashins(struct inode *ip)
{
	struct thread *td = curthread;		/* XXX */
	struct ihashhead *ipp;

	/* lock the inode, then put it on the appropriate hash list */
	lockmgr(&ip->i_lock, LK_EXCLUSIVE, NULL, td);

	lwkt_gettoken(&ufs_ihash_token);
	ipp = INOHASH(ip->i_dev, ip->i_number);
	LIST_INSERT_HEAD(ipp, ip, i_hash);
	ip->i_flag |= IN_HASHED;
	lwkt_reltoken(&ufs_ihash_token);
}

/*
 * Remove the inode from the hash table.
 */
void
ufs_ihashrem(ip)
	struct inode *ip;
{
	lwkt_gettoken(&ufs_ihash_token);
	if (ip->i_flag & IN_HASHED) {
		ip->i_flag &= ~IN_HASHED;
		LIST_REMOVE(ip, i_hash);
#ifdef DIAGNOSTIC
		ip->i_hash.le_next = NULL;
		ip->i_hash.le_prev = NULL;
#endif
	}
	lwkt_reltoken(&ufs_ihash_token);
}
