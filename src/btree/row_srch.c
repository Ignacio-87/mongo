/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __search_reset --
 *	Reset the cursor's state.
 */
static inline void
__search_reset(WT_CURSOR_BTREE *cbt)
{
	cbt->page = NULL;
	cbt->cip = NULL;
	cbt->rip = NULL;
	cbt->slot = UINT32_MAX;			/* Fail big. */

	cbt->ins_head = NULL;
	cbt->ins = NULL;

	cbt->match = 0;
	cbt->write_gen = 0;
}

/*
 * __search_insert --
 *	Search the slot's insert list.
 */
static inline WT_INSERT *
__search_insert(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt, WT_INSERT_HEAD *inshead, WT_ITEM *key)
{
	WT_BTREE *btree;
	WT_INSERT **ins;
	WT_ITEM insert_key;
	int cmp, i, (*compare)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	/* If there's no insert chain to search, we're done. */
	if (inshead == NULL)
		return (NULL);

	btree = session->btree;
	compare = btree->btree_compare;

	/*
	 * The insert list is a skip list: start at the highest skip level, then
	 * go as far as possible at each level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, ins = &inshead->head[i]; i >= 0; ) {
		if (*ins == NULL)
			cmp = -1;
		else {
			insert_key.data = WT_INSERT_KEY(*ins);
			insert_key.size = WT_INSERT_KEY_SIZE(*ins);
			cmp = compare(btree, key, &insert_key);
		}
		if (cmp == 0)			/* Exact match: return */
			return (*ins);
		else if (cmp > 0)		/* Keep going at this level */
			ins = &(*ins)->next[i];
		else				/* Drop down a level */
			cbt->ins_stack[i--] = ins--;
	}

	return (NULL);
}

/*
 * __wt_row_search --
 *	Search a row-store tree for a specific key.
 */
int
__wt_row_search(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_modify)
{
	WT_BTREE *btree;
	WT_IKEY *ikey;
	WT_ITEM *key, *item, _item;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_ROW_REF *rref;
	uint32_t base, indx, limit;
	int cmp, ret;
	int (*compare)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	key = (WT_ITEM *)&cbt->iface.key;

	__search_reset(cbt);

	btree = session->btree;
	rip = NULL;
	compare = btree->btree_compare;

	cmp = -1;				/* Assume we don't match. */

	/* Search the internal pages of the tree. */
	item = &_item;
	for (page = btree->root_page.page; page->type == WT_PAGE_ROW_INT;) {
		/* Binary search of internal pages. */
		for (base = 0, rref = NULL,
		    limit = page->entries; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			rref = page->u.row_int.t + indx;

			/*
			 * If we're about to compare an application key with the
			 * 0th index on an internal page, pretend the 0th index
			 * sorts less than any application key.  This test is so
			 * we don't have to update internal pages if the
			 * application stores a new, "smallest" key in the tree.
			 */
			if (indx != 0) {
				ikey = rref->key;
				item->data = WT_IKEY_DATA(ikey);
				item->size = ikey->size;

				cmp = compare(btree, key, item);
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;
			}
			base = indx + 1;
			--limit;
		}
		WT_ASSERT(session, rref != NULL);

		/*
		 * Reference the slot used for next step down the tree.
		 *
		 * Base is the smallest index greater than key and may be the
		 * (last + 1) index.  (Base cannot be the 0th index as the 0th
		 * index always sorts less than any application key).  The slot
		 * for descent is the one before base.
		 */
		if (cmp != 0)
			rref = page->u.row_int.t + (base - 1);

		/* Swap the parent page for the child page. */
		WT_ERR(__wt_page_in(session, page, &rref->ref, 0));
		__wt_page_release(session, page);
		page = WT_ROW_REF_PAGE(rref);
	}

	/*
	 * Copy the leaf page's write generation value before reading the page.
	 * Use a memory barrier to ensure we read the value before we read any
	 * of the page's contents.
	 */
	if (is_modify) {
		cbt->write_gen = page->write_gen;
		WT_MEMORY_FLUSH;
	}
	cbt->page = page;

	/* Do a binary search of the leaf page. */
	for (base = 0, limit = page->entries; limit != 0; limit >>= 1) {
		indx = base + (limit >> 1);
		rip = page->u.row_leaf.d + indx;

		/* The key may not have been instantiated yet. */
		if (__wt_off_page(page, rip->key)) {
			ikey = rip->key;
			_item.data = WT_IKEY_DATA(ikey);
			_item.size = ikey->size;
			item = &_item;
		} else {
			WT_ERR(
			    __wt_row_key(session, page, rip, &btree->key_srch));
			item = (WT_ITEM *)&btree->key_srch;
		}

		cmp = compare(btree, key, item);
		if (cmp == 0)
			break;
		if (cmp < 0)
			continue;

		base = indx + 1;
		--limit;
	}

	/*
	 * We now have a WT_ROW reference that's our best match on this search.
	 * The best case is finding an exact match in the page's WT_ROW slot
	 * array, which is likely for any read-mostly workload.
	 *
	 * In that case, we're not doing any kind of insert, all we can do is
	 * update an existing entry.  Check that case and get out fast.
	 */
	if (cmp == 0) {
		WT_ASSERT(session, rip != NULL);

		cbt->rip = rip;
		cbt->slot = WT_ROW_SLOT(page, rip);
		cbt->match = 1;
		return (0);
	}

	/*
	 * We didn't find an exact match in the WT_ROW array.
	 *
	 * Base is the smallest index greater than key and may be the 0th index
	 * or the (last + 1) index.  Set the WT_ROW reference to be the largest
	 * index less than the key if that's possible (if base is the 0th index
	 * it means the application is inserting a key before any key found on
	 * the page).
	 */
	rip = page->u.row_leaf.d;
	if (base != 0)
		rip += base - 1;
	cbt->rip = rip;

	/*
	 * It's still possible there is an exact match, but it's on an insert
	 * list.  Figure out which insert chain to search, and do the initial
	 * setup of the return information for the insert chain (we'll correct
	 * it as needed depending on what we find.)
	 *
	 * If inserting a key smaller than any key found in the WT_ROW array,
	 * use the extra slot of the insert array, otherwise insert lists map
	 * one-to-one to the WT_ROW array.
	 */
	if (base == 0) {
		cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
		cbt->slot = page->entries;		/* extra slot */
	} else {
		cbt->ins_head = WT_ROW_INSERT(page, rip);
		cbt->slot = WT_ROW_SLOT(page, rip);
	}

	/*
	 * Search the insert list for a match: if we don't find a match, we
	 * fail, unless we're inserting new data.
	 *
	 * No matter how things turn out, __wt_row_ins_search sets the return
	 * insert information appropriately, there's no more work to be done.
	 */
	cbt->ins = __search_insert(session, cbt, cbt->ins_head, key);
	cbt->match = cbt->ins == NULL ? 0 : 1;
	return (0);

err:	__wt_page_release(session, page);
	return (ret);
}
