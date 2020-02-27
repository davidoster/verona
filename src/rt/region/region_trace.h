// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "../object/object.h"
#include "region_arena.h"
#include "region_base.h"

namespace verona::rt
{
  using namespace snmalloc;

  /**
   * Please see region.h for the full documentation.
   *
   * This is a concrete implementation of a region, specifically one with a
   * tracing (or mark-and-sweep) garbage collector. This class inherits from
   * RegionBase, but it cannot call any of the static methods in Region.
   *
   * In a trace region, all objects have a `next` pointer to another object.
   * This forms a circular linked list (or a "ring") of objects, not to be
   * mistaken for the object graph.
   *                                 |
   *                                 v
   *                         iso or root object
   *                          ^            \
   *                        /               v
   *                    object_n         RegionTrace
   *                      |                object
   *                     ...                 |
   *                       \                 v
   *                        v             object_1
   *                         other __ ... ___/
   *                        objects
   *
   * If the Iso object is trivial (ie. it has no finaliser, no destructor and no
   * subregions), then all the objects in the ring are also trivial. Conversely,
   * if the Iso object is non-trivial, then all of the objects in the ring are
   * also non-trivial.
   *
   * The other objects are placed in a second ring, referenced by the
   * `next_not_root` and `last_not_root` pointers.
   *
   * Note that we use the "last" pointer to ensure constant-time merging of two
   * rings. We avoid a "last" pointer for the primary ring, since the iso
   * object is the last object, and we always have a pointer to it.
   **/
  class RegionTrace : public RegionBase
  {
    friend class Freeze;
    friend class Region;

  private:
    enum RingKind
    {
      TrivialRing,
      NonTrivialRing,
    };

    // Circular linked list ("secondary ring") for trivial objects if the root
    // is trivial, or vice versa.
    Object* next_not_root;
    Object* last_not_root;

    // Memory usage in the region.
    size_t current_memory_used = 0;

    // Compact representation of previous memory used as a sizeclass.
    snmalloc::sizeclass_t previous_memory_used = 0;

    explicit RegionTrace(Object* o) : next_not_root(this), last_not_root(this)
    {
      set_descriptor(desc());
      init_next(o);
    }

    static const Descriptor* desc()
    {
      static constexpr Descriptor desc = {
        sizeof(RegionTrace), nullptr, nullptr, nullptr};

      return &desc;
    }

  public:
    inline static RegionTrace* get(Object* o)
    {
      assert(o->debug_is_iso());
      assert(is_trace_region(o->get_region()));
      return (RegionTrace*)o->get_region();
    }

    inline static bool is_trace_region(Object* o)
    {
      return o->is_type(desc());
    }

    /**
     * Creates a new trace region by allocating Object `o` of type `desc`. The
     * object is initialised as the Iso object for that region, and points to a
     * newly created Region metadata object. Returns a pointer to `o`.
     *
     * The default template parameter `size = 0` is to avoid writing two
     * definitions which differ only in one line. This overload works because
     * every object must contain a descriptor, so 0 is not a valid size.
     **/
    template<size_t size = 0>
    static Object* create(Alloc* alloc, const Descriptor* desc)
    {
      Object* o = nullptr;
      if constexpr (size == 0)
        o = (Object*)alloc->alloc(desc->size);
      else
        o = (Object*)alloc->alloc<size>();
      assert(Object::debug_is_aligned(o));

      void* p = alloc->alloc<sizeof(RegionTrace)>();
      RegionTrace* reg = new (p) RegionTrace(o);
      reg->use_memory(desc->size);

      o->set_descriptor(desc);
      o->init_iso();
      o->set_region(reg);

      return o;
    }

    /**
     * Allocates an object `o` of type `desc` in the region represented by the
     * Iso object `in`, and adds it to the appropriate ring. Returns a pointer
     * to `o`.
     *
     * The default template parameter `size = 0` is to avoid writing two
     * definitions which differ only in one line. This overload works because
     * every object must contain a descriptor, so 0 is not a valid size.
     **/
    template<size_t size = 0>
    static Object* alloc(Alloc* alloc, Object* in, const Descriptor* desc)
    {
      RegionTrace* reg = get(in);

      Object* o = nullptr;
      if constexpr (size == 0)
        o = (Object*)alloc->alloc(desc->size);
      else
        o = (Object*)alloc->alloc<size>();
      assert(Object::debug_is_aligned(o));
      o->set_descriptor(desc);

      // Add to the ring.
      reg->append(o);

      // GC heuristics.
      reg->use_memory(desc->size);

      return o;
    }

