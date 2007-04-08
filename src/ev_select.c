/*
 * FD polling functions for generic select()
 *
 * Copyright 2000-2007 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/time.h>

#include <types/fd.h>
#include <types/global.h>

#include <proto/fd.h>
#include <proto/polling.h>
#include <proto/task.h>


static fd_set *ReadEvent, *WriteEvent;
static fd_set *StaticReadEvent, *StaticWriteEvent;


/*
 * Benchmarks performed on a Pentium-M notebook show that using functions
 * instead of the usual macros improve the FD_* performance by about 80%,
 * and that marking them regparm(2) adds another 20%.
 */
REGPRM2 static int __fd_isset(const int fd, const int dir)
{
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	return FD_ISSET(fd, ev);
}

REGPRM2 static void __fd_set(const int fd, const int dir)
{
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	FD_SET(fd, ev);
}

REGPRM2 static void __fd_clr(const int fd, const int dir)
{
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	FD_CLR(fd, ev);
}

REGPRM2 static int __fd_cond_s(const int fd, const int dir)
{
	int ret;
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	ret = !FD_ISSET(fd, ev);
	if (ret)
		FD_SET(fd, ev);
	return ret;
}

REGPRM2 static int __fd_cond_c(const int fd, const int dir)
{
	int ret;
	fd_set *ev;
	if (dir == DIR_RD)
		ev = StaticReadEvent;
	else
		ev = StaticWriteEvent;

	ret = FD_ISSET(fd, ev);
	if (ret)
		FD_CLR(fd, ev);
	return ret;
}

REGPRM1 static void __fd_rem(const int fd)
{
	FD_CLR(fd, StaticReadEvent);
	FD_CLR(fd, StaticWriteEvent);
}


/*
 * Initialization of the select() poller.
 * Returns 0 in case of failure, non-zero in case of success. If it fails, it
 * disables the poller by setting its pref to 0.
 */
REGPRM1 static int select_init(struct poller *p)
{
	__label__ fail_swevt, fail_srevt, fail_wevt, fail_revt;
	int fd_set_bytes;

	p->private = NULL;
	fd_set_bytes = sizeof(fd_set) * (global.maxsock + FD_SETSIZE - 1) / FD_SETSIZE;

	if ((ReadEvent = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_revt;
		
	if ((WriteEvent = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_wevt;

	if ((StaticReadEvent = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_srevt;

	if ((StaticWriteEvent = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_swevt;

	return 1;

 fail_swevt:
	free(StaticReadEvent);
 fail_srevt:
	free(WriteEvent);
 fail_wevt:
	free(ReadEvent);
 fail_revt:
	p->pref = 0;
	return 0;
}

/*
 * Termination of the select() poller.
 * Memory is released and the poller is marked as unselectable.
 */
REGPRM1 static void select_term(struct poller *p)
{
	if (StaticWriteEvent)
		free(StaticWriteEvent);
	if (StaticReadEvent)
		free(StaticReadEvent);
	if (WriteEvent)
		free(WriteEvent);
	if (ReadEvent)
		free(ReadEvent);
	p->private = NULL;
	p->pref = 0;
}

/*
 * Select() poller
 */
REGPRM2 static void select_poll(struct poller *p, int wait_time)
{
	int status;
	int fd, i;
	struct timeval delta;
	int readnotnull, writenotnull;
	int fds;
	char count;
		
	/* allow select to return immediately when needed */
	delta.tv_sec = delta.tv_usec = 0;
	if (wait_time > 0) {  /* FIXME */
		/* Convert to timeval */
		/* to avoid eventual select loops due to timer precision */
		wait_time += SCHEDULER_RESOLUTION;
		delta.tv_sec  = wait_time / 1000; 
		delta.tv_usec = (wait_time % 1000) * 1000;
	}

	/* let's restore fdset state */

	readnotnull = 0; writenotnull = 0;
	for (i = 0; i < (maxfd + FD_SETSIZE - 1)/(8*sizeof(int)); i++) {
		readnotnull |= (*(((int*)ReadEvent)+i) = *(((int*)StaticReadEvent)+i)) != 0;
		writenotnull |= (*(((int*)WriteEvent)+i) = *(((int*)StaticWriteEvent)+i)) != 0;
	}

	//	/* just a verification code, needs to be removed for performance */
	//	for (i=0; i<maxfd; i++) {
	//	    if (FD_ISSET(i, ReadEvent) != FD_ISSET(i, StaticReadEvent))
	//		abort();
	//	    if (FD_ISSET(i, WriteEvent) != FD_ISSET(i, StaticWriteEvent))
	//		abort();
	//	    
	//	}

	status = select(maxfd,
			readnotnull ? ReadEvent : NULL,
			writenotnull ? WriteEvent : NULL,
			NULL,
			(wait_time >= 0) ? &delta : NULL);
      
	tv_now(&now);

	if (status <= 0)
		return;

	for (fds = 0; (fds << INTBITS) < maxfd; fds++) {
		if ((((int *)(ReadEvent))[fds] | ((int *)(WriteEvent))[fds]) == 0)
			continue;

		for (count = 1<<INTBITS, fd = fds << INTBITS; count && fd < maxfd; count--, fd++) {
			/* if we specify read first, the accepts and zero reads will be
			 * seen first. Moreover, system buffers will be flushed faster.
			 */
			if (FD_ISSET(fd, ReadEvent)) {
				if (fdtab[fd].state == FD_STCLOSE)
					continue;
				fdtab[fd].cb[DIR_RD].f(fd);
			}

			if (FD_ISSET(fd, WriteEvent)) {
				if (fdtab[fd].state == FD_STCLOSE)
					continue;
				fdtab[fd].cb[DIR_WR].f(fd);
			}
		}
	}
}

/*
 * The only exported function. Returns 1.
 */
int select_register(struct poller *p)
{
	p->name = "select";
	p->pref = 150;
	p->private = NULL;

	p->init = select_init;
	p->term = select_term;
	p->poll = select_poll;
	p->isset = __fd_isset;
	p->set = __fd_set;
	p->clr = __fd_clr;
	p->clo = p->rem = __fd_rem;
	p->cond_s = __fd_cond_s;
	p->cond_c = __fd_cond_c;
	return 1;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */