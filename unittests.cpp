/* unittests.cpp
Unit testing for memory transactions
(C) 2013-2014 Niall Douglas http://www.nedproductions.biz/


Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#include "spinlock.hpp"
#include "timing.h"

#include <stdio.h>
#include <unordered_map>
#include <vector>

#ifndef BOOST_MEMORY_TRANSACTIONS_DISABLE_CATCH
#define CATCH_CONFIG_RUNNER
#include "catch.hpp"
#endif

#include <mutex>

namespace boost { namespace spinlock {
  /* \class concurrent_unordered_map
  \brief Provides an unordered_map which is thread safe and wait free to use and whose find, insert/emplace and erase functions are usually wait free.
   
  Notes:
  * Rehashing isn't implemented at all, so what you reserve for buckets at the beginning is what you keep. As the tables
  are tightly packed (16 bytes an entry) and linear, the performance hit from an excessive load factor is relatively low
  assuming that inserts and erases aren't constantly hitting the same cache lines. Finds don't modify any cache lines at all.

  * find, insert/emplace and erase all run completely wait free if in separate buckets which will be most
  of the time. When they hit the same bucket, all run completely wait free except under the following circumstances:
  
    1. If they are operating on the same key, in which case they will be serialised in a first come first served fashion.

    2. If there are insufficient empty slots in the table, a table resize is begun which will halt all new operations on that
    bucket and wait until existing operations exit before resizing the bucket, after which execution resumes.
  */
  template<class Key, class T, class Hash=std::hash<Key>, class Pred=std::equal_to<Key>, class Alloc=std::allocator<std::pair<const Key, T>>> class concurrent_unordered_map
  {
  public:
    typedef Key key_type;
    typedef T mapped_type;
    typedef std::pair<const key_type, mapped_type> value_type;
    typedef Hash hasher;
    typedef Pred key_equal;
    typedef Alloc allocator_type;
    
    typedef value_type& reference;
    typedef const value_type& const_reference;
    typedef value_type* pointer;
    typedef const value_type *const_pointer;
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;
  private:
    std::atomic<size_type> _size;
    mutable spinlock<bool> _rehash_lock; // will one day serialise rehashing
    hasher _hasher;
    key_equal _key_equal;
    struct item_type
    {
      spinlock<locked_ptr<value_type>> p;
      size_t hash;
      void set(value_type *ret, size_t _hash)
      {
        assert(is_lockable_locked(p)); // must be locked to maintain place
        p.set(ret);
        hash=_hash;
      }
      value_type *detach() BOOST_NOEXCEPT
      {
        spinlock<locked_ptr<value_type>> l(std::move(p)); // atomically detaches
        value_type *i=l.get();
        if(i)
          hash=0;
        return i;
      }
    };
    typedef typename allocator_type::template rebind<item_type>::other item_type_allocator_type;
    item_type_allocator_type _allocator;
    struct bucket_type // padded to 32 bytes each, so two per cache line
    {
    private:
      mutable std::atomic<unsigned> entered, exited; // tracks how many threads are using this bucket
      spinlock<bool> resize_lock; // halts new threads if we need to resize the bucket
      std::atomic<size_t> count;
      item_type *items;
      char __pad[32-2*sizeof(entered)-sizeof(resize_lock)-sizeof(count)-sizeof(items)];
    public:
      bucket_type() : entered(0), exited(0), count(0), items(nullptr) { }
      ~bucket_type() BOOST_NOEXCEPT
      {
        clear();
      }
      void clear(item_type_allocator_type &allocator) BOOST_NOEXCEPT
      {
        // Exclude all new threads for this bucket
        std::lock_guard<decltype(resize_lock)> g(resize_lock);
        // Wait until present users have exited
        while(entered!=exit)
          this_thread::yield();
        if(items)
        {
          item_type *i=items;
          size_t _count=count;
          for(size_t n=0; n<_count; n++, ++i)
          {
            i->destroy(allocator);
          }
          free(items);
          items=nullptr;
          count=0;
        }
      }
      friend struct read_lock;
      // Used to prevent resizes
      struct using_
      {
        bucket_type &b;
        using_(bucket_type *_b) : b(*_b)
        {
          b.++entered;
          // If resizing is currently happening, wait until it's done
          while(is_lockable_locked(b.resize_lock))
          {
            ++b.exited;
            b.resize_lock.lock();
            b.resize_lock.unlock();
            b.++entered;
          }
        }
        ~using_()
        {
          ++b.exited;
        }
      };
      // Must be called with read lock. Returns a LOCKED item if hash found
      item_type *find(item_type *start, size_t hash, size_t *empty==nullptr) BOOST_NOEXCEPT
      {
        item_type *ret=nullptr;
        if(items)
        {
          item_type *i=start ? start : items, *end=items+count;
          for(;;)
          {
            for(; i<end && i->hash!=hash; ++i)
            {
              if(empty && !i->p.get())
              {
                *empty=i-items;
                empty=nullptr;
              }
            }
            if(i==end)
              break;
            // Lock the item matching the hash
            i->p.lock();
            // Is the hash still matching and the item pointer valid?
            if(i->hash==hash && i->p.get())
            {
              ret=i;
              break;
            }
            i->p.unlock();
          }
        }
        return ret;
      }
      template<class P> size_t insert(item_type_allocator_type &allocator, P &&v, size_t hash, size_t hint=0)
      {
        size_t offset=(size_t)-1;
        do
        {
          size_t newsize;
          {
            using_ g(this); // prevent resizes
            if(items)
            {
              item_type *i=items+hint, *end=items+count;
              for(; i<end; i++)
              {
                if(!i->p.load() && i->p.try_lock())
                {
                  offset=i->items;
                  break;
                }
              }
            }
            newsize=count;
          }
          newsize+=newsize/2;
          resize(newsize);
        } while(offset==(size_t)-1);
        // Item slot is locked to prevent others taking it
        // resizes may happen at any time during item construction
        value_type *ret=nullptr;
        try
        {
          value_type *ret=allocator.allocate(1);
          allocator.construct(ret, std::forward<P>(v));
          using_ g(this); // prevent resizes
          item_type *i=items+offset;
          i->set(ret, hash);
          return offset;
        }
        catch(...)
        {
          using_ g(this); // prevent resizes
          item_type *i=items+offset;
          i->p.unlock();
          allocator.deallocate(ret, sizeof(value_type));
          throw;
        }
      }
      void remove(item_type_allocator_type &allocator, size_t offset)
      {
        value_type *v=nullptr;
        {
          using_ g(this);
          item_type *i=items+offset;
          i->p.lock();
          v=i->detach();
        }
        if(v)
        {
          allocator.destroy(v);
          allocator.deallocate(v, sizeof(value_type));          
        }
      }
      void resize(size_t newsize)
      {
        // Exclude all new threads for this bucket
        std::lock_guard<decltype(resize_lock)> g(resize_lock);
        // Wait until present users have exited
        while(entered!=exit)
          this_thread::yield();
        size_t oldbytes=count.load()*sizeof(item_type), newbytes=newsize*sizeof(item_type);
        item_type *n=(items) ? realloc(items, newbytes) : malloc(newbytes);
        if(!n) throw std::bad_alloc();
        if(newbytes>oldbytes)
          memset(n+count.load(), 0, newbytes-oldbytes);
        items=n;
        count.store(newsize);
      }
    };
    std::vector<bucket_type> _buckets;
    typename std::vector<bucket_type>::iterator _get_bucket(size_t k) BOOST_NOEXCEPT
    {
      size_type i=k % _buckets.size();
      return _buckets.begin()+i;
    }
    typename std::vector<bucket_type>::const_iterator _get_bucket(size_t k) const BOOST_NOEXCEPT
    {
      size_type i=k % _buckets.size();
      return _buckets.begin()+i;
    }
  public:
    class iterator : public std::iterator<std::forward_iterator_tag, value_type, difference_type, pointer, reference>
    {
      concurrent_unordered_map *_parent;
      typename std::vector<bucket_type>::iterator _itb;
      size_t _offset;
      friend class concurrent_unordered_map;
      iterator(concurrent_unordered_map *parent) : _parent(parent), _itb(parent->_buckets.begin()), _offset(0) { }
      iterator(concurrent_unordered_map *parent, std::nullptr_t) : _parent(parent), _itb(parent->_buckets.end()), _offset(0) { }
    public:
      iterator() : _parent(nullptr), _offset(0) { }
      iterator &operator++()
      {
        if(_itb==_parent->_buckets.end())
          return *this;
        bucket_type &b=*_itb;
        ++_offset;
        if(_offset>=b.count)
        {
          ++_itb;
          _offset=0;
        }
        return *this;
      }
      iterator operator++(int) { iterator t(*this); operator++(); return t; }
      value_type &operator*() { assert(_itb!=_parent->_buckets.end()); if(_itb==_parent->_buckets.end()) abort(); return _itb->items[_offset]->p.get(); }
      value_type &operator*() const { assert(_itb!=_parent->_buckets.end()); if(_itb==_parent->_buckets.end()) abort(); return _itb->items[_offset]->p.get(); }
    };
    // local_iterator
    // const_local_iterator
    concurrent_unordered_map() : _size(0), _buckets(13) { }
    concurrent_unordered_map(size_t n) : _size(0), _buckets(n>0 ? n : 1) { }
    ~concurrent_unordered_map() { clear(); }
    concurrent_unordered_map(const concurrent_unordered_map &);
    concurrent_unordered_map(concurrent_unordered_map &&);
    concurrent_unordered_map &operator=(const concurrent_unordered_map &);
    concurrent_unordered_map &operator=(concurrent_unordered_map &&);
    bool empty() const BOOST_NOEXCEPT { return _size==0; }
    size_type size() const BOOST_NOEXCEPT { return _size; }
    iterator begin() BOOST_NOEXCEPT
    {
       return iterator(this);
    }
    //const_iterator begin() const BOOST_NOEXCEPT
    iterator end() BOOST_NOEXCEPT
    {
      return iterator(this, nullptr);
    }
    //const_iterator end() const BOOST_NOEXCEPT
    iterator find(const key_type &k)
    {
      iterator ret=end();
      if(!_size) return ret;
      size_t h=_hasher(k);
      auto itb=_get_bucket(h);
      item_type *i=nullptr;
      bucket_type::using_ g(&(*itb)); // stop resizes during find
      for(;;)
      {
        if(!(i=itb->find(i, h)))
          break;
        std::unique_lock<decltype(i->p)> g(i->p, std::adopt_lock);
        if(_key_equal(k, i->p->first))
        {
          ret._itb=itb;
          ret._offset=itb->to_offset(i);
          break;
        }
      }
      return ret;
    }
    //const_iterator find(const keytype &k) const;
    template<class P> std::pair<iterator, bool> insert(P &&v)
    {
      std::pair<iterator, bool> ret(end(), true);
      size_t h=_hasher(k);
      auto itb=_get_bucket(h);
      item_type *i=nullptr;
      size_t emptyidx=0;
      {
        bucket_type::using_ g(&(*itb)); // stop resizes during find
        for(;;)
        {
          if(!(i=itb->find(i, h, !emptyidx ? &emptyidx : nullptr)))
            break;
          std::unique_lock<decltype(i->p)> g(i->p, std::adopt_lock);
          if(_key_equal(k, i->p->first))
          {
            ret.first._itb=itb;
            ret.first._offset=itb->to_offset(i);
            ret.second=false;
            break;
          }
        }
      }
      if(ret.second)
      {
        ret.first._itb=itb;
        ret.first._offset=itb->insert(_allocator, std::forward<P>(v), hash, emptyidx);
        ++_size;
      }
      return ret;
    }
    iterator erase(/*const_*/iterator it)
    {
      iterator ret=it;
      ret._itb->remove(allocator, ret._itb->_offset);
      ++ret;
      return ret;
    }
    void clear() BOOST_NOEXCEPT
    {
      for(auto &b : _buckets)
        b.clear(_allocator);
      _size=0;
    }
    void reserve(size_type n)
    {
      if(_size!=0) throw std::runtime_error("Cannot currently rehash existing content!");
      _buckets.resize(n);
    }
  };
} }

