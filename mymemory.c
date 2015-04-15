

/*****************************************************************************
***Developed by:    Shawn Krivorot
***Last Modified:   January 26, 2014
***Creation Date:   January 23, 2014
***Name:            mymemory_opt.c
***
***Description:     Wrapper function for malloc and free to keep control of heap
                    addressing, memory allocation and freeing unused space.

***Optimized Features:
      Coalescing
            - coalescing is now only taking place on the needed nodes rather
              than traversing the entire free_list and checking each node
              against the next. This bring it worst case upper bound from O(n)
              to O(2)->O(1). It becomes constant time because we will only check
              at most 2 nodes when coalescing.
      Expanding Heap
            - Instead of each time adding the minimum of 4096 bytes to the heap,
              we keep a counter of the number of times we have expanded, thus
              expanding the heap by size: 4096 * counter. This is based on the
              idea that, if a lot of space is malloc'd the probability of more 
              space being needed is higher. This does in some cases have a 
              reasonable amount of unused space at times, but it would save time
              from using sbrk() because the value of the time consumed to expand
              the heap is greater than the value of the amount of space unused.
      Locking
            - In order to save time when running, locking should be directly
              over the critical area so that as many commands as possible can 
              be performed before entering the waiting queue of a lock. This
              allows the lock to be held for shorter amounts of time, increasing
              concurent time.
******************************************************************************/


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#define BYTE_BOUNDARY 8
#define PAGE_SIZE 4096

//header for each section of memory allocated/freed
typedef struct header
{
  unsigned int size;
  struct header *next;
} header;

//head of free list with lock
typedef struct free_list
{
  header *head;
  pthread_mutex_t lock;
} free_list;

//prototypes of all functions
void insert_free_space(header *old_space);
header *find_free_space(unsigned int size);
void *shrink_free(header *free_space, unsigned int size);
int expand_heap();
void coalescing(header *parent);

//initial head of list
free_list *head_list;

//test address of next for all used blocks
header *used_head;
//counter for number of sbrk calls
int expansion_counter;
//size and begining of heap
unsigned int heap_size;
void *heap_head;

/**********************************************************
  Initializes all needed data structures and variables.
  Grabs a starting heap size from the OS using sbrk()
  and adds a free header to it.
  Returns 0 on success
  ERROR: 1 on failure
**********************************************************/
int mymalloc_init() 
{
  //first allocation of heap and starting point
  heap_head = sbrk(PAGE_SIZE);
  head_list = heap_head;
  //error checking for sbrk
  if(heap_head == (void *)-1)
  {
    return 1;
  }

  //location of head and initialization of lock
  head_list->head = (header *)((char *)(heap_head) + sizeof(free_list));
  pthread_mutex_init(&(head_list->lock), NULL);

  //tracking and testing variables
  expansion_counter = 1;
  used_head = (void *)-1;
  heap_size = PAGE_SIZE;

  //initialization of first header
  head_list->head->size = (unsigned int)PAGE_SIZE - sizeof(header) - sizeof(free_list);
  head_list->head->next = NULL;

  //successful return on 0
  return 0;
}

/**********************************************************
  Allocated memory on the heap in the amount of the requested size.
  Returns a pointer to after the header of the allocated block
  ERRORS: NULL if memory cannot be allocated or requested amount is 0.
**********************************************************/
void *mymalloc(unsigned int size) 
{
  //Check if requested size is valid
  if(size == 0)
  {
    return NULL;
  }

  //fixes 8-byte addressing issue
  if(size % BYTE_BOUNDARY != 0)
  {
    size = size + BYTE_BOUNDARY - (size % BYTE_BOUNDARY);
  }

  //critical zone entry
  pthread_mutex_lock(&(head_list->lock));

  header * found_block;
  while(1)
  {
    //gets first large enough free block
    found_block = find_free_space(size);

    if(found_block)
    {
      //shrinks free block and returns used space header ptr
      void *new_block = shrink_free(found_block, size);
      //leaving critical zone
      pthread_mutex_unlock(&(head_list->lock));

      //return pointer to space after header
      return (void *)((char *)(new_block) + sizeof(header));
    }
    //not enough space in any block
    else
    {
      //expands the size of heap
      if(!expand_heap())
      {
        //leaving critical area
        pthread_mutex_unlock(&(head_list->lock));
        //returns NULL on failure
        return NULL;
      }
    }
  }
}

