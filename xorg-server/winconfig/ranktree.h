/* ranktree.h - Paul Sheer <paulsheer@gmail.com> */

struct ranktree_node {
    struct ranktree_node *left;
    struct ranktree_node *right;
    size_t count;
};

struct ranktree;

typedef int (*ranktree_cmp_t) (void *user_data,
                              const void *a, const void *b);

struct ranktree *ranktree_alloc (ranktree_cmp_t fn,
                                  void *user_data, int off);
void ranktree_free (struct ranktree *o);

void *ranktree_insert (struct ranktree *o, void *elem);
void *ranktree_insert_replace(struct ranktree *o,
                              void *elem);
void ranktree_remove (struct ranktree *o,
                      struct ranktree_node *n);
size_t ranktree_count (struct ranktree *o);
void *ranktree_find (const struct ranktree *o,
                     const void *elem);
int ranktree_is_linked (struct ranktree_node *n);

#define ranktree_first(o) ranktree_index(o, 0)
#define ranktree_last(o) ranktree_rindex(o, 0)
void *ranktree_index (struct ranktree *o, size_t index);
void *ranktree_rindex (struct ranktree *o, size_t index);
void *ranktree_find_index (struct ranktree *o, const void *p,
                           size_t *index);

struct ranktree_iterator;

struct ranktree_iterator *ranktree_iterator_alloc (
                                                   struct ranktree *ranktree);
void ranktree_iterator_free (struct ranktree_iterator *i);
void *ranktree_iterator_first (struct ranktree_iterator *i,
                               const void *search);
void *ranktree_iterator_next (struct ranktree_iterator *i);