    /**
     * Insert the Object `o` into the RememberedSet of `into`'s region.
     *
     * If ownership of a reference count is being transfered to the region,
     * pass the template argument `transfer = YesTransfer`.
     **/
    template<TransferOwnership transfer = NoTransfer>
    static void insert(Alloc* alloc, Object* into, Object* o)
    {
      assert(o->debug_is_immutable() || o->debug_is_cown());
      RegionTrace* reg = get(into);

      Object::RegionMD c;
      o = o->root_and_class(c);
      reg->RememberedSet::insert<transfer>(alloc, o);
    }

    /**
     * Merges `o`'s region into `into`'s region. Both regions must be separate
     * and be the same kind of region, e.g. two trace regions.
     *
     * TODO(region): how to handle merging different types of regions?
     **/
    static void merge(Alloc* alloc, Object* into, Object* o)
    {
      assert(o->debug_is_iso());
      RegionTrace* reg = get(into);
      RegionBase* other = o->get_region();
      assert(reg != other);

      if (is_trace_region(other))
        reg->merge_internal(o, (RegionTrace*)other);
      else
        assert(0);

      // Merge the ExternalReferenceTable and RememberedSet.
      reg->ExternalReferenceTable::merge(alloc, other);
      reg->RememberedSet::merge(alloc, other);

      // Now we can deallocate the other region's metadata object.
      other->dealloc(alloc);
    }

    /**
     * Swap the Iso (root) Object of a region, `prev`, with another Object
     * within that region, `next`.
     **/
    static void swap_root(Object* prev, Object* next)
    {
      assert(prev != next);
      assert(prev->debug_is_iso());
      assert(next->debug_is_mutable());
      assert(prev->get_region() != next);

      RegionTrace* reg = get(prev);
      reg->swap_root_internal(prev, next);
    }

    /**
     * Run a garbage collection on the region represented by the Object `o`.
     * Only `o`'s region will be GC'd; we ignore pointers to Immutables and
     * other regions.
     **/
    static void gc(Alloc* alloc, Object* o)
    {
      Systematic::cout() << "Region GC called for: " << o << std::endl;
      assert(o->debug_is_iso());
      assert(is_trace_region(o->get_region()));

      RegionTrace* reg = get(o);
      ObjectStack f(alloc);
      ObjectStack collect(alloc);
      size_t marked = 0;

      reg->mark(alloc, o, f, marked);
      reg->sweep(alloc, o, f, collect, marked);

      // `collect` contains all the iso objects to unreachable subregions.
      // Since they are unreachable, we can just release them.
      while (!collect.empty())
      {
        o = collect.pop();
        assert(o->debug_is_iso());
        Systematic::cout() << "Region GC: releasing unreachable subregion: "
                           << o << std::endl;

        // Note that we need to dispatch because `r` is a different region
        // metadata object.
        RegionBase* r = o->get_region();
        assert(r != reg);

        // Unfortunately, we can't use Region::release_internal because of a
        // circular dependency between header files.
        if (RegionTrace::is_trace_region(r))
          ((RegionTrace*)r)->release_internal(alloc, o, f, collect);
        else if (RegionArena::is_arena_region(r))
          ((RegionArena*)r)->release_internal(alloc, o, f, collect);
        else
          abort();
      }
    }

  private:
    inline void append(Object* hd)
    {
      append(hd, hd);
    }

    /**
     * Inserts the object `hd` into the appropriate ring, right after the
     * region metadata object. `tl` is used for merging two rings; if there is
     * only one ring, then hd == tl.
     **/
    void append(Object* hd, Object* tl)
    {
      Object* p = get_next();

      if (hd->is_trivial() == p->is_trivial())
      {
        tl->init_next(p);
        set_next(hd);
      }
      else
      {
        tl->init_next(next_not_root);
        next_not_root = hd;

        if (last_not_root == this)
          last_not_root = tl;
      }
    }

    void merge_internal(Object* o, RegionTrace* other)
    {
      assert(o->get_region() == other);
      Object* head;

      // Merge the primary ring.
      head = other->get_next();
      if (head != other)
        append(head, o);

      // Merge the secondary ring.
      head = other->next_not_root;
      if (head != other)
        append(head, other->last_not_root);

      // Update memory usage.
      current_memory_used += other->current_memory_used;

      previous_memory_used = size_to_sizeclass(
        sizeclass_to_size(other->previous_memory_used) +
        sizeclass_to_size(other->previous_memory_used));
    }

