#include <hash.h>
#include <list.h>
#include "vm/frame.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/swap.h"
#include <stdio.h>

/* The frame table */
static struct hash frame_table;

/* List containing all frame table entries that have read-only page (used for eviction) */
struct list read_only_page_fte_list;

/* List containing all frame table entries that have writable page (used for eviction) */
struct list writable_page_fte_list;

/* The table of shareable pages */
static struct hash shareable_table;

/* Locks for the frame table and table of shareable pages */
static struct lock frame_table_lock;
static struct lock shareable_table_lock;


static unsigned frame_hash_hash_func (const struct hash_elem *h, void *aux UNUSED);
static bool frame_hash_less_func (const struct hash_elem *h1_raw, const struct hash_elem *h2_raw, void *aux UNUSED);

static unsigned shareable_hash_hash_func (const struct hash_elem *h, void *aux UNUSED);
static bool shareable_hash_less_func (const struct hash_elem *h1_raw, const struct hash_elem *h2_raw, void *aux UNUSED);

static struct frame_table_entry* find_frame (void *kpage);
static struct frame_table_entry* get_evictee (void);
static struct frame_table_entry* find_evictee(struct list *frame_table_enties_list);
static bool frame_is_accessed (struct frame_table_entry* fte); 

/* Initialises the frame table and associated structs. */
void
frame_init (void)
{   
    hash_init (&frame_table, frame_hash_hash_func, frame_hash_less_func, NULL);
    lock_init (&frame_table_lock);
    hash_init (&shareable_table, shareable_hash_hash_func, shareable_hash_less_func, NULL);
    lock_init (&shareable_table_lock);
    list_init(&read_only_page_fte_list);
    list_init(&writable_page_fte_list);
}

void 
frame_install (void *kpage, void *upage, struct shareable_page *shpage, bool is_mmap)
{
  #ifdef VM

  lock_acquire (&frame_table_lock);
  /* Add entry to the frame table */
  // void *kpage = frame_get (f);ƒ
  struct frame_table_entry *fte = malloc (sizeof (struct frame_table_entry));

  if (fte)
    {
      /* Initialise frame table entry */
      fte->kpage = kpage;
      fte->upage = upage;
      fte->shpage = shpage;
      fte->is_mmap = is_mmap;
      list_init (&fte->page_table_refs);
      fte->evictable = false;
      fte->t = thread_current ();

      /* Initialise page_table_ref and add to list */
      struct page_table_ref *pgtr = malloc (sizeof (struct page_table_ref));
      if (!pgtr) {
        lock_release (&frame_table_lock);
        PANIC ("Malloc failed for page table ref"); 
      }
      pgtr->pd = thread_current ()->pagedir;
      pgtr->page = upage;
      list_push_back (&fte->page_table_refs, &pgtr->elem);

      /* Add entries to frame table and frame table entries lists */
      // pagedir_set_page (pgtr->pd, upage, kpage, writable);
      hash_insert (&frame_table, &fte->hash_elem);

      if (!pagedir_is_writable(fte->t->pagedir, fte->upage))
      {
        /* If fte contains read-only page, add it to read_only_page_fte_list */
        list_push_back(&read_only_page_fte_list, &fte->list_elem);
      }
      else
      {
        /* If fte contains writable page, add it to writable_page_fte_list */
        list_push_back(&writable_page_fte_list, &fte->list_elem);
      }

      lock_release (&frame_table_lock);

      /* If successful and shpage is set, add frame entry to shpage */
      if (shpage)
      {
        lock_acquire (&shareable_table_lock);
        shpage->frame = fte;
        lock_release (&shareable_table_lock);
      }
    } 
  else 
    {
      lock_release (&frame_table_lock);
      PANIC ("Malloc failed for frame table entry");
    }

  #endif
}

void
frame_augment(struct frame_table_entry* fte, uint32_t *pd, void *upage)
{
  struct page_table_ref *ptr = malloc (sizeof (struct page_table_ref));
  if (ptr)
  {
    ptr->pd = pd;
    ptr->page = upage;

    lock_acquire (&frame_table_lock);
    list_push_back (&fte->page_table_refs, &ptr->elem);
    lock_release (&frame_table_lock);
  } else {
    PANIC ("Malloc failed for page reference");
  }
}

