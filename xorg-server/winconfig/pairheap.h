/* pairheap.h - Paul Sheer <paulsheer@gmail.com> */

struct pairheap_node {
    struct pairheap_node *prev;
    struct pairheap_node *next;
    struct pairheap_node *child;
};

struct pairheap;

typedef int (*pairheap_cmp_t) (void *user_data,
                              const void *a, const void *b);

struct pairheap *pairheap_alloc (pairheap_cmp_t fn,
                                  void *user_data, int off);
void pairheap_free (struct pairheap *rh);
void pairheap_insert (struct pairheap *rh, void *n);
void *pairheap_remove_extremum (struct pairheap *rh);
void pairheap_remove (struct pairheap *rh,
                      struct pairheap_node *n);
void *pairheap_extremum (struct pairheap *rh);