    void swap_root_internal(Object* oroot, Object* nroot)
    {
      assert(debug_is_in_region(nroot));

      // Swap the rings if necessary.
      if (oroot->is_trivial() != nroot->is_trivial())
      {
        assert(last_not_root->get_next() == this);

        Object* t = get_next();
        set_next(next_not_root);
        next_not_root = t;

        t = last_not_root;
        last_not_root = oroot;
        oroot->init_next(this);
        oroot = t;
      }

      // We can end up with oroot == nroot if the rings were swapped.
      if (oroot != nroot)
      {
        // We cannot end up with oroot == this, because a region metadata
        // object cannot be a root.
        assert(oroot != this);
        assert(oroot->get_next_any_mark() == this);
        assert(nroot->get_next() != this);

        Object* x = get_next();
        Object* y = nroot->get_next();

        oroot->init_next(x);
        set_next(y);
      }

      nroot->init_iso();
      nroot->set_region(this);
    }

    /**
     * Scan through the region and mark all objects reachable from the iso
     * object `o`. We don't follow pointers to subregions.
     **/
    void mark(Alloc* alloc, Object* o, ObjectStack& dfs, size_t& marked)
    {
      o->trace(dfs);
      while (!dfs.empty())
      {
        Object* p = dfs.pop();
        switch (p->get_class())
        {
          case Object::ISO:
          case Object::MARKED:
            break;

          case Object::UNMARKED:
            p->mark();
            p->trace(dfs);
            break;

          case Object::SCC_PTR:
            p = p->immutable();
            RememberedSet::mark(alloc, p, marked);
            break;

          case Object::RC:
          case Object::COWN:
            RememberedSet::mark(alloc, p, marked);
            break;

          default:
            assert(0);
        }
      }
    }

    enum class SweepAll
    {
      Yes,
      No
    };

    /**
     * Sweep and deallocate all unmarked objects in the region. If we find an
     * unmarked object that points to a subregion, we add it to `collect` so we
     * can release it later.
     *
     * If sweep_all is Yes, it is assumed the entire region is being released
     * and the Iso object is collected as well.
     **/
    template<SweepAll sweep_all = SweepAll::No>
    void sweep(
      Alloc* alloc,
      Object* o,
      ObjectStack& f,
      ObjectStack& collect,
      size_t marked)
    {
      current_memory_used = 0;

      RingKind primary_ring = o->is_trivial() ? TrivialRing : NonTrivialRing;

      // We sweep the non-trivial ring first, as finalisers in there could refer
      // to other objects. The ISO object o could be deallocated by either of
      // these two lines.
      sweep_ring<NonTrivialRing, sweep_all>(alloc, o, primary_ring, f, collect);
      sweep_ring<TrivialRing, sweep_all>(alloc, o, primary_ring, f, collect);

      hash_set->sweep_set(alloc, marked);
      previous_memory_used = size_to_sizeclass(current_memory_used);
    }

    /**
     * Garbage Collect an object. If the object is trivial, then it is
     * deallocated immediately. Otherwise it is added to the `gc` linked list.
     */
    template<RingKind ring>
    void sweep_object(Alloc* alloc, Object* p, Object** gc)
    {
      assert(
        p->get_class() == Object::ISO || p->get_class() == Object::UNMARKED);
      if constexpr (ring == TrivialRing)
      {
        UNUSED(gc);
        assert(p->is_trivial());

        // p is about to be collected; remove the entry for it in
        // the ExternalRefTable.
        if (p->has_ext_ref())
          ExternalReferenceTable::erase(p);

        p->dealloc(alloc);
      }
      else
      {
        UNUSED(alloc);

        assert(!p->is_trivial());
        p->finalise();

        // We can't deallocate the object yet, as other objects' finalisers may
        // look at it. We build up a linked list of all objects to delete, we'll
        // deallocate them after sweeping through the entire ring.
        p->init_next(*gc);
        *gc = p;
      }
    }

