/*
 * ispvideo.h
 *
 * TI OMAP3 ISP - Generic video node
 *
 * Copyright (C) 2009-2010 Nokia Corporation
 *
 * Contacts: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *	     Sakari Ailus <sakari.ailus@maxwell.research.nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#ifndef OMAP3_ISP_VIDEO_H
#define OMAP3_ISP_VIDEO_H

#include <linux/version.h>
#include <media/media-entity.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>

#include "ispqueue.h"

#define ISP_VIDEO_DRIVER_NAME		"ispvideo"
#define ISP_VIDEO_DRIVER_VERSION	KERNEL_VERSION(0, 0, 1)

struct isp_device;
struct isp_video;
struct v4l2_mbus_framefmt;
struct v4l2_pix_format;

enum isp_pipeline_stream_state {
	ISP_PIPELINE_STREAM_STOPPED = 0,
	ISP_PIPELINE_STREAM_CONTINUOUS = 1,
	ISP_PIPELINE_STREAM_SINGLESHOT = 2,
};

enum isp_pipeline_state {
	/* The stream has been started on the input video node. */
	ISP_PIPELINE_STREAM_INPUT = 1,
	/* The stream has been started on the output video node. */
	ISP_PIPELINE_STREAM_OUTPUT = 2,
	/* At least one buffer is queued on the input video node. */
	ISP_PIPELINE_QUEUE_INPUT = 4,
	/* At least one buffer is queued on the output video node. */
	ISP_PIPELINE_QUEUE_OUTPUT = 8,
	/* The input entity is idle, ready to be started. */
	ISP_PIPELINE_IDLE_INPUT = 16,
	/* The output entity is idle, ready to be started. */
	ISP_PIPELINE_IDLE_OUTPUT = 32,
	/* The pipeline is currently streaming. */
	ISP_PIPELINE_STREAM = 64,
};

struct isp_pipeline {
	struct media_pipeline pipe;
	spinlock_t lock;
	unsigned int state;
	enum isp_pipeline_stream_state stream_state;
	struct isp_video *input;
	struct isp_video *output;
	unsigned long l3_ick;
	unsigned int max_rate;
	struct v4l2_fract max_timeperframe;
};

#define to_isp_pipeline(__e) \
	container_of((__e)->pipe, struct isp_pipeline, pipe)

static inline int isp_pipeline_ready(struct isp_pipeline *pipe)
{
	return pipe->state == (ISP_PIPELINE_STREAM_INPUT |
			       ISP_PIPELINE_STREAM_OUTPUT |
			       ISP_PIPELINE_QUEUE_INPUT |
			       ISP_PIPELINE_QUEUE_OUTPUT |
			       ISP_PIPELINE_IDLE_INPUT |
			       ISP_PIPELINE_IDLE_OUTPUT);
}

/*
 * struct isp_buffer - ISP buffer
 * @buffer: ISP video buffer
 * @isp_addr: MMU mapped address (a.k.a. device address) of the buffer.
 */
struct isp_buffer {
	struct isp_video_buffer buffer;
	dma_addr_t isp_addr;
};

#define to_isp_buffer(buf)	container_of(buf, struct isp_buffer, buffer)

enum isp_video_dmaqueue_flags {
	/* Set if DMA queue becomes empty when ISP_PIPELINE_STREAM_CONTINUOUS */
	ISP_VIDEO_DMAQUEUE_UNDERRUN = (1 << 0),
	/* Set when queuing buffer to an empty DMA queue */
	ISP_VIDEO_DMAQUEUE_QUEUED = (1 << 1),
};

#define isp_video_dmaqueue_flags_clr(video)	\
			({ (video)->dmaqueue_flags = 0; })

/*
 * struct isp_video_operations - ISP video operations
 * @queue:	Resume streaming when a buffer is queued. Called on VIDIOC_QBUF
 *		if there was no buffer previously queued.
 */
struct isp_video_operations {
	int(*queue)(struct isp_video *video, struct isp_buffer *buffer);
};

struct isp_video {
	struct video_device video;
	enum v4l2_buf_type type;
	struct media_pad pad;

	struct mutex mutex;
	atomic_t active;

	struct isp_device *isp;

	unsigned int capture_mem;
	unsigned int alignment;

	/* Entity video node streaming */
	unsigned int streaming:1;

	/* Pipeline state */
	struct isp_pipeline pipe;
	struct mutex stream_lock;

	/* Video buffers queue */
	struct isp_video_queue *queue;
	struct list_head dmaqueue;
	atomic_t sequence;
	enum isp_video_dmaqueue_flags dmaqueue_flags;

	const struct isp_video_operations *ops;
};

#define to_isp_video(vdev)	container_of(vdev, struct isp_video, video)

struct isp_video_fh {
	struct v4l2_fh vfh;
	struct isp_video *video;
	struct isp_video_queue queue;
	struct v4l2_format format;
	struct v4l2_fract timeperframe;
};

#define to_isp_video_fh(fh)	container_of(fh, struct isp_video_fh, vfh)
#define isp_video_queue_to_isp_video_fh(q) \
				container_of(q, struct isp_video_fh, queue)

extern int isp_video_init(struct isp_video *video, const char *name);
extern int isp_video_register(struct isp_video *video,
			      struct v4l2_device *vdev);
extern void isp_video_unregister(struct isp_video *video);
extern struct isp_buffer *isp_video_buffer_next(struct isp_video *video,
						unsigned int error);
extern void isp_video_resume(struct isp_video *video, int continuous);
extern struct media_pad *isp_video_remote_pad(struct isp_video *video);
extern void isp_video_mbus_to_pix(const struct isp_video *video,
				  const struct v4l2_mbus_framefmt *mbus,
				  struct v4l2_pix_format *pix);
extern void isp_video_pix_to_mbus(const struct v4l2_pix_format *pix,
				  struct v4l2_mbus_framefmt *mbus);

extern enum v4l2_mbus_pixelcode
isp_video_uncompressed_code(enum v4l2_mbus_pixelcode code);

#endif /* OMAP3_ISP_VIDEO_H */
