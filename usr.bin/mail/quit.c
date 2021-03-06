/*	$OpenBSD: quit.c,v 1.23 2016/07/19 06:43:27 deraadt Exp $	*/
/*	$NetBSD: quit.c,v 1.6 1996/12/28 07:11:07 tls Exp $	*/

/*
 * Copyright (c) 1980, 1993
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
 */

#include "rcv.h"
#include <fcntl.h>
#include "extern.h"

/*
 * Rcv -- receive mail rationally.
 *
 * Termination processing.
 */

/*
 * The "quit" command.
 */
int
quitcmd(void *v)
{
	/*
	 * If we are sourcing, then return 1 so execute() can handle it.
	 * Otherwise, return -1 to abort command loop.
	 */
	if (sourcing)
		return(1);
	return(-1);
}

/*
 * Save all of the undetermined messages at the top of "mbox"
 * Save all untouched messages back in the system mailbox.
 * Remove the system mailbox, if none saved there.
 */
int
quit(void)
{
	int mcount, p, modify, autohold, anystat, holdbit, nohold;
	FILE *ibuf = NULL, *obuf, *fbuf, *rbuf, *abuf;
	struct message *mp;
	int c, fd;
	struct stat minfo;
	char *mbox, tempname[PATHSIZE];

	/*
	 * If we are read only, we can't do anything,
	 * so just return quickly.
	 */
	if (readonly)
		return(0);

	/*
	 * If editing (not reading system mail box), then do the work
	 * in edstop()
	 */
	if (edit)
		return(edstop());

	/*
	 * See if there any messages to save in mbox.  If no, we
	 * can save copying mbox to /tmp and back.
	 *
	 * Check also to see if any files need to be preserved.
	 * Delete all untouched messages to keep them out of mbox.
	 * If all the messages are to be preserved, just exit with
	 * a message.
	 */
	fbuf = Fopen(mailname, "r+");
	if (fbuf == NULL)
		goto newmail;
	if (flock(fileno(fbuf), LOCK_EX) == -1) {
		warn("Unable to lock mailbox");
		(void)Fclose(fbuf);
		return(-1);
	}
	if (!spool_lock()) {
		(void)Fclose(fbuf);
		return(-1);		/* lockspool printed the error for us */
	}
	rbuf = NULL;
	if (fstat(fileno(fbuf), &minfo) >= 0 && minfo.st_size > mailsize) {
		puts("New mail has arrived.");
		(void)snprintf(tempname, sizeof(tempname),
		    "%s/mail.RqXXXXXXXXXX", tmpdir);
		if ((fd = mkstemp(tempname)) == -1 ||
		    (rbuf = Fdopen(fd, "w")) == NULL)
			goto newmail;
#ifdef APPEND
		fseek(fbuf, (long)mailsize, SEEK_SET);
		while ((c = getc(fbuf)) != EOF)
			(void)putc(c, rbuf);
#else
		p = minfo.st_size - mailsize;
		while (p-- > 0) {
			c = getc(fbuf);
			if (c == EOF)
				goto newmail;
			(void)putc(c, rbuf);
		}
#endif
		(void)Fclose(rbuf);
		if ((rbuf = Fopen(tempname, "r")) == NULL)
			goto newmail;
		(void)rm(tempname);
	}

	/*
	 * Adjust the message flags in each message.
	 */
	anystat = 0;
	autohold = value("hold") != NULL;
	holdbit = autohold ? MPRESERVE : MBOX;
	nohold = MBOX|MSAVED|MDELETED|MPRESERVE;
	if (value("keepsave") != NULL)
		nohold &= ~MSAVED;
	for (mp = &message[0]; mp < &message[msgCount]; mp++) {
		if (mp->m_flag & MNEW) {
			mp->m_flag &= ~MNEW;
			mp->m_flag |= MSTATUS;
		}
		if (mp->m_flag & MSTATUS)
			anystat++;
		if ((mp->m_flag & MTOUCH) == 0)
			mp->m_flag |= MPRESERVE;
		if ((mp->m_flag & nohold) == 0)
			mp->m_flag |= holdbit;
	}
	modify = 0;
	for (c = 0, p = 0, mp = &message[0]; mp < &message[msgCount]; mp++) {
		if (mp->m_flag & MBOX)
			c++;
		if (mp->m_flag & MPRESERVE)
			p++;
		if (mp->m_flag & MODIFY)
			modify++;
	}
	if (p == msgCount && !modify && !anystat) {
		printf("Held %d message%s in %s\n",
			p, p == 1 ? "" : "s", mailname);
		(void)Fclose(fbuf);
		spool_unlock();
		return(0);
	}
	if (c == 0) {
		if (p != 0) {
			writeback(rbuf);
			(void)Fclose(fbuf);
			spool_unlock();
			return(0);
		}
		goto cream;
	}

	/*
	 * Create another temporary file and copy user's mbox file
	 * darin.  If there is no mbox, copy nothing.
	 * If he has specified "append" don't copy his mailbox,
	 * just copy saveable entries at the end.
	 */
	mbox = expand("&");
	mcount = c;
	if (value("append") == NULL) {
		int fdx;

		(void)snprintf(tempname, sizeof(tempname),
		    "%s/mail.RmXXXXXXXXXX", tmpdir);
		if ((fd = mkstemp(tempname)) == -1 ||
		    (obuf = Fdopen(fd, "w")) == NULL) {
			warn("%s", tempname);
			(void)Fclose(fbuf);
			spool_unlock();
			return(-1);
		}
		if ((ibuf = Fopen(tempname, "r")) == NULL) {
			warn("%s", tempname);
			(void)rm(tempname);
			(void)Fclose(obuf);
			(void)Fclose(fbuf);
			spool_unlock();
			return(-1);
		}
		(void)rm(tempname);
		if ((abuf = Fopen(mbox, "r")) != NULL) {
			while ((c = getc(abuf)) != EOF)
				(void)putc(c, obuf);
			(void)Fclose(abuf);
		}
		if (ferror(obuf)) {
			warn("%s", tempname);
			(void)Fclose(ibuf);
			(void)Fclose(obuf);
			(void)Fclose(fbuf);
			spool_unlock();
			return(-1);
		}
		(void)Fclose(obuf);
		if ((fdx = open(mbox, O_CREAT | O_TRUNC | O_WRONLY, 0600)) != -1)
			close(fdx);
		if ((obuf = Fopen(mbox, "r+")) == NULL) {
			warn("%s", mbox);
			(void)Fclose(ibuf);
			(void)Fclose(fbuf);
			spool_unlock();
			return(-1);
		}
	} else {
		if ((obuf = Fopen(mbox, "a")) == NULL) {
			warn("%s", mbox);
			(void)Fclose(fbuf);
			spool_unlock();
			return(-1);
		}
		fchmod(fileno(obuf), 0600);
	}
	for (mp = &message[0]; mp < &message[msgCount]; mp++)
		if (mp->m_flag & MBOX)
			if (sendmessage(mp, obuf, saveignore, NULL) < 0) {
				warn("%s", mbox);
				(void)Fclose(ibuf);
				(void)Fclose(obuf);
				(void)Fclose(fbuf);
				spool_unlock();
				return(-1);
			}

	/*
	 * Copy the user's old mbox contents back
	 * to the end of the stuff we just saved.
	 * If we are appending, this is unnecessary.
	 */
	if (value("append") == NULL) {
		rewind(ibuf);
		c = getc(ibuf);
		while (c != EOF) {
			(void)putc(c, obuf);
			if (ferror(obuf))
				break;
			c = getc(ibuf);
		}
		(void)Fclose(ibuf);
		fflush(obuf);
	}
	trunc(obuf);
	if (ferror(obuf)) {
		warn("%s", mbox);
		(void)Fclose(obuf);
		(void)Fclose(fbuf);
		spool_unlock();
		return(-1);
	}
	(void)Fclose(obuf);
	if (mcount == 1)
		puts("Saved 1 message in mbox");
	else
		printf("Saved %d messages in mbox\n", mcount);

	/*
	 * Now we are ready to copy back preserved files to
	 * the system mailbox, if any were requested.
	 */
	if (p != 0) {
		writeback(rbuf);
		(void)Fclose(fbuf);
		spool_unlock();
		return(0);
	}

	/*
	 * Finally, remove his /var/mail file.
	 * If new mail has arrived, copy it back.
	 */
cream:
	if (rbuf != NULL) {
		abuf = Fopen(mailname, "r+");
		if (abuf == NULL)
			goto newmail;
		while ((c = getc(rbuf)) != EOF)
			(void)putc(c, abuf);
		(void)Fclose(rbuf);
		trunc(abuf);
		(void)Fclose(abuf);
		alter(mailname);
		(void)Fclose(fbuf);
		spool_unlock();
		return(0);
	}
	demail();
	(void)Fclose(fbuf);
	spool_unlock();
	return(0);

newmail:
	puts("Thou hast new mail.");
	if (fbuf != NULL) {
		(void)Fclose(fbuf);
		spool_unlock();
	}
	return(0);
}

