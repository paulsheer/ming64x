/* pairheap.c - Paul Sheer <paulsheer@gmail.com> */

#define SWAP(t,a,b)   do {    t __t;        \
                              __t = (a);    \
                              (a) = (b);    \
                              (b) = __t;    } while (0)

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "pairheap.h"


struct pairheap {
#define PAIRHEAP_MAGIC                  0xa52d
    unsigned int magic;
    int off;
    struct pairheap_node *root;
    pairheap_cmp_t pairheap_cmp_fn;
    void *user_data;
};

struct pairheap *pairheap_alloc (pairheap_cmp_t fn, void *user_data, int off)
{
    struct pairheap *o;
    o = (struct pairheap *) malloc (sizeof (struct pairheap));
    memset (o, '\0', sizeof (*o));
    o->magic = PAIRHEAP_MAGIC;
    o->off = off;
    o->pairheap_cmp_fn = fn;
    o->user_data = user_data;
    return o;
}

void pairheap_free (struct pairheap *rh)
{
    free (rh);
}

static struct pairheap_node *pairheap_merge (struct pairheap *rh,
                                             struct pairheap_node *a,
                                             struct pairheap_node *b)
{
    if ((*rh->pairheap_cmp_fn) (rh->user_data, (char *) a - rh->off,
                                (char *) b - rh->off) < 0) {
        if (a->child != NULL)
            a->child->prev = b;
        if (b->next != NULL)
            b->next->prev = a;
        a->next = b->next;
        b->next = a->child;
        a->child = b;
        b->prev = a;
        return a;
    }
    if (b->child != NULL)
        b->child->prev = a;
    if (a->prev != NULL && a->prev->child != a)
        a->prev->next = b;
    b->prev = a->prev;
    a->prev = b;
    a->next = b->child;
    b->child = a;
    return b;
}

void pairheap_insert (struct pairheap *rh, void *n)
{
    struct pairheap_node *p = (struct pairheap_node *) ((char *) n + rh->off);

    p->prev = NULL;
    p->next = NULL;
    p->child = NULL;

    if (rh->root == NULL)
        rh->root = p;
    else
        rh->root = pairheap_merge (rh, p, rh->root);
}

static struct pairheap_node *pairheap_merge_right (struct pairheap *rh,
                                                   struct pairheap_node *a)
{
    struct pairheap_node *b;
    for (b = NULL; a != NULL; a = b->next) {
        if ((b = a->next) == NULL)
            return a;
        b = pairheap_merge (rh, a, b);
    }
    return b;
}

static struct pairheap_node *pairheap_merge_left (struct pairheap *rh,
                                                  struct pairheap_node *a)
{
    struct pairheap_node *b;
    for (b = a->prev; b != NULL; b = a->prev)
        a = pairheap_merge (rh, b, a);
    return a;
}

static struct pairheap_node *pairheap_merge_subheaps (struct pairheap *rh,
                                                      struct pairheap_node *a)
{
    struct pairheap_node *e;

    a->child->prev = NULL;
    e = pairheap_merge_right (rh, a->child);
    e = pairheap_merge_left (rh, e);

    return e;
}

void *pairheap_extremum (struct pairheap *rh)
{
    if (!rh->root)
        return NULL;
    return (char *) rh->root - rh->off;
}

void *pairheap_remove_extremum (struct pairheap *rh)
{
    void *ret;

    if (!rh->root)
        return NULL;

    ret = (char *) rh->root - rh->off;

    if (rh->root->child == NULL) {
        rh->root = NULL;
        return ret;
    }

    rh->root = pairheap_merge_subheaps (rh, rh->root);

    return ret;
}

static void pairheap_detach_subheap (struct pairheap_node *p)
{
    if (p->prev->child == p)
        p->prev->child = p->next;
    else
        p->prev->next = p->next;

    if (p->next != NULL)
        p->next->prev = p->prev;

    p->prev = NULL;
    p->next = NULL;
}

void pairheap_remove (struct pairheap *rh, struct pairheap_node *n)
{
    struct pairheap_node *p = n;

    if (p == rh->root) {
        (void) pairheap_remove_extremum (rh);
        return;
    }

    pairheap_detach_subheap (p);
    if (p->child) {
        p = pairheap_merge_subheaps (rh, p);
        rh->root = pairheap_merge (rh, rh->root, p);
    }

    n->prev = NULL;
    n->next = NULL;
    n->child = NULL;
}



