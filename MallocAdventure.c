#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "myMalloc.h"
#include "printing.h"

#ifdef TEST_ASSERT
  inline static void assert(int e) {
    if (!e) {
      const char * msg = "Assertion Failed!\n";
      write(2, msg, strlen(msg));
      exit(1);
    }
  }
#else
  #include <assert.h>
#endif

static pthread_mutex_t mutex;

eader freelistSentinels[N_LISTS];

header * lastFencePost;

void * base;

header * osChunkList [MAX_OS_CHUNKS];
size_t numOsChunks = 0;

static void init (void) __attribute__ ((constructor));

// Helper functions for manipulating pointers to headers
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off);
static inline header * get_left_header(header * h);
static inline header * ptr_to_header(void * p);

// Helper functions for allocating more memory from the OS
static inline void initialize_fencepost(header * fp, size_t object_left_size);
static inline void insert_os_chunk(header * hdr);
static inline void insert_fenceposts(void * raw_mem, size_t size);
static header * allocate_chunk(size_t size);

// Helper functions for freeing a block
static inline void deallocate_object(void * p);

// Helper functions for allocating a block
static inline header * allocate_object(size_t raw_size);

// Helper functions for verifying that the data structures are structurally 
// valid
static inline header * detect_cycles();
static inline header * verify_pointers();
static inline bool verify_freelist();
static inline header * verify_chunk(header * chunk);
static inline bool verify_tags();

//****************************************
static size_t roundUp8(size_t raw_size); 
static void insertFreeList(header* block);
static header* splitBlock(header* grabbed, size_t actualRequest);
static void removeHelper(header* target, header* sentinel);
static void removeFreeList(header* target);
static size_t getIndex(size_t size);
static header* getMem(size_t raw_size);
//****************************************

static void init();

static bool isMallocInitialized;

static inline header * get_header_from_offset(void * ptr, ptrdiff_t off) {
	return (header *)((char *) ptr + off);
}

header * get_right_header(header * h) {
	return get_header_from_offset(h, get_object_size(h));
}

inline static header * get_left_header(header * h) {
  return get_header_from_offset(h, -h->object_left_size);
}

inline static void initialize_fencepost(header * fp, size_t object_left_size) {
	set_object_state(fp,FENCEPOST);
	set_object_size(fp, ALLOC_HEADER_SIZE);
	fp->object_left_size = object_left_size;
}

inline static void insert_os_chunk(header * hdr) {
  if (numOsChunks < MAX_OS_CHUNKS) {
    osChunkList[numOsChunks++] = hdr;
  }
}

inline static void insert_fenceposts(void * raw_mem, size_t size) {
  // Convert to char * before performing operations
  char * mem = (char *) raw_mem;

  // Insert a fencepost at the left edge of the block
  header * leftFencePost = (header *) mem;
  initialize_fencepost(leftFencePost, ALLOC_HEADER_SIZE);

  // Insert a fencepost at the right edge of the block
  header * rightFencePost = get_header_from_offset(mem, size - ALLOC_HEADER_SIZE);
  initialize_fencepost(rightFencePost, size - 2 * ALLOC_HEADER_SIZE);
}

static header * allocate_chunk(size_t size) {
  void * mem = sbrk(size);
  
  insert_fenceposts(mem, size);
  header * hdr = (header *) ((char *)mem + ALLOC_HEADER_SIZE);
  set_object_state(hdr, UNALLOCATED);
  set_object_size(hdr, size - 2 * ALLOC_HEADER_SIZE);
  hdr->object_left_size = ALLOC_HEADER_SIZE;
  return hdr;
}

static inline header * allocate_object(size_t raw_size) {
  // TODO implement allocation

  //if the allocation is 0 bytes than we just return a null pointer
  if(raw_size == 0) {
      return NULL;
  }

  //Now we need to figure out if we use the header size or the users rounded up size
 
  //now we calculate the rounded up value of the users request
  size_t roundedRequest = roundUp8(raw_size); 
  //if the size of the header is greater than the rounded request+meta than we use that
  size_t actualRequest;
  if( (2 * sizeof(header *)) > roundedRequest) {
      actualRequest = 2 * sizeof(header *);
  } else {
      actualRequest = roundedRequest;
  }
  
  //Now we look for the smallest free block that is large enough to hold the rounded request size
  
  for(int i = 0; i < N_LISTS; i++) {
      header* sentinel = &freelistSentinels[i];

      //if the sentinel prev & next are pointing to itself then we have a list with nothing in it
      if(sentinel->next == sentinel && sentinel->prev == sentinel) {
          continue;
      }

      if(i == N_LISTS-1) {
          //searching for first node that is big enough
          header* currentNode = sentinel->next;
          while (currentNode != sentinel) {
               size_t totalSize = get_object_size(currentNode);
               
               if(totalSize - ALLOC_HEADER_SIZE >= actualRequest) {
                   
                   header* grabbed = currentNode;
 		    
                   //Now that is is removed we split the block
                   grabbed = splitBlock(grabbed, actualRequest);
                   return &grabbed->data[0];         
               }
               
               currentNode = currentNode->next;
          }
      }
      
      //grab from the first non empty list that satisfies the request
      else if( ((i+1)*8) >= actualRequest ) {
           //grab the first block in the list as they are all the same size
           header* grabbed = sentinel->next;

           //split the block if nessary
           grabbed = splitBlock(grabbed, actualRequest);
	   return &grabbed->data[0]; 
      }
  }

  return getMem(raw_size);
}