/*
 * Preserve all the appropriate messages back in the system
 * mailbox, and print a nice message indicated how many were
 * saved.  On any error, just return -1.  Else return 0.
 * Incorporate the any new mail that we found.
 */
int
writeback(FILE *res)
{
	struct message *mp;
	int p, c;
	FILE *obuf;

	p = 0;
	if ((obuf = Fopen(mailname, "r+")) == NULL) {
		warn("%s", mailname);
		return(-1);
	}
#ifndef APPEND
	if (res != NULL)
		while ((c = getc(res)) != EOF)
			(void)putc(c, obuf);
#endif
	for (mp = &message[0]; mp < &message[msgCount]; mp++)
		if ((mp->m_flag&MPRESERVE)||(mp->m_flag&MTOUCH)==0) {
			p++;
			if (sendmessage(mp, obuf, NULL, NULL) < 0) {
				warn("%s", mailname);
				(void)Fclose(obuf);
				return(-1);
			}
		}
#ifdef APPEND
	if (res != NULL)
		while ((c = getc(res)) != EOF)
			(void)putc(c, obuf);
#endif
	fflush(obuf);
	trunc(obuf);
	if (ferror(obuf)) {
		warn("%s", mailname);
		(void)Fclose(obuf);
		return(-1);
	}
	if (res != NULL)
		(void)Fclose(res);
	(void)Fclose(obuf);
	alter(mailname);
	if (p == 1)
		printf("Held 1 message in %s\n", mailname);
	else
		printf("Held %d messages in %s\n", p, mailname);
	return(0);
}

