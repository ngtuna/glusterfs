/*
  Copyright (c) 2006-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "xlator.h"
#include "glusterfs.h"
#include "posix.h"
#include <sys/uio.h>

#ifdef HAVE_LIBAIO
#include <libaio.h>


struct posix_aio_cb {
        struct iocb     iocb;
        call_frame_t   *frame;
        struct iobuf   *iobuf;
        struct iobref  *iobref;
        struct iatt     prebuf;
        int             fd;
        int             op;
        off_t           offset;
};


int
posix_aio_readv_complete (struct posix_aio_cb *paiocb, int res, int res2)
{
        call_frame_t   *frame = NULL;
        xlator_t       *this = NULL;
        struct iobuf   *iobuf = NULL;
        struct iatt     postbuf = {0,};
        int             _fd = -1;
        int             op_ret = -1;
        int             op_errno = 0;
        struct iovec    iov;
        struct iobref  *iobref = NULL;
        int             ret = 0;
        off_t           offset = 0;
        struct posix_private * priv = NULL;


        frame = paiocb->frame;
        this = frame->this;
        priv = this->private;
        iobuf = paiocb->iobuf;
        _fd = paiocb->fd;
        offset = paiocb->offset;

        if (res < 0) {
                op_ret = -1;
                op_errno = -res;
		gf_log (this->name, GF_LOG_ERROR,
			"readv(async) failed fd=%d,size=%lu,offset=%llu (%d/%s)",
			_fd, paiocb->iocb.u.c.nbytes,
			(unsigned long long) paiocb->offset,
			res, strerror (op_errno));
                goto out;
        }

        ret = posix_fdstat (this, _fd, &postbuf);
	if (ret != 0) {
		op_ret = -1;
                op_errno = errno;
                gf_log (this->name, GF_LOG_ERROR,
                        "fstat failed on fd=%d: %s", _fd,
                        strerror (op_errno));
                goto out;
	}

        op_ret = res;
        op_errno = 0;

        iobref = iobref_new ();
        if (!iobref) {
                op_ret = -1;
                op_errno = ENOMEM;
                goto out;
        }

        iobref_add (iobref, iobuf);

        iov.iov_base = iobuf_ptr (iobuf);
        iov.iov_len = op_ret;


        /* Hack to notify higher layers of EOF. */
        if (postbuf.ia_size == 0)
                op_errno = ENOENT;
        else if ((offset + iov.iov_len) == postbuf.ia_size)
                op_errno = ENOENT;
        else if (offset > postbuf.ia_size)
                op_errno = ENOENT;

        LOCK (&priv->lock);
        {
                priv->read_value += op_ret;
        }
        UNLOCK (&priv->lock);

out:
        STACK_UNWIND_STRICT (readv, frame, op_ret, op_errno, &iov, 1,
                             &postbuf, iobref, NULL);
        if (iobuf)
                iobuf_unref (iobuf);
        if (iobref)
                iobref_unref (iobref);

        GF_FREE (paiocb);

        return 0;
}


int
posix_aio_readv (call_frame_t *frame, xlator_t *this, fd_t *fd,
                 size_t size, off_t offset, uint32_t flags, dict_t *xdata)
{
        int32_t                op_errno   = EINVAL;
        int                    _fd        = -1;
        struct iobuf          *iobuf      = NULL;
        struct posix_fd *      pfd        = NULL;
        int                    ret        = -1;
        struct posix_aio_cb   *paiocb     = NULL;
        struct posix_private  *priv       = NULL;
        struct iocb           *iocb       = NULL;


        VALIDATE_OR_GOTO (frame, err);
        VALIDATE_OR_GOTO (this, err);
        VALIDATE_OR_GOTO (fd, err);

        priv = this->private;

        ret = posix_fd_ctx_get_off (fd, this, &pfd, offset);
        if (ret < 0) {
                op_errno = -ret;
                gf_log (this->name, GF_LOG_WARNING,
                        "pfd is NULL from fd=%p", fd);
                goto err;
        }
        _fd = pfd->fd;

        if (!size) {
                op_errno = EINVAL;
                gf_log (this->name, GF_LOG_WARNING, "size=%"GF_PRI_SIZET, size);
                goto err;
        }

        iobuf = iobuf_get2 (this->ctx->iobuf_pool, size);
        if (!iobuf) {
                op_errno = ENOMEM;
                goto err;
        }

        paiocb = GF_CALLOC (1, sizeof (*paiocb), gf_posix_mt_paiocb);
        if (!paiocb) {
                op_errno = ENOMEM;
                goto err;
        }


        paiocb->frame = frame;
        paiocb->iobuf = iobuf;
        paiocb->offset = offset;
        paiocb->fd = _fd;
        paiocb->op = GF_FOP_READ;

        paiocb->iocb.data = paiocb;
        paiocb->iocb.aio_fildes = _fd;
        paiocb->iocb.aio_lio_opcode = IO_CMD_PREAD;
        paiocb->iocb.aio_reqprio = 0;
        paiocb->iocb.u.c.buf = iobuf_ptr (iobuf);
        paiocb->iocb.u.c.nbytes = size;
        paiocb->iocb.u.c.offset = offset;

        iocb = &paiocb->iocb;

        ret = io_submit (priv->ctxp, 1, &iocb);
        if (ret != 1) {
                gf_log (this->name, GF_LOG_ERROR,
                        "io_submit() returned %d", ret);
                op_errno = -ret;
                goto err;
        }

        return 0;
err:
        STACK_UNWIND_STRICT (readv, frame, -1, op_errno, 0, 0, 0, 0, 0);
        if (iobuf)
                iobuf_unref (iobuf);

        if (paiocb)
                GF_FREE (paiocb);

        return 0;
}


