/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2021, Huawei Technologies Co. Ltd. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef LINUX_X86_64_SERVER_SLOWDEBUG_G1SEGMENTEDARRAY_HPP
#define LINUX_X86_64_SERVER_SLOWDEBUG_G1SEGMENTEDARRAY_HPP

#include "memory/allocation.hpp"
#include "utilities/lockFreeStack.hpp"


// A single buffer/arena containing _num_elems blocks of memory of _elem_size.
// SegmentedArrayBuffers can be linked together using a singly linked list.
template<MEMFLAGS flag>
class SegmentedArrayBuffer : public CHeapObj<flag> {
  const uint _elem_size;
  const uint _num_elems;

  SegmentedArrayBuffer* volatile _next;

  char* _buffer;  // Actual data.

  // Index into the next free block to allocate into. Full if equal (or larger)
  // to _num_elems (can be larger because we atomically increment this value and
  // check only afterwards if the allocation has been successful).
  uint volatile _next_allocate;

public:
  SegmentedArrayBuffer(uint elem_size, uint num_elems, SegmentedArrayBuffer* next);
  ~SegmentedArrayBuffer();

  SegmentedArrayBuffer* volatile* next_addr() { return &_next; }

  void* get_new_buffer_elem();

  uint num_elems() const { return _num_elems; }

  SegmentedArrayBuffer* next() const { return _next; }

  void set_next(SegmentedArrayBuffer* next) {
    assert(next != this, " loop condition");
    _next = next;
  }

  void reset(SegmentedArrayBuffer* next) {
    _next_allocate = 0;
    assert(next != this, " loop condition");
    set_next(next);
    memset((void*)_buffer, 0, (size_t)_num_elems * _elem_size);
  }

  uint elem_size() const { return _elem_size; }

  size_t mem_size() const { return sizeof(*this) + (size_t)_num_elems * _elem_size; }

  uint length() const { return _next_allocate; }

  char* start() const { return _buffer; }

  bool is_full() const { return _next_allocate >= _num_elems; }
};



// Set of (free) SegmentedArrayBuffers. The assumed usage is that allocation
// to it and removal of elements is strictly separate, but every action may be
// performed by multiple threads at the same time.
// Counts and memory usage are current on a best-effort basis if accessed concurrently.
template<MEMFLAGS flag>
class SegmentedArrayBufferList {
  static SegmentedArrayBuffer<flag>* volatile* next_ptr(SegmentedArrayBuffer<flag>& node) {
    return node.next_addr();
  }
  typedef LockFreeStack<SegmentedArrayBuffer<flag>, &SegmentedArrayBufferList::next_ptr> NodeStack;

  NodeStack _list;

  volatile size_t _num_buffers;
  volatile size_t _mem_size;

public:
  SegmentedArrayBufferList() : _list(), _num_buffers(0), _mem_size(0) { }
  ~SegmentedArrayBufferList() { free_all(); }

  void bulk_add(SegmentedArrayBuffer<flag>& first, SegmentedArrayBuffer<flag>& last, size_t num, size_t mem_size);
  void add(SegmentedArrayBuffer<flag>& elem) { _list.prepend(elem); }

  SegmentedArrayBuffer<flag>* get();
  SegmentedArrayBuffer<flag>* get_all(size_t& num_buffers, size_t& mem_size);

  // Give back all memory to the OS.
  void free_all();

  void print_on(outputStream* out, const char* prefix = "");

  size_t num_buffers() const { return Atomic::load(&_num_buffers); }
  size_t mem_size() const { return Atomic::load(&_mem_size); }
};



class G1SegmentedArrayAllocOptions {
protected:
  uint _elem_size;
  uint _initial_num_elems;
  // Defines a limit to the number of elements in the buffer
  uint _max_num_elems;
  uint _alignment;

  uint exponential_expand(uint prev_num_elems) {
    return clamp(prev_num_elems * 2, _initial_num_elems, _max_num_elems);
  }

public:
  static const uint BufferAlignment = 4;
  static const uint MinimumBufferSize = 8;
  static const uint MaximumBufferSize =  UINT_MAX / 2;

  G1SegmentedArrayAllocOptions(uint elem_size, uint initial_num_elems = MinimumBufferSize,
                               uint max_num_elems = MaximumBufferSize, uint alignment = BufferAlignment) :
    _elem_size(elem_size),
    _initial_num_elems(initial_num_elems),
    _max_num_elems(max_num_elems),
    _alignment(alignment) {
  }

  uint next_num_elems(uint prev_num_elems) {
    return _initial_num_elems;
  }

  uint elem_size () const {return _elem_size;}

  uint alignment() const { return _alignment; }
};



template <class Elem, MEMFLAGS flag>
class G1SegmentedArray {

protected:
  // G1CardSetAllocOptions provides parameters for allocation buffer
  // sizing and expansion.
  G1SegmentedArrayAllocOptions _alloc_options;

  volatile uint _num_available_nodes; // Number of nodes available in all buffers (allocated + free + pending + not yet used).
  volatile uint _num_allocated_nodes; // Number of total nodes allocated and in use.

private:
  SegmentedArrayBuffer<flag>* volatile _first;       // The (start of the) list of all buffers.
  SegmentedArrayBuffer<flag>* _last;                 // The last element of the list of all buffers.
  volatile uint _num_buffers;             // Number of assigned buffers to this allocator.
  volatile size_t _mem_size;              // Memory used by all buffers.

  SegmentedArrayBufferList<flag>* _free_buffer_list; // The global free buffer list to
  // preferentially get new buffers from.

private:
  SegmentedArrayBuffer<flag>* create_new_buffer(SegmentedArrayBuffer<flag>* const prev);

protected:
  uint elem_size() const;

public:
  G1SegmentedArray(const char* name,
                   const G1SegmentedArrayAllocOptions& buffer_options,
                   SegmentedArrayBufferList<flag>* free_buffer_list);
  ~G1SegmentedArray() {
    drop_all();
  }

  // Deallocate all buffers to the free buffer list and reset this allocator. Must
  // be called in a globally synchronized area.
  void drop_all();

  Elem* allocate();

  uint num_buffers() const;

  uint length();

  template<typename Visitor>
  void iterate_nodes(Visitor& v);
};


#endif //LINUX_X86_64_SERVER_SLOWDEBUG_G1SEGMENTEDARRAY_HPP