using namespace std;

TEST_CASE("spinlock/works", "Tests that the spinlock works as intended")
{
  boost::spinlock::spinlock<bool> lock;
  REQUIRE(lock.try_lock());
  REQUIRE(!lock.try_lock());
  lock.unlock();
  
  lock_guard<decltype(lock)> h(lock);
  REQUIRE(!lock.try_lock());
}

#ifdef _OPENMP
TEST_CASE("spinlock/works_threaded", "Tests that the spinlock works as intended under threads")
{
  boost::spinlock::spinlock<bool> lock;
  boost::spinlock::atomic<size_t> gate(0);
#pragma omp parallel
  {
    ++gate;
  }
  size_t threads=gate;
  for(size_t i=0; i<1000; i++)
  {
    gate.store(threads);
    size_t locked=0;
#pragma omp parallel for reduction(+:locked)
    for(int n=0; n<threads; n++)
    {
      --gate;
      while(gate);
      locked+=lock.try_lock();
    }
    REQUIRE(locked==1);
    lock.unlock();
  }
}

TEST_CASE("spinlock/works_transacted", "Tests that the spinlock works as intended under transactions")
{
  boost::spinlock::spinlock<bool> lock;
  boost::spinlock::atomic<size_t> gate(0);
  size_t locked=0;
#pragma omp parallel
  {
    ++gate;
  }
  size_t threads=gate;
#pragma omp parallel for
  for(int i=0; i<1000*threads; i++)
  {
    BOOST_BEGIN_TRANSACT_LOCK(lock)
    {
      ++locked;
    }
    BOOST_END_TRANSACT_LOCK(lock)
  }
  REQUIRE(locked==1000*threads);
}