static inline header * ptr_to_header(void * p) {
  return (header *)((char *) p - ALLOC_HEADER_SIZE); //sizeof(header));
}

static inline void deallocate_object(void * p) {
  // TODO implement deallocation

  //feeing a null pointer is a no op
  if(p == NULL) {
      return;
  }  
  
  //Now we have a few cases to consider
  //1. Neither the left or the right block are free
  //   than we just insert into the appropiate list
  //2. The right is free and the left is not
  //   we coalese the right and the block given than insert into the free list
  //3. Opposite of 2 the left is free and the right is not
  //4. Both are free. Than we coalese the whole thing than insert intot the freelist
  header* head = ptr_to_header(p); 
  
  //checking for double free
  if(get_object_state(head) == UNALLOCATED) {
      printf("Double Free Detected\nAssertion Failed!\n"); 
      exit(1);
  }
  
  set_object_state(head, UNALLOCATED);
  header* leftBlock = get_left_header(head);
  header* rightBlock = get_right_header(head);

  enum state leftState;
  enum state rightState;
  leftState = get_object_state(leftBlock);
  rightState = get_object_state(rightBlock);  

  if(rightState != UNALLOCATED && leftState != UNALLOCATED) {
      set_object_state(head, UNALLOCATED);
      insertFreeList(head);
      return;
  } 
  else if(rightState == UNALLOCATED && leftState != UNALLOCATED) {
      size_t userBlockSize = get_object_size(head);
      size_t rightBlockSize = get_object_size(rightBlock);

      removeFreeList(rightBlock);
      
      //changing state of head to free
      set_object_state(head, UNALLOCATED);
      
      //setting size and inserting      
      size_t newSize = rightBlockSize + userBlockSize; 
      set_object_size(head, newSize);
      insertFreeList(head);

      //making sure the right next block has the correct data
      header* rightCoa = get_right_header(head);
      rightCoa->object_left_size = rightBlockSize + userBlockSize;

  }
  else if(rightState != UNALLOCATED && leftState == UNALLOCATED) {
      size_t leftSize = get_object_size(leftBlock);
      size_t userBlockSize = get_object_size(head);            

      int newSize = leftSize + userBlockSize;
      if(getIndex(leftSize) != getIndex(newSize)) {
	 removeFreeList(leftBlock);
         set_object_size(leftBlock, newSize);
         insertFreeList(leftBlock);

         //making sure the right next block has the correct data
         header* rightCoa = get_right_header(leftBlock);
         rightCoa->object_left_size = leftSize + userBlockSize;

         return;
      }
     
      set_object_size(leftBlock, newSize);
      //making sure the right next block has the correct data
      header* rightCoa = get_right_header(leftBlock);
      rightCoa->object_left_size = leftSize + userBlockSize;
  }
  else if(rightState == UNALLOCATED && leftState == UNALLOCATED) {
     size_t rightSize = get_object_size(rightBlock);
     size_t leftSize = get_object_size(leftBlock);
     size_t userBlockSize = get_object_size(head);
     
     removeFreeList(rightBlock);   
     
     size_t newSize = rightSize + leftSize + userBlockSize;

     if(getIndex(leftBlock) != getIndex(newSize)) {
	 removeFreeList(leftBlock);
         set_object_size(leftBlock, newSize);
         insertFreeList(leftBlock);

         //making sure the right next block has the correct data
         header* rightCoa = get_right_header(leftBlock);
         rightCoa->object_left_size = rightSize + userBlockSize + leftSize;

         return;
     }

     set_object_size(leftBlock, newSize); 
     //making sure the right next block has the correct data
     header* rightCoa = get_right_header(leftBlock);
     rightCoa->object_left_size = rightSize + userBlockSize + leftSize;    
  }
}

