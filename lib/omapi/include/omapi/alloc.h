/*
 * Copyright (C) 1996, 1997, 1998, 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#ifndef OMAPI_ALLOC_H
#define OMAPI_ALLOC_H 1

/*****
 ***** Definitions for the object management API protocol memory allocation.
 *****/

#include <isc/lang.h>

#include <omapi/omapip.h>

ISC_LANG_BEGINDECLS

isc_result_t
omapi_buffer_new(omapi_buffer_t **buffer, const char *name);

void
omapi_buffer_reference(omapi_buffer_t **bufferp, omapi_buffer_t *buffer,
		       const char *);

void
omapi_buffer_dereference(omapi_buffer_t **bufferp, const char *name);

ISC_LANG_ENDDECLS

#endif /* OMAPI_ALLOC_H */