/**********************************************************
  Frees an allocated block of memory located at ptr after its header
  Returns 0 on successfull freeing of the block
  ERROR: 1 if not freed 
**********************************************************/
unsigned int myfree(void *ptr) {
  //check if pointer within valid range
  if(ptr < heap_head || ptr > (void *)((char *)(heap_head) + heap_size - 8))
  {
    return 1;
  }

  //checks if valid header exists
  header *old_head = (header *)(ptr) - 1;

  //checks against test pointer
  if(old_head->next != used_head)
  {
    return 1;
  }

  //enters critical zone
  pthread_mutex_lock(&(head_list->lock));

  //adds free space back in to free list
  insert_free_space(old_head);

  //exits critical zone
  pthread_mutex_unlock(&(head_list->lock));

  //return success
  return 0;
}

/***************************************************************
***           LIST OPERATIONS
***************************************************************/

/**********************************************************
  Checks the given node in list against its next node,
  and combines the two into if the touch each other
**********************************************************/
void coalescing(header *parent)
{
  if((header *)((char *)(parent) + parent->size + sizeof(header)) == parent->next)
  {
    parent->size += parent->next->size + sizeof(header);
    parent->next = parent->next->next;
  }
}

/**********************************************************
  Turns given pointer into free space on list,
  adds node in sorted spot by address
**********************************************************/
void insert_free_space(header *old_space)
{
  //when added space comes before current head of list
  if(old_space < head_list->head)
  {
    //becomes front of list
    old_space->next = head_list->head;
    head_list->head = old_space;

    //coalesces only front 2
    coalescing(head_list->head);
  }
  //when added space comes after current head of list
  else
  {
    //finds correct spot for new node to be placed
    header *temp_head;
    for(temp_head = head_list->head; temp_head->next != NULL && temp_head->next < old_space; temp_head = temp_head->next) {  }

    //places it in correct place
    if(temp_head->next)
    {
      old_space->next = temp_head->next;
    }

    temp_head->next = old_space;

    //coalesces only needed spots
    coalescing(temp_head->next);
    coalescing(temp_head);
  }
}

/**********************************************************
  Finds the first available node with enough free space as
  requested by size
  Returns pointer to node with free enough free space
  ERROR: NULL if no space found
**********************************************************/
header *find_free_space(unsigned int size)
{
  header *temp_head;
  for (temp_head = head_list->head; temp_head != NULL; temp_head = temp_head->next)
  {
    if(temp_head->size >= size + sizeof(header))
    {
      return temp_head;
    }
  }

  return NULL;
}

/**********************************************************
  Shrinks the given node by a size of given size
  Returns the original pointer, pointing to header of sized space
  ERROR: No errors should arrise because only called
         if correct space exists
**********************************************************/
void *shrink_free(header *free_space, unsigned int size)
{
  header *new_block;

  //if free space is head of list
  if(head_list->head == free_space)
  {
    new_block = head_list->head;

    //adjusts current head to new location
    head_list->head = (header *)((char *)(head_list->head) + size + sizeof(header));
    head_list->head->size = new_block->size - size - sizeof(header);
    head_list->head->next = new_block->next;

    //adjusts soon-to-be used block
    new_block->size = size;
    new_block->next = used_head;
  }
  //free space is not head of list
  else
  {
    //finds parent of node
    header *temp_head;
    for(temp_head = head_list->head; temp_head->next != free_space; temp_head = temp_head->next) { }

    //adjusts parents next position
    new_block = temp_head->next;

    temp_head->next = (header *)((char *)(temp_head->next) + size + sizeof(header));
    temp_head->next->size = new_block->size - size - sizeof(header);
    temp_head->next->next = new_block->next;

    new_block->size = size;
    new_block->next = used_head;
  }

  //returns
  return new_block;
}

/**********************************************************
  Expands the heap by a factor of the set PAGE_SIZE,
  and adds the extension to the last element in the free list
  Returns amount of expanded size on success
  ERROR: 0 is unable to increase heap size
**********************************************************/
int expand_heap()
{
  //amount of size to increase by
  unsigned int size = PAGE_SIZE * expansion_counter++;
  //error checking
  if(sbrk(size) == (void *)-1)
  {
    //fails
    return 0;
  }

  //adds new space to size of last element in list
  header *temp_head = head_list->head;
  while(temp_head->next != NULL)
  {
    temp_head = temp_head->next;
  }

  temp_head->size += size;
  heap_size += size;

  //returns added size on success
  return size;
}