static inline header * detect_cycles() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * slow = freelist->next, * fast = freelist->next->next; 
         fast != freelist; 
         slow = slow->next, fast = fast->next->next) {
      if (slow == fast) {
        return slow;
      }
    }
  }
  return NULL;
}

static inline header * verify_pointers() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * cur = freelist->next; cur != freelist; cur = cur->next) {
      if (cur->next->prev != cur || cur->prev->next != cur) {
        return cur;
      }
    }
  }
  return NULL;
}

static inline bool verify_freelist() {
  header * cycle = detect_cycles();
  if (cycle != NULL) {
    fprintf(stderr, "Cycle Detected\n");
    print_sublist(print_object, cycle->next, cycle);
    return false;
  }

  header * invalid = verify_pointers();
  if (invalid != NULL) {
    fprintf(stderr, "Invalid pointers\n");
    print_object(invalid);
    return false;
  }

  return true;
}

static inline header * verify_chunk(header * chunk) {
	if (get_object_state(chunk) != FENCEPOST) {
		fprintf(stderr, "Invalid fencepost\n");
		print_object(chunk);
		return chunk;
	}
	
	for (; get_object_state(chunk) != FENCEPOST; chunk = get_right_header(chunk)) {
		if (get_object_size(chunk)  != get_right_header(chunk)->object_left_size) {
			fprintf(stderr, "Invalid sizes\n");
			print_object(chunk);
			return chunk;
		}
	}
	
	return NULL;
}

static inline bool verify_tags() {
  for (size_t i = 0; i < numOsChunks; i++) {
    header * invalid = verify_chunk(osChunkList[i]);
    if (invalid != NULL) {
      return invalid;
    }
  }

  return NULL;
}

static void init() {
  // Initialize mutex for thread safety
  pthread_mutex_init(&mutex, NULL);

#ifdef DEBUG
  // Manually set printf buffer so it won't call malloc when debugging the allocator
  setvbuf(stdout, NULL, _IONBF, 0);
#endif // DEBUG

  // Allocate the first chunk from the OS
  header * block = allocate_chunk(ARENA_SIZE);

  header * prevFencePost = get_header_from_offset(block, -ALLOC_HEADER_SIZE);
  insert_os_chunk(prevFencePost);

  lastFencePost = get_header_from_offset(block, get_object_size(block));

  // Set the base pointer to the beginning of the first fencepost in the first
  // chunk from the OS
  base = ((char *) block) - ALLOC_HEADER_SIZE; //sizeof(header);

  // Initialize freelist sentinels
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    freelist->next = freelist;
    freelist->prev = freelist;
  }

  // Insert first chunk into the free list
  header * freelist = &freelistSentinels[N_LISTS - 1];
  freelist->next = block;
  freelist->prev = block;
  block->next = freelist;
  block->prev = freelist;
}

/* 
 * External interface
 */
void * my_malloc(size_t size) {
  pthread_mutex_lock(&mutex);
  header * hdr = allocate_object(size); 
  pthread_mutex_unlock(&mutex);
  return hdr;
}

void * my_calloc(size_t nmemb, size_t size) {
  return memset(my_malloc(size * nmemb), 0, size * nmemb);
}

void * my_realloc(void * ptr, size_t size) {
  void * mem = my_malloc(size);
  memcpy(mem, ptr, size);
  my_free(ptr);
  return mem; 
}

void my_free(void * p) {
  pthread_mutex_lock(&mutex);
  deallocate_object(p);
  pthread_mutex_unlock(&mutex);
}

bool verify() {
  return verify_freelist() && verify_tags();
}

static size_t roundUp8(size_t raw_size) {
    //by modulusing by 8 we get the number that we need to add to raw size to get a multiple of 8
    //EX: 15 % 8 = 7 next 15 + (8-7) = 16 which is the number we want 
    int remainder  = raw_size % 8;
    
    //if the remainder is 0 we do not have to do anything
    if(remainder == 0) {
        return raw_size;
    }
    
    //now we return the raw_size + remainder to get the rounded up value 
    return raw_size + (8 - remainder);
}

