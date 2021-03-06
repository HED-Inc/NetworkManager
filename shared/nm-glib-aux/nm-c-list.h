/* NetworkManager -- Network link manager
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * (C) Copyright 2014 Red Hat, Inc.
 */

#ifndef __NM_C_LIST_H__
#define __NM_C_LIST_H__

#include "c-list/src/c-list.h"

/*****************************************************************************/

#define nm_c_list_contains_entry(list, what, member) \
	({ \
		typeof (what) _what = (what); \
		\
		_what && c_list_contains (list, &_what->member); \
	})

/*****************************************************************************/

typedef struct {
	CList lst;
	void *data;
} NMCListElem;

static inline NMCListElem *
nm_c_list_elem_new_stale (void *data)
{
	NMCListElem *elem;

	elem = g_slice_new (NMCListElem);
	elem->data = data;
	return elem;
}

static inline gboolean
nm_c_list_elem_free_full (NMCListElem *elem, GDestroyNotify free_fcn)
{
	if (!elem)
		return FALSE;
	c_list_unlink_stale (&elem->lst);
	if (free_fcn)
		free_fcn (elem->data);
	g_slice_free (NMCListElem, elem);
	return TRUE;
}

static inline gboolean
nm_c_list_elem_free (NMCListElem *elem)
{
	return nm_c_list_elem_free_full (elem, NULL);
}

static inline void
nm_c_list_elem_free_all (CList *head, GDestroyNotify free_fcn)
{
	NMCListElem *elem;

	while ((elem = c_list_first_entry (head, NMCListElem, lst)))
		nm_c_list_elem_free_full (elem, free_fcn);
}

/**
 * nm_c_list_elem_find_first:
 * @head: the @CList head of a list containing #NMCListElem elements.
 *   Note that the head is not itself part of the list.
 * @needle: the needle pointer.
 *
 * Iterates the list and returns the first #NMCListElem with the matching @needle,
 * using pointer equality.
 *
 * Returns: the found list element or %NULL if not found.
 */
static inline NMCListElem *
nm_c_list_elem_find_first (CList *head, gconstpointer needle)
{
	NMCListElem *elem;

	c_list_for_each_entry (elem, head, lst) {
		if (elem->data == needle)
			return elem;
	}
	return NULL;
}

/*****************************************************************************/

static inline gboolean
nm_c_list_move_before (CList *lst, CList *elem)
{
	nm_assert (lst);
	nm_assert (elem);
	nm_assert (c_list_contains (lst, elem));

	if (   lst != elem
	    && lst->prev != elem) {
		c_list_unlink_stale (elem);
		c_list_link_before (lst, elem);
		return TRUE;
	}
	return FALSE;
}
#define nm_c_list_move_tail(lst, elem) nm_c_list_move_before (lst, elem)

static inline gboolean
nm_c_list_move_after (CList *lst, CList *elem)
{
	nm_assert (lst);
	nm_assert (elem);
	nm_assert (c_list_contains (lst, elem));

	if (   lst != elem
	    && lst->next != elem) {
		c_list_unlink_stale (elem);
		c_list_link_after (lst, elem);
		return TRUE;
	}
	return FALSE;
}
#define nm_c_list_move_front(lst, elem) nm_c_list_move_after (lst, elem)

#endif /* __NM_C_LIST_H__ */