/*
 * Terminate an editing session by attempting to write out the user's
 * file from the temporary.  Save any new stuff appended to the file.
 */
int
edstop(void)
{
	int gotcha, c;
	struct message *mp;
	FILE *obuf, *ibuf;
	struct stat statb;
	char tempname[PATHSIZE];

	if (readonly)
		return(0);
	holdsigs();
	for (mp = &message[0], gotcha = 0; mp < &message[msgCount]; mp++) {
		if (mp->m_flag & MNEW) {
			mp->m_flag &= ~MNEW;
			mp->m_flag |= MSTATUS;
		}
		if (mp->m_flag & (MODIFY|MDELETED|MSTATUS))
			gotcha++;
	}
	if (!gotcha)
		goto done;
	ibuf = NULL;
	if (stat(mailname, &statb) >= 0 && statb.st_size > mailsize) {
		int fd;

		(void)snprintf(tempname, sizeof(tempname), "%s/mbox.XXXXXXXXXX",
		    tmpdir);
		if ((fd = mkstemp(tempname)) == -1 ||
		    (obuf = Fdopen(fd, "w")) == NULL) {
			warn("%s", tempname);
			if (fd != -1)
				close(fd);
			relsesigs();
			return(-1);
		}
		if ((ibuf = Fopen(mailname, "r")) == NULL) {
			warn("%s", mailname);
			(void)Fclose(obuf);
			(void)rm(tempname);
			relsesigs();
			return(-1);
		}
		fseek(ibuf, (long)mailsize, SEEK_SET);
		while ((c = getc(ibuf)) != EOF)
			(void)putc(c, obuf);
		(void)Fclose(ibuf);
		(void)Fclose(obuf);
		if ((ibuf = Fopen(tempname, "r")) == NULL) {
			warn("%s", tempname);
			(void)rm(tempname);
			relsesigs();
			return(-1);
		}
		(void)rm(tempname);
	}
	printf("\"%s\" ", mailname);
	fflush(stdout);
	if ((obuf = Fopen(mailname, "r+")) == NULL) {
		warn("%s", mailname);
		relsesigs();
		return(-1);
	}
	trunc(obuf);
	c = 0;
	for (mp = &message[0]; mp < &message[msgCount]; mp++) {
		if ((mp->m_flag & MDELETED) != 0)
			continue;
		c++;
		if (sendmessage(mp, obuf, NULL, NULL) < 0) {
			warn("%s", mailname);
			relsesigs();
			return(-1);
		}
	}
	gotcha = (c == 0 && ibuf == NULL);
	if (ibuf != NULL) {
		while ((c = getc(ibuf)) != EOF)
			(void)putc(c, obuf);
		(void)Fclose(ibuf);
	}
	fflush(obuf);
	if (ferror(obuf)) {
		warn("%s", mailname);
		relsesigs();
		return(-1);
	}
	(void)Fclose(obuf);
	if (gotcha) {
		(void)rm(mailname);
		puts("removed");
	} else
		puts("complete");
	fflush(stdout);

done:
	relsesigs();
	return(0);
}
