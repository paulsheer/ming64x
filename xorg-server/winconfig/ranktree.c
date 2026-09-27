/* ranktree.c - Paul Sheer <paulsheer@gmail.com> */

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include "ranktree.h"

#ifndef INT_MAX
#error INT_MAX not defined
#endif

#if INT_MAX == 32767
#define RANKTREE_MAX_DEPTH      48
#else
#define RANKTREE_MAX_DEPTH      96
#endif
#define IMBALANCE               3       /* best worse-case results */

struct ranktree {
#define RANKTREE_MAGIC                  0xbdda
    unsigned int magic;
    int off;
    struct ranktree_node *root;
    ranktree_cmp_t ranktree_cmp_fn;
    void *user_data;
    struct ranktree_iterator *iter_list;
    struct ranktree_node **p[RANKTREE_MAX_DEPTH];       /* temporary variable */
};

enum frame_state {
    ST_INV = -1,
    ST_LEFT = 0,
    ST_CENTER = 1,
    ST_RIGHT = 2,
    ST_UP = 3,
};

struct ranktree_frame {
    struct ranktree_node *n;
    enum frame_state state;
};

struct ranktree_iterator {
#define RANKTREE_ITERATOR_MAGIC         0xb235
    unsigned int magic;
    struct ranktree *ranktree;
    struct ranktree_iterator *iter_next;
    struct ranktree_frame stack[RANKTREE_MAX_DEPTH];
    struct ranktree_frame *top;
    struct ranktree_node *current;
    int bumped;   /* an remove operation has bumped the iterator to the right */
    size_t index;
    int depth;
};

#define COUNT(n)        ((n) ? (n)->count : 0)
#define ELEM(i)         ((void *) ((char *) (i) - o->off))
#define NODE(i)         ((struct ranktree_node *) ((char *) (i) + o->off))

struct ranktree *ranktree_alloc (ranktree_cmp_t fn, void *user_data, const int off)
{
    struct ranktree *o;
    o = (struct ranktree *) malloc (sizeof (struct ranktree));
    memset (o, '\0', sizeof (*o));
    o->magic = RANKTREE_MAGIC;
    o->off = off;
    o->ranktree_cmp_fn = fn;
    o->user_data = user_data;
    return o;
}

void ranktree_free (struct ranktree *o)
{
    assert (!o->iter_list);
    assert (o->magic == RANKTREE_MAGIC);
    o->magic = 0;
    free (o);
}

size_t ranktree_count (struct ranktree *o)
{
    return COUNT (o->root);
}

#define ROTATE(leg_a,leg_b) \
static void ranktree_rotate_##leg_a (struct ranktree_node **n) \
{ \
    struct ranktree_node *x, *y, *b; \
    y = *n; \
    if (!(x = y->leg_a)) \
        return; \
    b = x->leg_b; \
    y->leg_a = NULL, y->count -= x->count; \
    x->leg_b = NULL, x->count -= COUNT (b); \
    y->leg_a = b, y->count += COUNT (b); \
    x->leg_b = y, x->count += y->count; \
    *n = x; \
}

ROTATE (left, right)
    ROTATE (right, left)
#if !defined(ULONG_MAX) || !defined(SIZE_MAX) || !defined(UINT_MAX)
typedef unsigned long long big_size_t;
#elif UINT_MAX > SIZE_MAX
typedef unsigned int big_size_t;
#elif ULONG_MAX > SIZE_MAX
typedef unsigned long big_size_t;
#else
typedef unsigned long long big_size_t;
#endif

static void ranktree_iterator_rebuild (struct ranktree_iterator *i,
                                       struct ranktree_node *n,
                                       const int direction);

static void rebalance (struct ranktree *o, const int d, struct ranktree_node *n,
                       const int direction)
{
    struct ranktree_iterator *iter;
    struct ranktree_node ***p = o->p;
    int j;
    /* check if insert overflows the count: */
    assert (!(direction == -1 && o->root->count == 0));
    for (j = d; j >= 0; j--) {
        size_t cl, cr;
        struct ranktree_node *q = *p[j];
        cl = COUNT (q->left);
        cr = COUNT (q->right);
        if ((big_size_t) cl > (big_size_t) cr * IMBALANCE)
            ranktree_rotate_left (p[j]);
        if ((big_size_t) cr > (big_size_t) cl * IMBALANCE)
            ranktree_rotate_right (p[j]);
    }
    for (iter = o->iter_list; iter; iter = iter->iter_next)
        ranktree_iterator_rebuild (iter, n, direction);
}

#define CMP(a,b)     (*o->ranktree_cmp_fn) (o->user_data, ELEM (a), (b))