static double CalculatePerformance(bool use_transact)
{
  boost::spinlock::spinlock<bool> lock;
  boost::spinlock::atomic<size_t> gate(0);
  struct
  {
    size_t value;
    char padding[64-sizeof(size_t)];
  } count[64];
  memset(&count, 0, sizeof(count));
  usCount start, end;
#pragma omp parallel
  {
    ++gate;
  }
  size_t threads=gate;
  //printf("There are %u threads in this CPU\n", (unsigned) threads);
  start=GetUsCount();
#pragma omp parallel for
  for(int thread=0; thread<threads; thread++)
  {
    --gate;
    while(gate);
    for(size_t n=0; n<10000000; n++)
    {
      if(use_transact)
      {
        BOOST_BEGIN_TRANSACT_LOCK(lock)
        {
          ++count[thread].value;
        }
        BOOST_END_TRANSACT_LOCK(lock)
      }
      else
      {
        std::lock_guard<decltype(lock)> g(lock);
        ++count[thread].value;      
      }
    }
  }
  end=GetUsCount();
  size_t increments=0;
  for(size_t thread=0; thread<threads; thread++)
  {
    REQUIRE(count[thread].value == 10000000);
    increments+=count[thread].value;
  }
  return increments/((end-start)/1000000000000.0);
}