void*
frame_get (enum palloc_flags f)
{   
    #ifndef VM
    return palloc_get_page (f);
    #else

    lock_acquire (&frame_table_lock);
    /* Try to get memory page */
    void *kpage = palloc_get_page(PAL_USER | f);
    
    if (kpage == NULL) 
      {
        /* Evict a page if there are no more pages */

        struct frame_table_entry *evictee = get_evictee ();

        if (pagedir_is_writable(evictee->t->pagedir, evictee->kpage) && pagedir_is_dirty(evictee->t->pagedir, evictee->kpage)) 
        {
          if (evictee->is_mmap) 
          {
            struct page_table_ref *page_ref = list_entry(list_head(&evictee->page_table_refs), struct page_table_ref, elem);
            spt_save_page(page_ref->pd, page_ref->page);
          }
          else 
          {
            size_t swap_slot = swap_out (evictee->kpage);
            set_page_to_swap (evictee->t->sup_page_table, evictee->upage, swap_slot);
          }
        } 

        frame_free (evictee->kpage);
        kpage = palloc_get_page (PAL_USER | f);
        ASSERT (kpage != NULL);
      }
    
    lock_release (&frame_table_lock);
    return kpage;
    #endif
}

/* Frees reference from pd & upage to frame, if it was the last reference triggers freeing of entire entry
   If upage is NULL free all references from pd */
void
frame_free_process (void *kpage, uint32_t *pd, void *upage)
{
  lock_acquire (&frame_table_lock);
  struct frame_table_entry *ft_entry = find_frame (kpage);
  struct list *page_refs = &ft_entry->page_table_refs;
  struct list_elem *e;
  struct page_table_ref *pr;

  e = list_begin (page_refs);
  while (e != list_end (page_refs))
  {
    pr = list_entry (e, struct page_table_ref, elem);
    e = list_next (e);
    if (pr->pd == pd && (upage == NULL || pr->page == upage))
      {
        pagedir_clear_page(pr->pd, pr->page);
        list_remove (&pr->elem);
        free (pr);
      }
  }
  
  if (list_empty (page_refs))
  {
    lock_release (&frame_table_lock);
    frame_free (kpage);
    return;
  }
  else
  { 
  lock_release (&frame_table_lock);
  }
}

/* Destroys entire frame table entry corresponding to kpage */
void 
frame_free (void *kpage)
{   
    #ifndef VM
    palloc_free_page (kpage);
    #else

    lock_acquire (&frame_table_lock);

    /* Ensure kpage is valid */
    ASSERT (kpage != NULL);
    ASSERT (is_kernel_vaddr (kpage));

    /* Find frame table entry */
    struct frame_table_entry *ft_entry = find_frame (kpage);

    if (ft_entry)
      {
        list_remove (&ft_entry->list_elem);

        /* Delete the entry from the frame table */
        struct hash_elem *he = hash_delete (&frame_table, &ft_entry->hash_elem);
        if (he)
          {
            /* Clear the page table entries pointing to the frame and free the structs */
            struct list *prs = &ft_entry->page_table_refs;
            while (!list_empty(prs))
              {
                struct page_table_ref *pr = list_entry 
                     (list_pop_front (prs), struct page_table_ref, elem);
                 pagedir_clear_page(pr->pd, pr->page);
                 free (pr);
              }
            /* Remove and free the corresponding shareable_page table entry, if existed */ 
            if (ft_entry->shpage)
            {
              lock_acquire (&shareable_table_lock);
	            hash_delete (&shareable_table, &ft_entry->shpage->elem);
	            lock_release (&shareable_table_lock);
              free (ft_entry->shpage);
            }
            palloc_free_page (kpage);
            free (ft_entry);
          }
      }
    /* Release the lock */
    lock_release (&frame_table_lock);
    #endif
}

static struct frame_table_entry*
find_frame (void *kpage)
{
  struct frame_table_entry fte_aux = {.kpage = kpage};
  struct hash_elem *he = hash_find (&frame_table, &fte_aux.hash_elem);
  if (he)
    {
      return hash_entry (he, struct frame_table_entry, hash_elem);
    }
  else 
    {
      return NULL;
    }
}

static unsigned 
frame_hash_hash_func (const struct hash_elem *h, void *aux UNUSED)
{
    struct frame_table_entry *fte = hash_entry (h, struct frame_table_entry, hash_elem);
    return hash_int ((int) fte->kpage);
}

