/*  This file is part of Jellyfish.

    Jellyfish is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Jellyfish is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Jellyfish.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __JELLYFISH_CONCURRENT_QUEUES_HPP__
#define __JELLYFISH_CONCURRENT_QUEUES_HPP__

#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <new>
#include <stdio.h>
#include <assert.h>

#include <jellyfish/atomic_gcc.hpp>
#include <jellyfish/dbg.hpp>
#include <jellyfish/divisor.hpp>

/***
 * Circular buffer of fixed size with thread safe enqueue and dequeue
 * operation to make it behave like a FIFO. Elements are enqueued at
 * the head and dequeued at the tail. Never more than n elements
 * should be enqueued if the size is n+1. There is no check for this.
 *
 * It is possible for the tail pointer to go past an element (i.e. it
 * has been "dequeued"), but the thread is slow to zero the pointer
 * (i.e. to claim the element). It is then possible for the head
 * pointer to point to this not yet claimed element. The enqueue()
 * method blindly skip over such an element. Hence, it is possible
 * that the same element will be dequeued again before it is
 * claimed. Or, it will be claimed after being skipped and another
 * thread will dequeue what looks like an empty element. The outer
 * loop of dequeue() handles this situation.
 */

template<class Val>
class concurrent_queue {
  Val                   **queue;
  const unsigned int      size;
  unsigned int volatile   head;
  unsigned int volatile   tail;
  bool volatile           closed;
  divisor64               size_div;
  
public:
  concurrent_queue(unsigned int _size) : 
    size(20 *_size), head(0), tail(0), closed(false),
    size_div(size) 
  { 
    queue = new Val *[size];
    memset(queue, 0, sizeof(Val *) * size);
  }
  ~concurrent_queue() { delete [] queue; }

  void enqueue(Val *v);
  Val *dequeue();
  bool is_closed() { return closed; }
  void close() { closed = true; __sync_synchronize(); }
  bool has_space() { return head != tail; }
  bool is_low() { 
    unsigned int ctail = tail;
    __sync_synchronize();
    unsigned int chead = head;
    int len = chead - ctail;
    if(len < 0)
      len += size;
    return (unsigned int)(4*len) <= size;
  }
};

template<class Val>
void concurrent_queue<Val>::enqueue(Val *v) {
  int          done = 0;
  unsigned int chead;

  chead = head;
  do {
    uint64_t q, r;
    size_div.division(chead + 1, q, r);
    //    unsigned int nhead = (chead + 1) % size;
    unsigned int nhead = r;

    done = (atomic::gcc::cas(&queue[chead], (Val*)0, v) == (Val*)0);
    chead = atomic::gcc::cas(&head, chead, nhead);
  } while(!done);
}

template<class Val>
Val *concurrent_queue<Val>::dequeue() {
  bool done = false;
  Val *res;
  unsigned int ctail, ntail;

  ctail = tail;
  //  __sync_synchronize();
  do {
    bool dequeued = false;
    do {
      //    if(ctail == head)
      //      return NULL;

      // Complicated way to do ctail == head. Is it necessary? Or is
      // the memory barrier above sufficient? Or even necessary?
      if(atomic::gcc::cas(&head, ctail, ctail) == ctail)
        return NULL;
      //      ntail    = (ctail + 1) % size;
      uint64_t q, r;
      size_div.division(ctail + 1, q, r);
      ntail = r;
      ntail    = atomic::gcc::cas(&tail, ctail, ntail);
      dequeued = ntail == ctail;
      ctail    = ntail;
    } while(!dequeued);

    // Claim dequeued slot.  We may have dequeued an element which is
    // empty or that another thread also has dequeued but not yet
    // claimed. This can happen if a thread is slow to claim (set
    // pointer to 0) and the enqueue method has queued elements past
    // this one.
    res = queue[ctail];
    if(res)
      done = atomic::gcc::cas(&queue[ctail], res, (Val*)0) == res;
  } while(!done);

  return res;
}
  
#endif
