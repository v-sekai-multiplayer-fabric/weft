#ifndef LIST_H_
#define LIST_H_

#include <stdbool.h>
#include <stddef.h>

typedef struct list_t {
    struct list_t *next;
} list_t;

typedef struct {
    list_t *head;
    list_t **tail;
} queue_t;

static inline void queue_init(queue_t *q)
{
    q->head = NULL;
    q->tail = &q->head;
}

static inline void queue_push(queue_t *q, list_t *node)
{
    node->next = NULL;
    *q->tail = node;
    q->tail = &node->next;
}

static inline list_t *queue_pop(queue_t *q)
{
    list_t *node = q->head;
    if (node) {
        q->head = node->next;
        if (!q->head)
            q->tail = &q->head;
    }
    return node;
}

static inline bool queue_is_empty(queue_t *q)
{
    return q->head == NULL;
}

#endif
