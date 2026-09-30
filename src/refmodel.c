// SPDX-License-Identifier: MIT
#include "refmodel.h"

#include <stdlib.h>
#include <string.h>

#include "ftl.h"

int ref_init(struct refmodel *r, uint32_t pages, uint32_t page_size)
{
	r->pages = pages;
	r->page_size = page_size;
	r->data = calloc((size_t)pages, page_size);
	r->mapped = calloc(pages, sizeof(*r->mapped));
	if (!r->data || !r->mapped) {
		ref_free(r);
		return FTL_ENOMEM;
	}
	return FTL_OK;
}

void ref_free(struct refmodel *r)
{
	free(r->data);
	free(r->mapped);
	r->data = NULL;
	r->mapped = NULL;
}

int ref_write(struct refmodel *r, uint32_t lpn, const void *data)
{
	if (!data)
		return FTL_EINVAL;
	if (lpn >= r->pages)
		return FTL_ERANGE;
	memcpy(r->data + (size_t)lpn * r->page_size, data, r->page_size);
	r->mapped[lpn] = true;
	return FTL_OK;
}

int ref_read(const struct refmodel *r, uint32_t lpn, void *data)
{
	if (!data)
		return FTL_EINVAL;
	if (lpn >= r->pages)
		return FTL_ERANGE;
	if (!r->mapped[lpn]) {
		memset(data, 0, r->page_size);
		return FTL_UNMAPPED;
	}
	memcpy(data, r->data + (size_t)lpn * r->page_size, r->page_size);
	return FTL_OK;
}

int ref_discard(struct refmodel *r, uint32_t lpn)
{
	if (lpn >= r->pages)
		return FTL_ERANGE;
	r->mapped[lpn] = false;
	memset(r->data + (size_t)lpn * r->page_size, 0, r->page_size);
	return FTL_OK;
}