TEST_CASE("performance/spinlock", "Tests the performance of spinlocks")
{
  printf("\n=== Spinlock performance ===\n");
  printf("1. Achieved %lf transactions per second\n", CalculatePerformance(false));
  printf("2. Achieved %lf transactions per second\n", CalculatePerformance(false));
  printf("3. Achieved %lf transactions per second\n", CalculatePerformance(false));
}

TEST_CASE("performance/transaction", "Tests the performance of spinlock transactions")
{
  printf("\n=== Transacted spinlock performance ===\n");
  printf("This CPU %s support Intel TSX memory transactions.\n", boost::spinlock::intel_stuff::have_intel_tsx_support() ? "DOES" : "does NOT");
  printf("1. Achieved %lf transactions per second\n", CalculatePerformance(true));
  printf("2. Achieved %lf transactions per second\n", CalculatePerformance(true));
  printf("3. Achieved %lf transactions per second\n", CalculatePerformance(true));
#ifdef BOOST_USING_INTEL_TSX
  if(boost::spinlock::intel_stuff::have_intel_tsx_support())
  {
    printf("\nForcing Intel TSX support off ...\n");
    boost::spinlock::intel_stuff::have_intel_tsx_support_result=1;
    printf("1. Achieved %lf transactions per second\n", CalculatePerformance(true));
    printf("2. Achieved %lf transactions per second\n", CalculatePerformance(true));
    printf("3. Achieved %lf transactions per second\n", CalculatePerformance(true));
    boost::spinlock::intel_stuff::have_intel_tsx_support_result=0;
  }
#endif
}