static inline int compare (struct ranktree *o, const struct ranktree_node *a_,
                           const void *b)
{
    const void *a = ELEM (a_);
    int c;

    if ((c = (*o->ranktree_cmp_fn) (o->user_data, a, b)))
        return c;
    return a > b ? 1 : (a < b ? -1 : 0);
}

void *ranktree_find (const struct ranktree *o, const void *elem)
{
    struct ranktree_node *i;

    for (i = o->root; i;) {
        int c;
        if ((c = CMP (i, elem)) < 0)
            i = i->right;
        else if (c > 0)
            i = i->left;
        else {
            void *r = ELEM (i);
            assert (r != elem); /* prevent multi-map misuse */
            return r;
        }
    }
    return NULL;
}

void *ranktree_index (struct ranktree *o, const size_t index)
{
    struct ranktree_node *i, *n = o->root;
    size_t c;
    if (!n)
        return NULL;
    c = COUNT (n->left);
    while (n) {
        i = n;
        if (index < c) {
            if ((n = n->left))
                c -= 1 + COUNT (n->right);
        } else if (index > c) {
            if ((n = n->right))
                c += 1 + COUNT (n->left);
        } else {
            return ELEM (i);
        }
    }
    return NULL;
}

static void *ranktree_find_get_index (struct ranktree *o, const void *elem,
                                      size_t *index, const int exact)
{
    size_t j = 0, index_last = 0;
    struct ranktree_node *n, *last = NULL, *found = NULL;
    if ((n = o->root))
        j = COUNT (n->left);
    while (n) {
        int c;
        last = n;
        index_last = j;
        if (!(c = CMP (n, elem))) {
            if (found)      /* find the left-most of multiple identical nodes */
                c = compare (o, found, ELEM (n));
            found = n;
            *index = j;
        }
        if (c >= 0) {
            if ((n = n->left))
                j -= 1 + COUNT (n->right);
        } else if (c < 0) {
            if ((n = n->right))
                j += 1 + COUNT (n->left);
        }
    }
    if (!exact && !found && last)
        found = last, *index = index_last;
    if (exact) {
        assert (found);
        assert (ELEM (found) == elem);  /* use this only with connected real
                                                                      element */
    } else if (found)
        assert (ELEM (found) != elem);  /* use this only with unconnected search
                                                                      element */
    if (found && CMP (found, elem) < 0) {       /* edge case */
        (*index)++;
        return ranktree_index (o, *index);
    }
    return found ? ELEM (found) : NULL;
}

void *ranktree_find_index (struct ranktree *o, const void *p, size_t *index)
{
    return ranktree_find_get_index (o, p, index, 0);
}

void *ranktree_rindex (struct ranktree *o, const size_t index)
{
    return ranktree_index (o, COUNT (o->root) - 1 - index);
}

/* returns existing element or NULL */
static void *ranktree_insert_replace_ (struct ranktree *o, void *elem,
                                       const int replace)
{
    struct ranktree_node *n, **found = NULL, *i, ***p = o->p;
    int d = 0;
    int j;

    n = NODE (elem);
    for (p[d] = &o->root, i = o->root; i;) {
        int c;
        if ((c = CMP (i, elem)) < 0)
            p[++d] = &i->right, i = i->right;
        else if (c > 0)
            p[++d] = &i->left, i = i->left;
        else if (!replace)
            return ELEM (i);
        else {              /* maintain best order of pointers when replacing */
            found = p[d];
            if ((c = compare (o, i, elem)) < 0)
                p[++d] = &i->right, i = i->right;
            else if (c > 0)
                p[++d] = &i->left, i = i->left;
            else
                assert (!"already in tree");
        }
    }
    assert (d < RANKTREE_MAX_DEPTH - 1);
    if (found) {
        struct ranktree_node *r;
        r = *found;
        *n = **found;
        *found = n;
        rebalance (o, -1, NULL, 0);
        r->count = 0;
        return ELEM (r);
    }
    for (j = 0; j < d; j++)
        (*p[j])->count++;
    n->left = n->right = NULL;
    n->count = 1;
    *p[d] = n;                  /* insert */
    rebalance (o, d, n, -1);
    return NULL;
}

void *ranktree_insert (struct ranktree *o, void *elem)
{
    return ranktree_insert_replace_ (o, elem, 0);
}

void *ranktree_insert_replace (struct ranktree *o, void *elem)
{
    return ranktree_insert_replace_ (o, elem, 1);
}

