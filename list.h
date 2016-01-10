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

static inline void list_add(struct list_head *head, struct list_head *item)
{
	item->next = head;
	item->prev = head->prev;
	head->prev->next = item;
	head->prev = item;
}

static inline void list_del(struct list_head *item)
{
	item->prev->next = item->next;
	item->next->prev = item->prev;
}

#define container_of(ptr, type, member) __extension__ ({ \
	const __typeof__(((type *) 0)->member) * __mptr = (ptr); \
	(type *) ((char *) __mptr - offsetof(type, member)); })

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_walk(curr, head) \
	for (curr = (head)->next; curr != (head); curr = curr->next)

#define list_walk_safe(curr, temp, head) \
	for (curr = (head)->next, temp = curr->next; curr != (head); \
		curr = temp, temp = curr->next)

#endif /* __LIST_H__ */