static double CalculateMallocPerformance(size_t size, bool use_transact)
{
  boost::spinlock::spinlock<bool> lock;
  boost::spinlock::atomic<size_t> gate(0);
  usCount start, end;
#pragma omp parallel
  {
    ++gate;
  }
  size_t threads=gate;
  //printf("There are %u threads in this CPU\n", (unsigned) threads);
  start=GetUsCount();
#pragma omp parallel for
  for(int n=0; n<10000000*threads; n++)
  {
    void *p;
    if(use_transact)
    {
      BOOST_BEGIN_TRANSACT_LOCK(lock)
      {
        p=malloc(size);
      }
      BOOST_END_TRANSACT_LOCK(lock)
    }
    else
    {
      std::lock_guard<decltype(lock)> g(lock);
      p=malloc(size);
    }
    if(use_transact)
    {
      BOOST_BEGIN_TRANSACT_LOCK(lock)
      {
        free(p);
      }
      BOOST_END_TRANSACT_LOCK(lock)
    }
    else
    {
      std::lock_guard<decltype(lock)> g(lock);
      free(p);
    }
  }
  end=GetUsCount();
  REQUIRE(true);
//  printf("size=%u\n", (unsigned) map.size());
  return threads*10000000/((end-start)/1000000000000.0);
}

TEST_CASE("performance/malloc/transact/small", "Tests the transact performance of multiple threads using small memory allocations")
{
  printf("\n=== Small malloc transact performance ===\n");
  printf("1. Achieved %lf transactions per second\n", CalculateMallocPerformance(16, 1));
  printf("2. Achieved %lf transactions per second\n", CalculateMallocPerformance(16, 1));
  printf("3. Achieved %lf transactions per second\n", CalculateMallocPerformance(16, 1));
}

TEST_CASE("performance/malloc/transact/large", "Tests the transact performance of multiple threads using large memory allocations")
{
  printf("\n=== Large malloc transact performance ===\n");
  printf("1. Achieved %lf transactions per second\n", CalculateMallocPerformance(65536, 1));
  printf("2. Achieved %lf transactions per second\n", CalculateMallocPerformance(65536, 1));
  printf("3. Achieved %lf transactions per second\n", CalculateMallocPerformance(65536, 1));
}

static double CalculateUnorderedMapPerformance(size_t reserve, bool use_transact, bool readwrites)
{
  boost::spinlock::spinlock<bool> lock;
  boost::spinlock::atomic<size_t> gate(0);
  std::unordered_map<int, int> map;
  usCount start, end;
  if(reserve)
  {
    map.reserve(reserve);
    for(size_t n=0; n<reserve/2; n++)
      map.insert(std::make_pair(reserve+n, n));
  }
#pragma omp parallel
  {
    ++gate;
  }
  size_t threads=gate;
  //printf("There are %u threads in this CPU\n", (unsigned) threads);
  start=GetUsCount();
#pragma omp parallel for
  for(int thread=0; thread<threads; thread++)
  for(int n=0; n<10000000; n++)
  {
    if(readwrites)
    {
      // One thread always writes with lock, remaining threads read with transact
      bool amMaster=(thread==0);
      if(amMaster)
      {
        bool doInsert=((n/threads) & 1)!=0;
        std::lock_guard<decltype(lock)> g(lock);
        if(doInsert)
          map.insert(std::make_pair(n, n));
        else if(!map.empty())
          map.erase(map.begin());
      }
      else
      {
        if(use_transact)
        {
          BOOST_BEGIN_TRANSACT_LOCK(lock)
          {
            map.find(n-1);
          }
          BOOST_END_TRANSACT_LOCK(lock)
        }
        else
        {
          std::lock_guard<decltype(lock)> g(lock);
          map.find(n-1);
        }
      }
    }
    else
    {
      if(use_transact)
      {
        BOOST_BEGIN_TRANSACT_LOCK(lock)
        {
          if((n & 255)<128)
            map.insert(std::make_pair(n, n));
          else if(!map.empty())
            map.erase(map.begin());
        }
        BOOST_END_TRANSACT_LOCK(lock)
      }
      else
      {
        std::lock_guard<decltype(lock)> g(lock);
        if((n & 255)<128)
          map.insert(std::make_pair(n, n));
        else if(!map.empty())
          map.erase(map.begin());
      }
    }
  }
  end=GetUsCount();
  REQUIRE(true);
//  printf("size=%u\n", (unsigned) map.size());
  return threads*10000000/((end-start)/1000000000000.0);
}