static bool
frame_hash_less_func (const struct hash_elem *h1_raw, const struct hash_elem *h2_raw, void *aux UNUSED)
{
    struct frame_table_entry *h1 = hash_entry (h1_raw, struct frame_table_entry, hash_elem);
    struct frame_table_entry *h2 = hash_entry (h2_raw, struct frame_table_entry, hash_elem);
    return h1->kpage < h2->kpage;
}


static unsigned
shareable_hash_hash_func (const struct hash_elem *h, void *aux UNUSED)
{
  struct shareable_page *p = hash_entry (h, struct shareable_page, elem);
  int64_t pair = ((((int64_t)(int32_t) (p->file_inode)) << 32) + p->offset);
  return hash_bytes(&pair, 8);
}


static bool
shareable_hash_less_func (const struct hash_elem *h1_raw, const struct hash_elem *h2_raw, void *aux UNUSED)
{
  struct shareable_page *p1 = hash_entry (h1_raw, struct shareable_page, elem);
  struct shareable_page *p2 = hash_entry (h2_raw, struct shareable_page, elem);
  int64_t pair1 = ((((int64_t)(int32_t) p1->file_inode) << 32) + p1->offset);
  int64_t pair2 = ((((int64_t)(int32_t) p2->file_inode) << 32) + p2->offset);
  return pair1 < pair2;
}


/* Makes entry in the shareable_pages_table */
struct shareable_page* 
shareable_page_add (struct inode *file_inode, off_t offset)
{
  struct shareable_page *shpage = malloc (sizeof (struct shareable_page));
  if (shpage)
  {
    shpage->file_inode = file_inode;
    shpage->offset = offset;
    lock_acquire (&shareable_table_lock);
    hash_insert (&shareable_table, &shpage->elem);
    lock_release (&shareable_table_lock);
    return shpage;
  }
  else
  {
    return NULL;
  }
}

/* If the page is sharable in the frame, return its frame table entry, NULL otherwise */
struct frame_table_entry *
find_shareable_page (struct inode *file_inode, off_t offset)
{
  lock_acquire (&shareable_table_lock);
  struct shareable_page sp_aux = {.file_inode = file_inode, .offset = offset};
  struct hash_elem *h = hash_find (&shareable_table, &sp_aux.elem);
  if (h)
  {
    lock_release (&shareable_table_lock);
    return hash_entry (h, struct shareable_page, elem)->frame;
  }
  else
  {
    lock_release (&shareable_table_lock);
    return NULL;
  }
}

static struct frame_table_entry*
get_evictee (void)
{
  struct frame_table_entry *evictee;

  if (!list_empty(&read_only_page_fte_list))
  {
    evictee = find_evictee(&read_only_page_fte_list);
  }
  else
  {
    evictee = find_evictee(&writable_page_fte_list);
  }

  ASSERT(evictee);
  return evictee;
}

static struct frame_table_entry*
find_evictee (struct list *frame_table_entries_list)
{
  struct frame_table_entry *evictee;
  struct frame_table_entry *curr_fte;

  int size = (int) list_size (frame_table_entries_list);
  for (int i = 0; i < (3 * size); i++)
  {
    curr_fte = list_entry (list_pop_front (frame_table_entries_list), struct frame_table_entry, list_elem);
    ASSERT (curr_fte != NULL);
    if (pagedir_is_accessed(curr_fte->t->pagedir, curr_fte->upage))
    {
      pagedir_set_accessed(curr_fte->t->pagedir, curr_fte->upage, false);
      list_push_back(frame_table_entries_list, &curr_fte->list_elem);
      continue;
    }
    else
    {
      /* A page with accessed bit 0 is found */
      return curr_fte;
    }
  }

  if (evictee == NULL)
  {
    /* Since no pages with access bit 0 is found, clear the oldest element */
    evictee = list_entry(list_begin(frame_table_entries_list), struct frame_table_entry, list_elem);
  }

  return evictee;
}

static bool 
frame_is_accessed (struct frame_table_entry* fte) 
{
  bool non_accessed = true;
  struct page_table_ref *curr_page_table_ref;
  struct list_elem *curr_page_table_refs_elem = list_head(&fte->page_table_refs);
  while ((curr_page_table_refs_elem = list_next(curr_page_table_refs_elem)) != list_tail(&fte->page_table_refs)) {
    curr_page_table_ref = list_entry(curr_page_table_refs_elem, struct page_table_ref, elem);

    if (pagedir_is_accessed(curr_page_table_ref->pd, curr_page_table_ref->page)) {
      non_accessed = false;
      break;
    }
  }

  return non_accessed;
}