int
posix_aio_writev_complete (struct posix_aio_cb *paiocb, int res, int res2)
{
        call_frame_t   *frame = NULL;
        xlator_t       *this = NULL;
        struct iatt     prebuf = {0,};
        struct iatt     postbuf = {0,};
        int             _fd = -1;
        int             op_ret = -1;
        int             op_errno = 0;
        int             ret = 0;
        struct posix_private * priv = NULL;


        frame = paiocb->frame;
        this = frame->this;
        priv = this->private;
        prebuf = paiocb->prebuf;
        _fd = paiocb->fd;

        if (res < 0) {
                op_ret = -1;
                op_errno = -res;
		gf_log (this->name, GF_LOG_ERROR,
			"writev(async) failed fd=%d,offset=%llu (%d/%s)",
			_fd, (unsigned long long) paiocb->offset, res,
			strerror (op_errno));

                goto out;
        }

        ret = posix_fdstat (this, _fd, &postbuf);
	if (ret != 0) {
		op_ret = -1;
                op_errno = errno;
                gf_log (this->name, GF_LOG_ERROR,
                        "fstat failed on fd=%d: %s", _fd,
                        strerror (op_errno));
                goto out;
	}


        op_ret = res;
        op_errno = 0;

        LOCK (&priv->lock);
        {
                priv->write_value += op_ret;
        }
        UNLOCK (&priv->lock);

out:
        STACK_UNWIND_STRICT (writev, frame, op_ret, op_errno, &prebuf, &postbuf,
			     NULL);

        if (paiocb) {
                if (paiocb->iobref)
                        iobref_unref (paiocb->iobref);
                GF_FREE (paiocb);
        }

        return 0;
}


int
posix_aio_writev (call_frame_t *frame, xlator_t *this, fd_t *fd,
                  struct iovec *iov, int count, off_t offset, uint32_t flags,
                  struct iobref *iobref, dict_t *xdata)
{
        int32_t                op_errno   = EINVAL;
        int                    _fd        = -1;
        struct posix_fd *      pfd        = NULL;
        int                    ret        = -1;
        struct posix_aio_cb   *paiocb     = NULL;
        struct posix_private  *priv       = NULL;
        struct iocb           *iocb       = NULL;


        VALIDATE_OR_GOTO (frame, err);
        VALIDATE_OR_GOTO (this, err);
        VALIDATE_OR_GOTO (fd, err);

        priv = this->private;

        ret = posix_fd_ctx_get_off (fd, this, &pfd, offset);
        if (ret < 0) {
                op_errno = -ret;
                gf_log (this->name, GF_LOG_WARNING,
                        "pfd is NULL from fd=%p", fd);
                goto err;
        }
        _fd = pfd->fd;

        paiocb = GF_CALLOC (1, sizeof (*paiocb), gf_posix_mt_paiocb);
        if (!paiocb) {
                op_errno = ENOMEM;
                goto err;
        }


        paiocb->frame = frame;
        paiocb->offset = offset;
        paiocb->fd = _fd;
        paiocb->op = GF_FOP_WRITE;

        paiocb->iocb.data = paiocb;
        paiocb->iocb.aio_fildes = _fd;
        paiocb->iobref = iobref_ref (iobref);
        paiocb->iocb.aio_lio_opcode = IO_CMD_PWRITEV;
        paiocb->iocb.aio_reqprio = 0;
        paiocb->iocb.u.v.vec = iov;
        paiocb->iocb.u.v.nr = count;
        paiocb->iocb.u.v.offset = offset;