TEST_CASE("performance/unordered_map/small", "Tests the performance of multiple threads using a small unordered_map")
{
  printf("\n=== Small unordered_map spinlock performance ===\n");
  printf("1. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(0, false, false));
  printf("2. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(0, false, false));
  printf("3. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(0, false, false));
}

TEST_CASE("performance/unordered_map/large", "Tests the performance of multiple threads using a large unordered_map")
{
  printf("\n=== Large unordered_map spinlock performance ===\n");
  printf("1. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(10000, false, false));
  printf("2. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(10000, false, false));
  printf("3. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(10000, false, false));
}

TEST_CASE("performance/unordered_map/transact/small", "Tests the transact performance of multiple threads using a small unordered_map")
{
  printf("\n=== Small unordered_map transact performance ===\n");
  printf("1. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(0, true, false));
#ifndef BOOST_HAVE_TRANSACTIONAL_MEMORY_COMPILER
  printf("2. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(0, true, false));
  printf("3. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(0, true, false));
#endif
}

TEST_CASE("performance/unordered_map/transact/large", "Tests the transact performance of multiple threads using a large unordered_map")
{
  printf("\n=== Large unordered_map transact performance ===\n");
  printf("1. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(10000, true, false));
#ifndef BOOST_HAVE_TRANSACTIONAL_MEMORY_COMPILER
  printf("2. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(10000, true, false));
  printf("3. Achieved %lf transactions per second\n", CalculateUnorderedMapPerformance(10000, true, false));
#endif
}

static double CalculateConcurrentUnorderedMapPerformance(size_t reserve, bool readwrites)
{
  boost::spinlock::atomic<size_t> gate(0);
  boost::spinlock::concurrent_unordered_map<int, int> map;
  usCount start, end;
  if(reserve)
  {
    map.reserve(reserve);
    for(size_t n=0; n<reserve/2; n++)
      map.insert(std::make_pair(reserve+n, n));
  }
//#pragma omp parallel
  {
    ++gate;
  }
  size_t threads=gate;
  //printf("There are %u threads in this CPU\n", (unsigned) threads);
  start=GetUsCount();
//#pragma omp parallel for
  for(int thread=0; thread<threads; thread++)
  for(int n=0; n<10000000; n++)
  {
    if(readwrites)
    {
      // One thread always writes with lock, remaining threads read with transact
      bool amMaster=(thread==0);
      if(amMaster)
      {
        bool doInsert=((n/threads) & 1)!=0;
        if(doInsert)
          map.insert(std::make_pair(n, n));
        else if(!map.empty())
          map.erase(map.begin());
      }
      else
      {
        map.find(n-1);
      }
    }
    else
    {
      if((n & 255)<128)
        map.insert(std::make_pair(n, n));
      else if(!map.empty())
        map.erase(map.begin());
    }
  }
  end=GetUsCount();
  REQUIRE(true);
//  printf("size=%u\n", (unsigned) map.size());
  return threads*10000000/((end-start)/1000000000000.0);
}

TEST_CASE("performance/concurrent_unordered_map/small", "Tests the performance of multiple threads using a small concurrent_unordered_map")
{
  printf("\n=== Small concurrent_unordered_map performance ===\n");
  printf("1. Achieved %lf transactions per second\n", CalculateConcurrentUnorderedMapPerformance(0, false));
  printf("2. Achieved %lf transactions per second\n", CalculateConcurrentUnorderedMapPerformance(0, false));
  printf("3. Achieved %lf transactions per second\n", CalculateConcurrentUnorderedMapPerformance(0, false));
}

TEST_CASE("performance/concurrent_unordered_map/large", "Tests the performance of multiple threads using a large concurrent_unordered_map")
{
  printf("\n=== Large concurrent_unordered_map spinlock performance ===\n");
  printf("1. Achieved %lf transactions per second\n", CalculateConcurrentUnorderedMapPerformance(10000, false));
  printf("2. Achieved %lf transactions per second\n", CalculateConcurrentUnorderedMapPerformance(10000, false));
  printf("3. Achieved %lf transactions per second\n", CalculateConcurrentUnorderedMapPerformance(10000, false));
}

#endif

#ifndef BOOST_MEMORY_TRANSACTIONS_DISABLE_CATCH
int main(int argc, char *argv[])
{
#ifdef _OPENMP
  printf("These unit tests have been compiled with parallel support. I will use as many threads as CPU cores.\n");
#else
  printf("These unit tests have not been compiled with parallel support and will execute only those which are sequential.\n");
#endif
#ifdef BOOST_HAVE_TRANSACTIONAL_MEMORY_COMPILER
  printf("These unit tests have been compiled using a transactional compiler. I will use __transaction_relaxed.\n");
#else
  printf("These unit tests have not been compiled using a transactional compiler.\n");
#endif
  int result=Catch::Session().run(argc, argv);
  return result;
}
#endif