    template<RingKind ring, SweepAll sweep_all>
    void sweep_ring(
      Alloc* alloc,
      Object* o,
      RingKind primary_ring,
      ObjectStack& f,
      ObjectStack& collect)
    {
      Object* prev = this;
      Object* p = ring == primary_ring ? get_next() : next_not_root;
      Object* gc = nullptr;

      // Note: we don't use the iterator because we need to remove and
      // deallocate objects from the rings.
      while (p != this)
      {
        switch (p->get_class())
        {
          case Object::ISO:
          {
            // An iso is always the root, and the last thing in the ring.
            assert(p->get_next_any_mark() == this);
            assert(p->get_region() == this);

            // The ISO is considered marked, unless we're releasing the
            // entire region anyway.
            if constexpr (sweep_all == SweepAll::Yes)
            {
              sweep_object<ring>(alloc, p, &gc);
            }
            else
            {
              use_memory(p->size());
            }

            p = this;
            break;
          }

          case Object::MARKED:
          {
            assert(sweep_all == SweepAll::No);
            use_memory(p->size());
            p->unmark();
            prev = p;
            p = p->get_next();
            break;
          }

          case Object::UNMARKED:
          {
            Object* q = p->get_next();
            sweep_object<ring>(alloc, p, &gc);

            if (ring != primary_ring && prev == this)
              next_not_root = q;
            else
              prev->set_next(q);

            if (ring != primary_ring && last_not_root == p)
              last_not_root = prev;

            p = q;
            break;
          }

          default:
            assert(0);
        }
      }

      // We need to collect all sub-regions and then deallocate the objects.
      // Unfortunately we can't do this as a single pass, as find_iso_fields
      // looks at the referenced object's header to see if it points to the same
      // region or to a different one.
      if constexpr (ring == NonTrivialRing)
      {
        p = gc;
        while (p != nullptr)
        {
          p->find_iso_fields(o, f, collect);
          p = p->get_next();
        }

        p = gc;
        while (p != nullptr)
        {
          Object* q = p->get_next();
          p->destructor();
          p->dealloc(alloc);
          p = q;
        }
      }
      else
      {
        UNUSED(o);
        UNUSED(f);
        UNUSED(collect);
      }
    }

    /**
     * Release and deallocate all objects within the region represented by the
     * Iso Object `o`.
     *
     * Note: this does not release subregions. Use Region::release instead.
     **/
    void release_internal(
      Alloc* alloc, Object* o, ObjectStack& f, ObjectStack& collect)
    {
      assert(o->debug_is_iso());

      Systematic::cout() << "Region release: trace region: " << o << std::endl;

      // Sweep everything, including the entrypoint.
      sweep<SweepAll::Yes>(alloc, o, f, collect, 0);

      dealloc(alloc);
    }

    void use_memory(size_t size)
    {
      current_memory_used += size;
    }

  public:
    template<IteratorType type = AllObjects>
    class iterator
    {
      friend class RegionTrace;

      static_assert(
        type == Trivial || type == NonTrivial || type == AllObjects);

      iterator(RegionTrace* r) : reg(r)
      {
        Object* q = r->get_next();
        if constexpr (type == Trivial)
          ptr = q->is_trivial() ? q : r->next_not_root;
        else if constexpr (type == NonTrivial)
          ptr = !q->is_trivial() ? q : r->next_not_root;
        else
          ptr = q;

        // If the next object is the region metadata object, then there was
        // nothing to iterate over.
        if (ptr == r)
          ptr = nullptr;
      }

      iterator(RegionTrace* r, Object* p) : reg(r), ptr(p) {}

    public:
      iterator operator++()
      {
        Object* q = ptr->get_next_any_mark();
        if (q != reg)
        {
          ptr = q;
          return *this;
        }

        if constexpr (type == AllObjects)
        {
          if (ptr != reg->last_not_root && reg->next_not_root != reg)
          {
            // We finished the primary ring and there's a secondary ring to
            // switch to.
            assert(ptr->debug_is_iso());
            ptr = reg->next_not_root;
          }
          else
          {
            // We finished the secondary ring, so we're done.
            ptr = nullptr;
          }
        }
        else
        {
          // We finished a ring and don't care about the other ring.
          ptr = nullptr;
        }
        return *this;
      }

      inline bool operator!=(const iterator& other) const
      {
        assert(reg == other.reg);
        return ptr != other.ptr;
      }

      inline Object* operator*() const
      {
        return ptr;
      }

    private:
      RegionTrace* reg;
      Object* ptr;
    };

    template<IteratorType type = AllObjects>
    inline iterator<type> begin()
    {
      return {this};
    }

    template<IteratorType type = AllObjects>
    inline iterator<type> end()
    {
      return {this, nullptr};
    }

  private:
    bool debug_is_in_region(Object* o)
    {
      for (auto p : *this)
      {
        if (p == o)
          return true;
      }
      return false;
    }
  };
} // namespace verona::rt
