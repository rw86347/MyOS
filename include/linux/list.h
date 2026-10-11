/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_LIST_H
#define LINUX_LIST_H

/*
 * Linux's doubly linked lists. The links live INSIDE the objects (a
 * struct list_head member); a list is a ring through a head node, so
 * adding and removing are O(1) with no special cases for the ends.
 * container_of turns a pointer to the link back into the object.
 */
#include <linux/compiler.h>
#include <stddef.h>

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

struct list_head { struct list_head *next, *prev; };
struct hlist_head { struct hlist_node *first; };
struct hlist_node { struct hlist_node *next, **pprev; };

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name)      struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }

static inline void __list_add(struct list_head *n, struct list_head *prev, struct list_head *next)
{
    next->prev = n;
    n->next = next;
    n->prev = prev;
    WRITE_ONCE(prev->next, n);
}
static inline void list_add(struct list_head *n, struct list_head *head) { __list_add(n, head, head->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *head) { __list_add(n, head->prev, head); }
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    WRITE_ONCE(prev->next, next);
}
static inline void list_del(struct list_head *e)
{
    __list_del(e->prev, e->next);
    e->next = e->prev = NULL;           /* Linux poisons; NULL faults just as well */
}
static inline void list_del_init(struct list_head *e) { __list_del(e->prev, e->next); INIT_LIST_HEAD(e); }
static inline void list_move(struct list_head *e, struct list_head *head) { __list_del(e->prev, e->next); list_add(e, head); }
static inline void list_move_tail(struct list_head *e, struct list_head *head) { __list_del(e->prev, e->next); list_add_tail(e, head); }
static inline void list_replace(struct list_head *old, struct list_head *n)
{
    n->next = old->next; n->next->prev = n; n->prev = old->prev; n->prev->next = n;
}
static inline int list_empty(const struct list_head *head) { return READ_ONCE(head->next) == head; }
static inline int list_is_singular(const struct list_head *h) { return !list_empty(h) && h->next == h->prev; }
static inline int list_is_last(const struct list_head *e, const struct list_head *head) { return e->next == head; }
static inline int list_is_first(const struct list_head *e, const struct list_head *head) { return e->prev == head; }
static inline void list_splice_init(struct list_head *list, struct list_head *head)
{
    if (list_empty(list))
        return;
    struct list_head *first = list->next, *last = list->prev, *at = head->next;
    first->prev = head; head->next = first; last->next = at; at->prev = last;
    INIT_LIST_HEAD(list);
}
static inline void list_splice_tail_init(struct list_head *list, struct list_head *head)
{
    if (list_empty(list))
        return;
    struct list_head *first = list->next, *last = list->prev, *at = head->prev;
    first->prev = at; at->next = first; last->next = head; head->prev = last;
    INIT_LIST_HEAD(list);
}
/* The same without emptying `list` (it is left pointing into head). */
static inline void list_splice(const struct list_head *list, struct list_head *head)
{
    if (list_empty(list))
        return;
    struct list_head *first = list->next, *last = list->prev, *at = head->next;
    first->prev = head; head->next = first; last->next = at; at->prev = last;
}
static inline void list_splice_tail(struct list_head *list, struct list_head *head)
{
    if (list_empty(list))
        return;
    struct list_head *first = list->next, *last = list->prev, *at = head->prev;
    first->prev = at; at->next = first; last->next = head; head->prev = last;
}
static inline void list_replace_init(struct list_head *old, struct list_head *n)
{
    list_replace(old, n);
    INIT_LIST_HEAD(old);
}
static inline size_t list_count_nodes(const struct list_head *head)
{
    size_t n = 0;
    for (const struct list_head *p = head->next; p != head; p = p->next) n++;
    return n;
}

#define list_entry(ptr, type, member)        container_of(ptr, type, member)
#define list_first_entry(head, type, member) list_entry((head)->next, type, member)
#define list_last_entry(head, type, member)  list_entry((head)->prev, type, member)
#define list_first_entry_or_null(head, type, member) \
    (list_empty(head) ? NULL : list_first_entry(head, type, member))
#define list_next_entry(pos, member)  list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_prev_entry(pos, member)  list_entry((pos)->member.prev, __typeof__(*(pos)), member)
#define list_entry_is_head(pos, head, member) (&(pos)->member == (head))
#define list_for_each(p, head) for (p = (head)->next; p != (head); p = p->next)
#define list_for_each_safe(p, n, head) for (p = (head)->next, n = p->next; p != (head); p = n, n = p->next)
#define list_for_each_entry(pos, head, member) \
    for (pos = list_first_entry(head, __typeof__(*pos), member); \
         !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member) \
    for (pos = list_last_entry(head, __typeof__(*pos), member); \
         !list_entry_is_head(pos, head, member); pos = list_prev_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_first_entry(head, __typeof__(*pos), member), n = list_next_entry(pos, member); \
         !list_entry_is_head(pos, head, member); pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_continue(pos, head, member) \
    for (pos = list_next_entry(pos, member); !list_entry_is_head(pos, head, member); \
         pos = list_next_entry(pos, member))
#define list_for_each_entry_from(pos, head, member) \
    for (; !list_entry_is_head(pos, head, member); pos = list_next_entry(pos, member))

/* hlist: a list with a one-pointer head, for hash tables. */
#define HLIST_HEAD_INIT { .first = NULL }
#define INIT_HLIST_HEAD(h) ((h)->first = NULL)
static inline void INIT_HLIST_NODE(struct hlist_node *n) { n->next = NULL; n->pprev = NULL; }
static inline int hlist_empty(const struct hlist_head *h) { return !READ_ONCE(h->first); }
static inline int hlist_unhashed(const struct hlist_node *n) { return !n->pprev; }
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
    struct hlist_node *first = h->first;
    n->next = first;
    if (first) first->pprev = &n->next;
    WRITE_ONCE(h->first, n);
    n->pprev = &h->first;
}
static inline void hlist_del(struct hlist_node *n)
{
    struct hlist_node *next = n->next, **pprev = n->pprev;
    WRITE_ONCE(*pprev, next);
    if (next) next->pprev = pprev;
    n->next = NULL; n->pprev = NULL;
}
static inline void hlist_del_init(struct hlist_node *n) { if (!hlist_unhashed(n)) hlist_del(n); }
#define hlist_entry(ptr, type, member) container_of(ptr, type, member)
#define hlist_entry_safe(ptr, type, member) \
    ({ __typeof__(ptr) ____ptr = (ptr); ____ptr ? hlist_entry(____ptr, type, member) : NULL; })
#define hlist_for_each_entry(pos, head, member) \
    for (pos = hlist_entry_safe((head)->first, __typeof__(*(pos)), member); pos; \
         pos = hlist_entry_safe((pos)->member.next, __typeof__(*(pos)), member))
#define hlist_for_each_entry_safe(pos, n, head, member) \
    for (pos = hlist_entry_safe((head)->first, __typeof__(*pos), member); \
         pos && ({ n = pos->member.next; 1; }); \
         pos = hlist_entry_safe(n, __typeof__(*pos), member))

#endif
