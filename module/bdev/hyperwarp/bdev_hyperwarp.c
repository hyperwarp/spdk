/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Hyperwarp project. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Hyperwarp project nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/likely.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include <hyperwarp/metadata.h>

#include "bdev_hyperwarp.h"

struct hyperwarp_bdev {
	struct spdk_bdev	bdev;
	TAILQ_ENTRY(hyperwarp_bdev)	tailq;
};

struct hyperwarp_io_channel {
	struct spdk_poller		*poller;
	TAILQ_HEAD(, spdk_bdev_io)	io;
};

static TAILQ_HEAD(, hyperwarp_bdev) g_hyperwarp_bdev_head = TAILQ_HEAD_INITIALIZER(g_hyperwarp_bdev_head);
static void *g_hyperwarp_read_buf;

static Metadata *g_hyperwarp_metadata;

static int bdev_hyperwarp_initialize(void);
static void bdev_hyperwarp_finish(void);

static struct spdk_bdev_module hyperwarp_if = {
	.name = "hyperwarp",
	.module_init = bdev_hyperwarp_initialize,
	.module_fini = bdev_hyperwarp_finish,
	.async_fini = true,
};

SPDK_BDEV_MODULE_REGISTER(hyperwarp, &hyperwarp_if)

static int
bdev_hyperwarp_destruct(void *ctx)
{
	struct hyperwarp_bdev *bdev = ctx;

	TAILQ_REMOVE(&g_hyperwarp_bdev_head, bdev, tailq);
	free(bdev->bdev.name);
	free(bdev);

	return 0;
}

static bool
bdev_hyperwarp_abort_io(struct hyperwarp_io_channel *ch, struct spdk_bdev_io *bio_to_abort)
{
	struct spdk_bdev_io *bdev_io;

	TAILQ_FOREACH(bdev_io, &ch->io, module_link) {
		if (bdev_io == bio_to_abort) {
			TAILQ_REMOVE(&ch->io, bio_to_abort, module_link);
			spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);
			return true;
		}
	}

	return false;
}

static void
bdev_hyperwarp_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct hyperwarp_io_channel *ch = spdk_io_channel_get_ctx(_ch);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			assert(bdev_io->u.bdev.iovcnt == 1);
			if (spdk_likely(bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen <=
					SPDK_BDEV_LARGE_BUF_MAX_SIZE)) {
				bdev_io->u.bdev.iovs[0].iov_base = g_hyperwarp_read_buf;
				bdev_io->u.bdev.iovs[0].iov_len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
			} else {
				SPDK_ERRLOG("Overflow occurred. Read I/O size %" PRIu64 " was larger than permitted %d\n",
					    bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
					    SPDK_BDEV_LARGE_BUF_MAX_SIZE);
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		if (bdev_hyperwarp_abort_io(ch, bdev_io->u.abort.bio_to_abort)) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static bool
bdev_hyperwarp_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_ABORT:
		return true;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_hyperwarp_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_hyperwarp_bdev_head);
}

static void
bdev_hyperwarp_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	char uuid_str[SPDK_UUID_STRING_LEN];

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_hyperwarp_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table hyperwarp_fn_table = {
	.destruct		= bdev_hyperwarp_destruct,
	.submit_request		= bdev_hyperwarp_submit_request,
	.io_type_supported	= bdev_hyperwarp_io_type_supported,
	.get_io_channel		= bdev_hyperwarp_get_io_channel,
	.write_config_json	= bdev_hyperwarp_write_config_json,
};

