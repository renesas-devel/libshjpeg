/*
 * libshjpeg: A library for controlling SH-Mobile JPEG hardware codec
 *
 * Copyright (C) 2010 IGEL Co.,Ltd.
 * Copyright (C) 2008,2009 Renesas Technology Corp.
 * Copyright (C) 2008 Denis Oliver Kropp
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA	 02110-1301 USA
 */

#include "libmessage.h"
#include "jpeg_io.h"
#include "override.h"
#include <string.h>
#include <stdlib.h>
/*Keep one global context for now.  Will be replaced with a
 *hash lookup based on cinfo pointer in the future*/
extern cinfo_context cictxt;

boolean cache_input_buffer (j_decompress_ptr cinfo) {
	buffer_cache_context_t *cache_con;
	cache_con = cictxt.cache_con;
	if (cache_con->current_start) {
		void *temp_buf = malloc(cache_con->total_buffer_size +
			 cache_con->last_buffer_size);
		if (cache_con->buffer_cache) {
			memcpy(temp_buf, cache_con->buffer_cache,
				cache_con->total_buffer_size);
			free(cache_con->buffer_cache);
		}
		memcpy(temp_buf + cache_con->total_buffer_size,
			cache_con->current_start, cache_con->last_buffer_size);
		cache_con->buffer_cache = cache_con->cache_read =
			temp_buf;
		cache_con->total_buffer_size += cache_con->last_buffer_size;
	}
	cache_con->fill_buffer_function(cinfo);
	cache_con->last_buffer_size = cinfo->src->bytes_in_buffer;
	cache_con->current_start = cache_con->current_read =
		(void *) cinfo->src->next_input_byte;
	return TRUE;
}

/*Streams operations*/
/*Using libjpeg src/dest callback functions to access data via JPU
  library */

int jpeg_src_init(void *private_data) {
  cinfo_context *cctx = (cinfo_context *) private_data;
  j_decompress_ptr cinfo = (j_decompress_ptr) cctx->cinfo;

  cinfo->src->init_source(cinfo);
  return 0;
}

void jpeg_src_finalize(void *private_data) {
  cinfo_context *cctx = (cinfo_context *) private_data;
  j_decompress_ptr cinfo = (j_decompress_ptr) cctx->cinfo;

  cinfo->src->term_source(cinfo);
}
int jpeg_src_read_header (void *private_data, size_t *n_bytes, void *dataptr) {
  cinfo_context *cctx = (cinfo_context *) private_data;
  j_decompress_ptr cinfo = (j_decompress_ptr) cctx->cinfo;
  shjpeg_context_t *context = cctx->context;
  buffer_cache_context_t *cache_con;
  cache_con = context->cache_con;

  size_t copybytes = 0, copied, local_n_bytes = *n_bytes;

  if (cache_con->total_buffer_size) {
  	copybytes = *n_bytes > cache_con->total_buffer_size ?
		cache_con->total_buffer_size : *n_bytes;
	memcpy (dataptr, cache_con->cache_read, copybytes);
	cache_con->total_buffer_size -= copybytes;
	dataptr += copybytes;
	if (!cache_con->total_buffer_size) {
		free (cache_con->buffer_cache);
		cache_con->buffer_cache = cache_con->cache_read = NULL;
	} else {
		cache_con->cache_read += copybytes;
		return 0;
	}
  	local_n_bytes -= copybytes;
  }
  copied = cache_con->last_buffer_size - cinfo->src->bytes_in_buffer;
  copybytes = local_n_bytes > copied ?
		copied : local_n_bytes;

  memcpy (dataptr, cache_con->current_read, copybytes);
  cache_con->last_buffer_size -= copybytes;
  dataptr += copybytes;

  if (cache_con->last_buffer_size == cinfo->src->bytes_in_buffer) {
	cache_con->current_start = cache_con->current_read = NULL;
	cache_con->last_buffer_size = 0;
  	local_n_bytes -= copybytes;
	copied = *n_bytes - local_n_bytes;
	jpeg_src_read(private_data, &local_n_bytes, dataptr);
	*n_bytes = copied + local_n_bytes;
	context->sops->read = jpeg_src_read;
  } else {
	cache_con->current_read += copybytes;
	return 0;
  }
  return 0;
}

int jpeg_src_read (void *private_data, size_t *n_bytes, void *dataptr) {
  cinfo_context *cctx = (cinfo_context *) private_data;
  j_decompress_ptr cinfo = (j_decompress_ptr) cctx->cinfo;

  struct jpeg_source_mgr *src = cinfo->src;
  int datacnt = *n_bytes;
  int command_byte = 0;
  JOCTET *bufend;
  while (src->bytes_in_buffer < datacnt)  {
	/*no end-of-file condition is returned from the callback
          so we look for an EOI tag at the end of the data*/
	bufend = (JOCTET *) src->next_input_byte + src->bytes_in_buffer - 2;
	if (src->bytes_in_buffer == 0) {
		*n_bytes -= datacnt;
		return 0;
	} else if (src->bytes_in_buffer == 1 && command_byte) {
		if (*(bufend+1) == JPEG_EOI) {
			*n_bytes -= (datacnt - src->bytes_in_buffer);
			datacnt = src->bytes_in_buffer;
			break;
		}
	} else if (*(bufend) == 0xFF && *(bufend+1) == JPEG_EOI) {
		*n_bytes -= (datacnt - src->bytes_in_buffer);
		datacnt = src->bytes_in_buffer;
		break;
	}
	memcpy(dataptr, src->next_input_byte, src->bytes_in_buffer);
	datacnt -= src->bytes_in_buffer;
	dataptr += src->bytes_in_buffer;
	src->fill_input_buffer(cinfo);

	bufend = (JOCTET *) src->next_input_byte + src->bytes_in_buffer - 1;
	if (*(bufend) == 0xFF) {
		command_byte = 1;
	} else {
		command_byte = 0;
	}
  }
  memcpy(dataptr, src->next_input_byte, datacnt);
  src->bytes_in_buffer -= datacnt;
  return 0;
}

int jpeg_dest_init(void *private_data) {
  cinfo_context *cctx = (cinfo_context *) private_data;
  j_compress_ptr cinfo = (j_compress_ptr) cctx->cinfo;

  cinfo->dest->init_destination(cinfo);
  return 0;
}

void jpeg_dest_finalize(void *private_data) {
  cinfo_context *cctx = (cinfo_context *) private_data;
  j_compress_ptr cinfo = (j_compress_ptr) cctx->cinfo;

  cinfo->dest->term_destination(cinfo);

}

int jpeg_dest_write (void *private_data, size_t *n_bytes, void *dataptr) {
  cinfo_context *cctx = (cinfo_context *) private_data;
  j_compress_ptr cinfo = (j_compress_ptr) cctx->cinfo;

  struct jpeg_destination_mgr *dest = cinfo->dest;
  int datacnt = *n_bytes;
  while (cinfo->dest->free_in_buffer < datacnt)  {
	memcpy(dest->next_output_byte, dataptr, dest->free_in_buffer);
	datacnt -= dest->free_in_buffer;
	dataptr += dest->free_in_buffer;
	dest->empty_output_buffer(cinfo);
  }
  memcpy(dest->next_output_byte, dataptr, datacnt);
  dest->free_in_buffer -= datacnt;
  dest->term_destination(cinfo);
  return TRUE;
}