        iocb = &paiocb->iocb;

        ret = posix_fdstat (this, _fd, &paiocb->prebuf);
	if (ret != 0) {
                op_errno = errno;
                gf_log (this->name, GF_LOG_ERROR,
                        "fstat failed on fd=%p: %s", fd,
                        strerror (op_errno));
                goto err;
	}


        ret = io_submit (priv->ctxp, 1, &iocb);
        if (ret != 1) {
                gf_log (this->name, GF_LOG_ERROR,
                        "io_submit() returned %d", ret);
                op_errno = -ret;
                goto err;
        }

        return 0;
err:
        STACK_UNWIND_STRICT (writev, frame, -1, op_errno, 0, 0, 0);

        if (paiocb) {
                if (paiocb->iobref)
                        iobref_unref (paiocb->iobref);
                GF_FREE (paiocb);
        }

        return 0;
}


void *
posix_aio_thread (void *data)
{
        xlator_t              *this = NULL;
        struct posix_private  *priv = NULL;
        int                    ret = 0;
	int                    i = 0;
        struct io_event        events[POSIX_AIO_MAX_NR_GETEVENTS];
        struct io_event       *event = NULL;
        struct posix_aio_cb   *paiocb = NULL;

        this = data;
	THIS = this;
        priv = this->private;

        for (;;) {
                memset (&events[0], 0, sizeof (events));
                ret = io_getevents (priv->ctxp, 1, POSIX_AIO_MAX_NR_GETEVENTS,
				    &events[0], NULL);
                if (ret <= 0) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "io_getevents() returned %d", ret);
                        if (ret == -EINTR)
                                continue;
                        break;
                }

		for (i = 0; i < ret; i++) {
			event = &events[i];

			paiocb = event->data;

			switch (paiocb->op) {
			case GF_FOP_READ:
				posix_aio_readv_complete (paiocb, event->res,
							  event->res2);
				break;
			case GF_FOP_WRITE:
				posix_aio_writev_complete (paiocb, event->res,
							   event->res2);
				break;
			default:
				gf_log (this->name, GF_LOG_ERROR,
					"unknown op %d found in piocb",
					paiocb->op);
				break;
			}
                }
        }

        return NULL;
}


int
posix_aio_init (xlator_t *this)
{
        struct posix_private *priv = NULL;
        int                   ret = 0;

        priv = this->private;

        ret = io_setup (POSIX_AIO_MAX_NR_EVENTS, &priv->ctxp);
        if ((ret == -1 && errno == ENOSYS) || ret == -ENOSYS) {
                gf_log (this->name, GF_LOG_WARNING,
                        "Linux AIO not availble at run-time."
			" Continuing with synchronous IO");
                ret = 0;
                goto out;
        }

        if (ret < 0) {
		gf_log (this->name, GF_LOG_WARNING,
			"io_setup() failed. ret=%d, errno=%d",
			ret, errno);
                goto out;
	}

        ret = pthread_create (&priv->aiothread, NULL,
                              posix_aio_thread, this);
        if (ret != 0) {
                io_destroy (priv->ctxp);
                goto out;
        }

        this->fops->readv  = posix_aio_readv;
        this->fops->writev = posix_aio_writev;
out:
        return ret;
}


int
posix_aio_on (xlator_t *this)
{
	struct posix_private *priv = NULL;
	int                   ret = 0;

	priv = this->private;

	if (!priv->aio_init_done) {
		ret = posix_aio_init (this);
		if (ret == 0)
			priv->aio_capable = _gf_true;
		else
			priv->aio_capable = _gf_false;
		priv->aio_init_done = _gf_true;
	}

	if (priv->aio_capable) {
		this->fops->readv  = posix_aio_readv;
		this->fops->writev = posix_aio_writev;
	}

        return ret;
}

int
posix_aio_off (xlator_t *this)
{
	this->fops->readv  = posix_readv;
	this->fops->writev = posix_writev;

        return 0;
}


#else


int
posix_aio_on (xlator_t *this)
{
        gf_log (this->name, GF_LOG_INFO,
                "Linux AIO not availble at build-time."
		" Continuing with synchronous IO");
        return 0;
}

int
posix_aio_off (xlator_t *this)
{
        gf_log (this->name, GF_LOG_INFO,
                "Linux AIO not availble at build-time."
		" Continuing with synchronous IO");
        return 0;
}

#endif