int
bdev_hyperwarp_create(struct spdk_bdev **bdev, const struct spdk_hyperwarp_bdev_opts *opts)
{
	struct hyperwarp_bdev *hyperwarp_disk;
	int rc;

	if (!opts) {
		SPDK_ERRLOG("No options provided for Hyperwarp bdev.\n");
		return -EINVAL;
	}
	
	hyperwarp_disk = calloc(1, sizeof(*hyperwarp_disk));
	if (!hyperwarp_disk) {
		SPDK_ERRLOG("could not allocate hyperwarp_bdev\n");
		return -ENOMEM;
	}

	hyperwarp_disk->bdev.name = strdup(opts->name);
	if (!hyperwarp_disk->bdev.name) {
		free(hyperwarp_disk);
		return -ENOMEM;
	}
	hyperwarp_disk->bdev.product_name = "Hyperwarp disk";

	hyperwarp_disk->bdev.write_cache = 0;
	hyperwarp_disk->bdev.blocklen = 4096;
	hyperwarp_disk->bdev.blockcnt = 262144;

	if (opts->uuid) {
		hyperwarp_disk->bdev.uuid = *opts->uuid;
	} else {
		spdk_uuid_generate(&hyperwarp_disk->bdev.uuid);
	}

	hyperwarp_disk->bdev.ctxt = hyperwarp_disk;
	hyperwarp_disk->bdev.fn_table = &hyperwarp_fn_table;
	hyperwarp_disk->bdev.module = &hyperwarp_if;

	rc = spdk_bdev_register(&hyperwarp_disk->bdev);
	if (rc) {
		free(hyperwarp_disk->bdev.name);
		free(hyperwarp_disk);
		return rc;
	}

	*bdev = &(hyperwarp_disk->bdev);

	TAILQ_INSERT_TAIL(&g_hyperwarp_bdev_head, hyperwarp_disk, tailq);

	SPDK_NOTICELOG("Successfully created hyperwarp_bdev %s\n", hyperwarp_disk->bdev.name);

	return rc;
}

void
bdev_hyperwarp_delete(struct spdk_bdev *bdev, spdk_delete_hyperwarp_complete cb_fn, void *cb_arg)
{
	if (!bdev || bdev->module != &hyperwarp_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static int
hyperwarp_io_poll(void *arg)
{
	struct hyperwarp_io_channel		*ch = arg;
	TAILQ_HEAD(, spdk_bdev_io)	io;
	struct spdk_bdev_io		*bdev_io;

	TAILQ_INIT(&io);
	TAILQ_SWAP(&ch->io, &io, spdk_bdev_io, module_link);

	if (TAILQ_EMPTY(&io)) {
		return SPDK_POLLER_IDLE;
	}

	while (!TAILQ_EMPTY(&io)) {
		bdev_io = TAILQ_FIRST(&io);
		TAILQ_REMOVE(&io, bdev_io, module_link);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}

	return SPDK_POLLER_BUSY;
}

static int
hyperwarp_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct hyperwarp_io_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->io);
	ch->poller = SPDK_POLLER_REGISTER(hyperwarp_io_poll, ch, 0);

	return 0;
}

static void
hyperwarp_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct hyperwarp_io_channel *ch = ctx_buf;

	spdk_poller_unregister(&ch->poller);
}

static int 
bdev_hyperwarp_initialize(void)
{
	int rc = 0;
    if ((rc = use_metadata_storage_backend("foundationdb")) != 0) {
        SPDK_ERRLOG("Could not load foundationdb backend!\n");
        return rc;
    }

    if ((rc = metadata_backend_initialize()) != 0) {
        SPDK_ERRLOG("Could not initialize the foundationdb backend\n");
        return rc;
    }

	g_hyperwarp_metadata = metadata_load();

	/*
	 * This will be used if upper layer expects us to allocate the read buffer.
	 *  Instead of using a real rbuf from the bdev pool, just always point to
	 *  this same zeroed buffer.
	 */
	g_hyperwarp_read_buf = spdk_zmalloc(SPDK_BDEV_LARGE_BUF_MAX_SIZE, 0, NULL,
				       SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (g_hyperwarp_read_buf == NULL) {
		return -1;
	}

	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	spdk_io_device_register(&g_hyperwarp_bdev_head, hyperwarp_bdev_create_cb, hyperwarp_bdev_destroy_cb,
				sizeof(struct hyperwarp_io_channel), "hyperwarp_bdev");

	SPDK_NOTICELOG("Successfully initialized hyperwarp_bdev\n");

	return 0;
}

static void
_bdev_hyperwarp_finish_cb(void *arg)
{
	spdk_free(g_hyperwarp_read_buf);
	spdk_bdev_module_finish_done();
}

static void
bdev_hyperwarp_finish(void)
{
	spdk_io_device_unregister(&g_hyperwarp_bdev_head, _bdev_hyperwarp_finish_cb);

	metadata__free_unpacked(g_hyperwarp_metadata, NULL);

	metadata_backend_finalize();

	SPDK_NOTICELOG("Successfully finalized hyperwarp_bdev\n");
}

SPDK_LOG_REGISTER_COMPONENT(bdev_hyperwarp)