static void ranktree_rotate_left_for_removal_ (struct ranktree_node **n)
{
    struct ranktree_node *p, *t;
    t = *n;
    p = t->left;
    t->left = NULL, t->count -= p->count;
    *n = p, p->count += t->count;
    while (p->right)
        p = p->right, p->count += t->count;
    p->right = t;
}

static void ranktree_rotate_right_for_removal_ (struct ranktree_node **n)
{
    struct ranktree_node *p, *t;
    t = *n;
    p = t->right;
    t->right = NULL, t->count -= p->count;
    *n = p, p->count += t->count;
    while (p->left)
        p = p->left, p->count += t->count;
    p->left = t;
}

static void ranktree_rotate_for_removal (struct ranktree_node **n)
{
    big_size_t ll, lr, rl, rr;

    ll = COUNT ((*n)->left->left) + 1;
    lr = COUNT ((*n)->left->right) + COUNT (*n);
    rl = COUNT ((*n)->right->left) + COUNT (*n);
    rr = COUNT ((*n)->right->right) + 1;

    /* some rotations are better than others */
    if (16 * lr / ll < 16 * rl / rr)  /* 30% more difference in counts with > */
        ranktree_rotate_left_for_removal_ (n);
    else
        ranktree_rotate_right_for_removal_ (n);
}

static void *ranktree_find_remove_ (struct ranktree *o, const void *search,
                                    struct ranktree_node *found)
{
    struct ranktree_node *i, ***p = o->p;
    int d = 0, j;

    if (!o->root)
        return NULL;

    p[d] = &o->root, i = o->root;
    if (!found) {
        assert (search != NULL);
        for (;;) {
            int c;
            if ((c = CMP (i, search)) < 0)
                p[++d] = &i->right, i = i->right;
            else if (c > 0)
                p[++d] = &i->left, i = i->left;
            else {
                found = i;
                break;
            }
            if (!i)
                return NULL;
        }
    }
    for (;;) {
        if (i == found) {
            if (!i->right || !i->left)
                break;
            ranktree_rotate_for_removal (p[d]);
            i = *p[d];
            continue;           /* the node has been bumped down the tree, */
        }                       /* so keep building the path to find it again */
        assert (i);             /* not found */
        if (compare (o, i, ELEM (found)) < 0)
            p[++d] = &i->right, i = i->right;
        else
            p[++d] = &i->left, i = i->left;
    }
    assert (d < RANKTREE_MAX_DEPTH - 1);
    for (j = 0; j < d; j++)
        (*p[j])->count--;
    assert (*p[d] == i);
    *p[d] = i->right ? i->right : i->left;      /* delete node */
#ifdef CHECKS
    i->right = (struct ranktree_node *) (void *) -1;
    i->left = (struct ranktree_node *) (void *) -1;
#endif
    rebalance (o, d - 1, i, 1);
    i->count = 0;
    return ELEM (i);
}

int ranktree_is_linked (struct ranktree_node *n)
{
    return n->count != 0;
}

void ranktree_remove (struct ranktree *o, struct ranktree_node *n)
{
    assert (n->count != 0);
    ranktree_find_remove_ (o, NULL, n);
}

struct ranktree_iterator *ranktree_iterator_alloc (struct ranktree *ranktree)
{
    struct ranktree_iterator *j, *i;
    assert (ranktree->magic == RANKTREE_MAGIC);
    i = (struct ranktree_iterator *) malloc (sizeof (*i));
    memset (i, '\0', sizeof (*i));
    i->magic = RANKTREE_ITERATOR_MAGIC;
    i->ranktree = ranktree;
    i->iter_next = i->ranktree->iter_list;
    i->ranktree->iter_list = i;
    for (j = i->ranktree->iter_list; j; j = j->iter_next)
        assert (j->magic == RANKTREE_ITERATOR_MAGIC);
    return i;
}

void ranktree_iterator_free (struct ranktree_iterator *i)
{
    struct ranktree *o = i->ranktree;
    struct ranktree_iterator **p;
    assert (o->magic == RANKTREE_MAGIC);
    for (p = &o->iter_list; *p;) {
        assert ((*p)->magic == RANKTREE_ITERATOR_MAGIC);
        if (*p == i)
            *p = (*p)->iter_next;
        else
            p = &(*p)->iter_next;
    }
    i->magic = 0;
    free (i);
}

