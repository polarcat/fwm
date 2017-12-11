/*
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#ifndef __LIST_H__
#define __LIST_H__

#include <stddef.h>

struct list_head {
	struct list_head *prev;
	struct list_head *next;
};

static inline void list_init(struct list_head *head)
{
	head->next = head->prev = head;
}

static inline int list_single(struct list_head *head)
{
	return (head->next == head->prev && head->next != head);
}

static inline int list_empty(struct list_head *head)
{
	return (head->next == head);
}

static inline void list_top(struct list_head *head, struct list_head *item)
{
	item->next = head->next;
	head->next->prev = item;
	head->next = item;
	item->prev = head;
}

static inline void list_add(struct list_head *head, struct list_head *item)
{
	item->prev = head->prev;
	head->prev->next = item;
	head->prev = item;
	item->next = head;
}

static inline void list_del(struct list_head *item)
{
	item->prev->next = item->next;
	item->next->prev = item->prev;
}

static inline struct list_head *list_next(struct list_head *item,
					  struct list_head *head)
{
	if (item->next != head)
		return item->next;
	else if (item->next == head && item->next->next == head)
		return item;
	else if (item->next == head && item->next->next != head)
		return item->next->next;
	else
		return item;
}

static inline struct list_head *list_prev(struct list_head *item,
					  struct list_head *head)
{
	if (item->prev != head)
		return item->prev;
	else if (item->prev == head && item->prev->prev == head)
		return item;
	else if (item->prev == head && item->prev->prev != head)
		return item->prev->prev;
	else
		return item;
}

#define container_of(ptr, type, member) __extension__ ({\
	const __typeof__(((type *) 0)->member) * __mptr = (ptr);\
	(type *) ((char *) __mptr - offsetof(type, member)); })

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_walk(cur, head)\
	for (cur = (head)->next; cur != (head); cur = cur->next)

#define list_back(cur, head)\
	for (cur = (head)->prev; cur != (head); cur = cur->prev)

#define list_walk_safe(cur, temp, head)\
	for (cur = (head)->next, temp = cur->next; cur != (head);\
	     cur = temp, temp = cur->next)

#endif /* __LIST_H__ */