static void insertFreeList(header* block) {
    //first we need to find where the block goes
    //get the total size of the block first 
    size_t totalSize = get_object_size(block);
    size_t userSize = totalSize - ALLOC_HEADER_SIZE;
    
    //now that we have the free space for the user we can insert it 
    for(int i = 0; i < N_LISTS; i++) {
        header* sentinel1 = &freelistSentinels[i];
     
        //if we hit the last free list than we just insert
        if(i == N_LISTS -1) {
            if(sentinel1->next == sentinel1 && sentinel1->prev == sentinel1) {
                sentinel1->next = block;
                sentinel1->prev = block;
                block->prev = sentinel1;
                block->next = sentinel1;
                return;
            }

            header* temp = sentinel1->next;
            sentinel1->next = block;
            block->prev = sentinel1;
            block->next = temp;
            temp->prev = block;
            return;        
        }

        if(((userSize/8) - 1) == i) {
            if(sentinel1->next == sentinel1 && sentinel1->prev == sentinel1) {
                sentinel1->next = block;
                sentinel1->prev = block;
                block->prev = sentinel1;
                block->next = sentinel1; 
		return;              
            }

            header* temp = sentinel1->next;
            sentinel1->next = block;
            block->prev = sentinel1;
            block->next = temp;
            temp->prev = block;
            return;
        }
    }

}
 
static header* splitBlock(header* grabbed, size_t actualRequest) {
    if(get_object_size(grabbed) - actualRequest -  ALLOC_HEADER_SIZE < 32) {
        //set the grabbed to allocated
        enum state toSet;
        toSet = ALLOCATED;
        set_object_state(grabbed, toSet);
        removeFreeList(grabbed);
        return grabbed;
    } else {
        //if we have 32 bytes minimum as extra than we can split the memory
        //Make sure that the right side of the memory for the people
        size_t totalSize = get_object_size(grabbed);
        size_t newSize = totalSize - actualRequest - ALLOC_HEADER_SIZE;
        header* leftover = grabbed;
        set_object_size(leftover, newSize);

        //setting up other section of memory
        grabbed = get_right_header(leftover);
        enum state toSet;
        toSet = ALLOCATED;
        set_object_state(grabbed, toSet);
        set_object_size(grabbed, actualRequest + ALLOC_HEADER_SIZE);
        grabbed->object_left_size = totalSize - actualRequest -  ALLOC_HEADER_SIZE;
        
        
        //now we need to put the free part of the list in the appropiate free list
        if(getIndex(totalSize) != getIndex(newSize)) {
            removeFreeList(leftover); 
            insertFreeList(leftover);
        }

        //setting the right block next to the split block correctly
        header* rightSplit = get_right_header(grabbed);
        rightSplit->object_left_size = get_object_size(grabbed);

        return grabbed;
    }       
}

static void removeFreeList(header* target) {
    target->prev->next = target->next;
    target->next->prev = target->prev;
    target->prev = NULL;
    target->next = NULL;
}

static void removeHelper(header* target, header* sentinel) {
        header* currentNode = sentinel->next;
        header* previous = sentinel;

	while(currentNode != sentinel) {
	    if(currentNode == target) {
		//case for last node in list
		if(currentNode->next == sentinel) {
		    previous->next = sentinel;
                    sentinel->prev = previous;
		} else {
		    //general case: its in the middle
	 	    header* temp = currentNode->next;

                    previous->next = temp;
                    temp->prev = previous;
                }
	    }

	    previous = currentNode; 	    
	    currentNode = currentNode->next;
	}
}

static size_t getIndex(size_t size) {
   if((size/8)-1 >= N_LISTS-1) {
      return N_LISTS-1;
   } else {
      return (size/8)-1;
   }

}

static header* getMem(size_t raw_size) {
    header* freeSection = allocate_chunk(ARENA_SIZE);     
    header* newFencePost = get_left_header(freeSection);
    header* lastSection = get_left_header(lastFencePost);
    
   if(newFencePost == get_right_header(lastFencePost)) {
       if(get_object_state(lastSection) == UNALLOCATED) {
           size_t oldSize = get_object_size(lastSection);
           size_t newSize = (2 * ALLOC_HEADER_SIZE) + get_object_size(freeSection) + get_object_size(lastSection);
           set_object_size(lastSection, newSize);
           lastFencePost = get_right_header(lastSection);
           lastFencePost->object_left_size = get_object_size(lastSection);
           
           //now we insert the block again to see if it changes places
           if(getIndex(oldSize) != getIndex(newSize)) {
                removeFreeList(lastSection);
		insertFreeList(lastSection);
           }
       } else {
           size_t newSize = (ALLOC_HEADER_SIZE * 2) + get_object_size(freeSection);
           header* newHead = get_left_header(newFencePost);
           set_object_state(newHead, UNALLOCATED);
           set_object_size(newHead, newSize);
           lastFencePost = get_right_header(newHead);
           insertFreeList(newHead);
       }     

   } else {
       insert_os_chunk(newFencePost); 
       insertFreeList(freeSection);
       lastFencePost = get_right_header(freeSection); 
   }

   return allocate_object(raw_size); 
}