static struct ranktree_node *ranktree_iterator_next__ (struct ranktree_iterator
                                                       *i)
{
    struct ranktree_node *t;

  tail_recursion:
#ifdef CHECKS
    assert (i->top < &i->stack[RANKTREE_MAX_DEPTH - 1]);
#endif
    switch (i->top->state) {
    case ST_INV:
        assert (!"not allowed");
    case ST_LEFT:
        i->top->state = ST_CENTER;
        if ((t = i->top->n->left)) {
            i->top++;
            i->top->n = t;
            i->top->state = ST_LEFT;
            goto tail_recursion;
        }
        /* fall through */
    case ST_CENTER:
        i->top->state = ST_RIGHT;
        return i->top->n;
        /* fall through */
    case ST_RIGHT:
        i->top->state = ST_UP;
        if ((t = i->top->n->right)) {
            i->top++;
            i->top->n = t;
            i->top->state = ST_LEFT;
            goto tail_recursion;
        }
        /* fall through */
    case ST_UP:
        if (i->top == i->stack)
            return NULL;
        i->top->state = ST_INV; /* sanity check */
        i->top--;
        goto tail_recursion;
    }
    return NULL;
}

/* if search is NULL this rewinds to the first item,
 * otherwise it finds the item with the value of search */
void *ranktree_iterator_first (struct ranktree_iterator *i, const void *search)
{
    struct ranktree *o = i->ranktree;
    struct ranktree_node *p = o->root;
    struct ranktree_frame *f;
    size_t j = 0, last_index;
    int found = 0;

    i->top = NULL;
    i->bumped = 0;
    if (!p)
        return (void *) (i->current = NULL);
    j = COUNT (p->right);
    for (f = i->stack;; f++) {
        int c;
        f->n = p;
        last_index = j;
        if (!search) {
            c = 0;              /* always go left to find the first node */
            i->top = f, i->index = j;
        } else if (!(c = CMP (p, search))) {
            if (found++)
                c = compare (o, i->top->n, ELEM (p));   /* find the left-most of
                                                     multiple identical nodes */
            i->top = f, i->index = j;
        }
        if (c < 0) {
            if ((p = p->right))
                j -= 1 + COUNT (p->left);
            f->state = ST_UP;
        } else {
            if ((p = p->left))
                j += 1 + COUNT (p->right);
            f->state = ST_CENTER;
        }
        if (!p)
            break;
    }
    if (search && !found)
        i->top = f, i->index = last_index;
    if (!i->top)
        return (void *) (i->current = NULL);
    p = i->top->n;
    i->top->state = ST_RIGHT;
    if (search && CMP (p, search) < 0)  /* edge case */
        if (!(i->index--, p = ranktree_iterator_next__ (i)))
            return NULL;
    return i->current = p, ELEM (p);
}

static void *ranktree_iterator_rindex_ (struct ranktree_iterator *i,
                                        const size_t index)
{
    struct ranktree *o = i->ranktree;
    struct ranktree_node *p = o->root;
    struct ranktree_frame *f;
    size_t j = 0;

    i->current = NULL;
    i->top = NULL;
    if (!p)
        return NULL;
    j = COUNT (p->right);
    for (f = i->stack; (f->n = p) != NULL; f++) {
        if (index < j) {
            if ((p = p->right))
                j -= 1 + COUNT (p->left);
            f->state = ST_UP;
        } else if (index > j) {
            if ((p = p->left))
                j += 1 + COUNT (p->right);
            f->state = ST_CENTER;
        } else {
            i->top = f;
            break;
        }
    }
    if (!i->top)
        return NULL;
    p = i->top->n;
    i->top->state = ST_RIGHT;
    return i->current = p, ELEM (p);
}

void *ranktree_iterator_next (struct ranktree_iterator *i)
{
    struct ranktree *o = i->ranktree;
    struct ranktree_node *r;
    if (!i->current || !i->index) {
        i->current = NULL;
        return NULL;
    }
    i->index--;
    if (i->bumped) {
        i->bumped = 0;
        return ELEM (i->current);
    }
    if (!(r = ranktree_iterator_next__ (i)))
        return (void *) (i->current = NULL);
    i->current = r;
    return ELEM (r);
}

static void ranktree_iterator_rebuild (struct ranktree_iterator *i,
                                       struct ranktree_node *n,
                                       const int direction)
{
    void *r;
    struct ranktree *o = i->ranktree;
    int c;

    if (!i->current)            /* not initialized */
        return;
    if (!n);                    /* pass */
    else if (!(c = compare (o, i->current, ELEM (n))))  /* Note 1 */
        i->index -= (size_t) i->bumped, i->bumped = 1;
    else if (c < 0) {
        if (direction < 0)
            i->index++;
        else if (direction > 0)
            i->index--;
    }
    r = ranktree_iterator_rindex_ (i, i->index - (size_t) i->bumped);
    i->current = r ? NODE (r) : NULL;
    assert (!(direction == 1 && i->current == n));
}
