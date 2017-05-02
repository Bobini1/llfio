/* Efficient large actor read-write lock
(C) 2016-2017 Niall Douglas <http://www.nedproductions.biz/> (23 commits)
File Created: Aug 2016


Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License in the accompanying file
Licence.txt or at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.


Distributed under the Boost Software License, Version 1.0.
    (See accompanying file Licence.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

#ifndef BOOST_AFIO_SHARED_FS_MUTEX_MEMORY_MAP_HPP
#define BOOST_AFIO_SHARED_FS_MUTEX_MEMORY_MAP_HPP

#include "../../map_handle.hpp"
#include "base.hpp"

#include "../../hash.hpp"
#include "../../small_prng.hpp"
#include "../../spinlock.hpp"

//! \file memory_map.hpp Provides algorithm::shared_fs_mutex::memory_map

BOOST_AFIO_V2_NAMESPACE_BEGIN

namespace algorithm
{
  namespace shared_fs_mutex
  {
    /*! \class memory_map
    \brief Many entity memory mapped shared/exclusive file system based lock
    \tparam Hasher A STL compatible hash algorithm to use (defaults to `fnv1a_hash`)
    \tparam HashIndexSize The size in bytes of the hash index to use (defaults to 4Kb)
    \tparam SpinlockType The type of spinlock to use (defaults to a `SharedMutex` concept spinlock)

    This is the highest performing filing system mutex in AFIO, but it comes with a long list of potential
    gotchas. It works by creating a random temporary file somewhere on the system and placing its path
    in a file at the lock file location. The random temporary file is mapped into memory by all processes
    using the lock where an open addressed hash table is kept. Each entity is hashed into somewhere in the
    hash table and its individual spin lock is used to implement the exclusion. As with `byte_ranges`, each
    entity is locked individually in sequence but if a particular lock fails, all are unlocked and the
    list is randomised before trying again. Because this locking
    implementation is entirely implemented in userspace using shared memory without any kernel syscalls,
    performance is probably as fast as any many-arbitrary-entity shared locking system could be.

    Performance ought to be excellent so long as no lock user attempts to use the lock from across a
    networked filing system. As soon as a locking entity fails to find the temporary file given in the
    lock file location, it will *permanently* degrade the memory mapped lock into the "fallback" lock
    specified in the constructor (if you do not specify one, `EBUSY` will be forever returned from then
    on when trying to lock). It is up to the end user to decide when it might be time to destroy and
    reconstruct `memory_map` in order to restore full performance.

    - Compatible with networked file systems, though with a substantial performance degrade as described above.
    - Linear complexity to number of concurrent users up until hash table starts to get full or hashed
    entries collide.
    - Sudden power loss during use is recovered from.
    - Safe for multithreaded usage of the same instance.
    - In the lightly contended case, an order of magnitude faster than any other `shared_fs_mutex` algorithm.

    Caveats:
    - A transition between mapped and fallback locks will not complete until all current mapped memory users
    have realised the transition has happened. This can take a very significant amount of time if a lock user
    does not regularly lock its locks.
    - No ability to sleep until a lock becomes free, so CPUs are spun at 100%.
    - Sudden process exit with locks held will deadlock all other users.
    - Exponential complexity to number of entities being concurrently locked.
    - Exponential complexity to concurrency if entities hash to the same cache line. Most SMP and especially
    NUMA systems have a finite bandwidth for atomic compare and swap operations, and every attempt to
    lock or unlock an entity under this implementation is several of those operations. Under heavy contention,
    whole system performance very noticeably nose dives from excessive atomic operations, things like audio and the
    mouse pointer will stutter.
    - Sometimes different entities hash to the same offset and collide with one another, causing very poor performance.
    - Byte range locks need to work properly on your system. Misconfiguring NFS or Samba
    to cause byte range locks to not work right will produce bad outcomes.
    - Memory mapped files need to be cache unified with normal i/o in your OS kernel. Known OSs which
    don't use a unified cache for memory mapped and normal i/o are QNX, OpenBSD. Furthermore, doing
    normal i/o and memory mapped i/o to the same file needs to not corrupt the file. In the past,
    there have been editions of the Linux kernel and the OS X kernel which did this.
    - If your OS doesn't have sane byte range locks (OS X, BSD, older Linuxes) and multiple
    objects in your process use the same lock file, misoperation will occur.

    \todo It should be possible to auto early out from a memory_map transition by scanning the memory map for any
    locked items, and if none then to proceed.
    \todo fs_mutex_map needs to check if this inode is that at the path after lock is granted, awaiting stat_t port.
    \todo memory_map::_hash_entities needs to hash x16, x8 and x4 at a time to encourage auto vectorisation
    \todo memory_map::unlock() degrade is racy when single instance being used by multiple threads
    */
    template <template <class> class Hasher = boost_lite::algorithm::hash::fnv1a_hash, size_t HashIndexSize = 4096, class SpinlockType = boost_lite::configurable_spinlock::shared_spinlock<>> class memory_map : public shared_fs_mutex
    {
    public:
      //! The type of an entity id
      using entity_type = shared_fs_mutex::entity_type;
      //! The type of a sequence of entities
      using entities_type = shared_fs_mutex::entities_type;
      //! The type of the hasher being used
      using hasher_type = Hasher<entity_type::value_type>;
      //! The type of the spinlock being used
      using spinlock_type = SpinlockType;

    private:
      static constexpr size_t _container_entries = HashIndexSize / sizeof(spinlock_type);
      using _hash_index_type = std::array<spinlock_type, _container_entries>;
      static constexpr file_handle::extent_type _lockinuseoffset = (file_handle::extent_type) 1024 * 1024;
      static constexpr file_handle::extent_type _mapinuseoffset = (file_handle::extent_type) 1024 * 1024 + 1;

      file_handle _h, _temph;
      file_handle::extent_guard _hlockinuse;  // shared lock of last byte of _h marking if lock is in use
      file_handle::extent_guard _hmapinuse;   // shared lock of second last byte of _h marking if mmap is in use
      map_handle _hmap, _temphmap;
      shared_fs_mutex *_fallbacklock;
      bool _have_degraded;

      _hash_index_type &_index() const
      {
        _hash_index_type *ret = (_hash_index_type *) _temphmap.address();
        return *ret;
      }

      memory_map(file_handle &&h, file_handle &&temph, file_handle::extent_guard &&hlockinuse, file_handle::extent_guard &&hmapinuse, map_handle &&hmap, map_handle &&temphmap, shared_fs_mutex *fallbacklock)
          : _h(std::move(h))
          , _temph(std::move(temph))
          , _hlockinuse(std::move(hlockinuse))
          , _hmapinuse(std::move(hmapinuse))
          , _hmap(std::move(hmap))
          , _temphmap(std::move(temphmap))
          , _fallbacklock(fallbacklock)
          , _have_degraded(false)
      {
        _hlockinuse.set_handle(&_h);
        _hmapinuse.set_handle(&_h);
      }
      memory_map(const memory_map &) = delete;
      memory_map &operator=(const memory_map &) = delete;

    public:
      //! Returns the fallback lock
      shared_fs_mutex *fallback() const noexcept { return _fallbacklock; }
      //! Sets the fallback lock
      void fallback(shared_fs_mutex *fbl) noexcept { _fallbacklock = fbl; }
      //! True if this lock has degraded due to a network user trying to use it
      bool is_degraded() const noexcept { return _hmap.address()[0] == 0; }

      //! Move constructor
      memory_map(memory_map &&o) noexcept : _h(std::move(o._h)), _temph(std::move(o._temph)), _hlockinuse(std::move(o._hlockinuse)), _hmapinuse(std::move(o._hmapinuse)), _hmap(std::move(o._hmap)), _temphmap(std::move(o._temphmap)), _fallbacklock(std::move(o._fallbacklock)), _have_degraded(std::move(o._have_degraded))
      {
        _hlockinuse.set_handle(&_h);
        _hmapinuse.set_handle(&_h);
      }
      //! Move assign
      memory_map &operator=(memory_map &&o) noexcept
      {
        this->~memory_map();
        new(this) memory_map(std::move(o));
        return *this;
      }
      ~memory_map()
      {
        if(_h.is_valid())
        {
          // Release my shared locks and try locking inuse exclusively
          _hmapinuse.unlock();
          _hlockinuse.unlock();
          auto lockresult = _h.try_lock(_lockinuseoffset, 1, true);
#ifndef NDEBUG
          if(!lockresult && lockresult.error() != stl11::errc::timed_out)
          {
            BOOST_AFIO_LOG_FATAL(0, "memory_map::~memory_map() try_lock failed");
            abort();
          }
#endif
          if(lockresult)
          {
            // This means I am the last user, so zop the file contents as temp file is about to go away
            char buffer[4096];
            memset(buffer, 0, sizeof(buffer));
            (void) _h.write(0, buffer, sizeof(buffer));
            // You might wonder why I am now truncating to zero? It's to ensure any
            // memory maps definitely get written with zeros before truncation, some
            // OSs don't reflect zeros into memory maps upon truncation for quite a
            // long time (or ever)
            _h.truncate(0);
            // Unlink the temp file
            _temph.unlink();
          }
        }
      }

      /*! Initialises a shared filing system mutex using the file at \em lockfile and an optional fallback lock.
      \errors Awaiting the clang result<> AST parser which auto generates all the error codes which could occur,
      but a particularly important one is `EBUSY` which will be returned if the memory map lock is already in
      a degraded state (i.e. just use the fallback lock directly).
      */
      //[[bindlib::make_free]]
      static result<memory_map> fs_mutex_map(file_handle::path_type lockfile, shared_fs_mutex *fallbacklock = nullptr) noexcept
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(0);
        try
        {
          BOOST_OUTCOME_TRY(ret, file_handle::file(std::move(lockfile), file_handle::mode::write, file_handle::creation::if_needed, file_handle::caching::reads));
          file_handle temph;
          // Am I the first person to this file? Lock the inuse exclusively
          auto lockinuse = ret.try_lock(_lockinuseoffset, 1, true);
          file_handle::extent_guard mapinuse;
          if(lockinuse.has_error())
          {
            if(lockinuse.get_error().value() != ETIMEDOUT)
              return lockinuse.get_error();
            // Somebody else is also using this file, so try to read the hash index file I ought to use
            lockinuse = ret.lock(_lockinuseoffset, 1, false);  // last byte shared access
            char buffer[65536];
            memset(buffer, 0, sizeof(buffer));
            {
              BOOST_OUTCOME_TRY(_, ret.read(0, buffer, 65535));
              (void) _;
            }
            fixme_path::value_type *temphpath = (fixme_path::value_type *) buffer;
            result<file_handle> _temph;
            // If path is zeroed, fall back onto backup lock
            if(!buffer[0])
              goto use_fall_back_lock;
            else
              _temph = file_handle::file(temphpath, file_handle::mode::write, file_handle::creation::open_existing, file_handle::caching::temporary);
            // If temp file doesn't exist, I am on a different machine
            if(!_temph)
            {
              // Zop the path so any new entrants into this lock will go to the fallback lock
              memset(buffer, 0, 4096);
              (void) ret.write(0, buffer, 4096);
            use_fall_back_lock:
              // I am guaranteed that all mmap users have locked the second last byte
              // and will unlock it once everyone has stopped using the mmap, so make
              // absolutely sure the mmap is not in use by anyone by taking an exclusive
              // lock on the second final byte
              BOOST_OUTCOME_TRY(mapinuse2, ret.lock(_mapinuseoffset, 1, true));
              // Release the exclusive lock and tell caller to just use the fallback lock directly
              return make_errored_result<memory_map>(stl11::errc::device_or_resource_busy);
            }
            else
            {
              // Mark the map as being in use by me too
              BOOST_OUTCOME_TRY(mapinuse2, ret.lock(_mapinuseoffset, 1, false));
              mapinuse = std::move(mapinuse2);
              temph = std::move(_temph.get());
            }
            // Map the files into memory, being very careful that the lock file is only ever mapped read only
            // as some OSs can get confused if you use non-mmaped writes on a region mapped for writing.
            BOOST_OUTCOME_TRY(hsection, section_handle::section(ret, 0, section_handle::flag::read));
            BOOST_OUTCOME_TRY(temphsection, section_handle::section(temph, HashIndexSize));
            BOOST_OUTCOME_TRY(hmap, map_handle::map(hsection, 0, 0, section_handle::flag::read));
            BOOST_OUTCOME_TRY(temphmap, map_handle::map(temphsection, HashIndexSize));
            return memory_map(std::move(ret), std::move(temph), std::move(lockinuse.get()), std::move(mapinuse), std::move(hmap), std::move(temphmap), fallbacklock);
          }
          else
          {
            // I am the first person to be using this (stale?) file, so create a new hash index file and write its path
            BOOST_OUTCOME_TRYV(ret.truncate(0));
            BOOST_OUTCOME_TRY(_temph, file_handle::random_file(fixme_temporary_files_directory()));
            temph = std::move(_temph);
            auto temppath(temph.path());
            BOOST_OUTCOME_TRYV(temph.truncate(HashIndexSize));
            /* Linux appears to have a race where:
                 1. This process creates a new file and fallocate's its maximum extent.
                 2. Another process opens this file and mmaps it.
                 3. The other process tries to read from the mmap, and gets a SIGBUS for its efforts.

               I tried writing zeros using write after the fallocate, but it appears not to help, so
               for Linux compatibility we will have to mmap before publishing the path of the hash index.
            */
            // Map the files into memory, being very careful that the lock file is only ever mapped read only
            // as some OSs can get confused if you use non-mmaped writes on a region mapped for writing.
            BOOST_OUTCOME_TRY(temphsection, section_handle::section(temph, HashIndexSize));
            BOOST_OUTCOME_TRY(temphmap, map_handle::map(temphsection, HashIndexSize));
            // Force page allocation now
            memset(temphmap.address(), 0, HashIndexSize);
            // Write the path of my new hash index file and convert my lock to a shared one
            BOOST_OUTCOME_TRYV(ret.write(0, (const char *) temppath.c_str(), temppath.native().size() * sizeof(*temppath.c_str())));
            BOOST_OUTCOME_TRY(hsection, section_handle::section(ret, 0, section_handle::flag::read));
            BOOST_OUTCOME_TRY(hmap, map_handle::map(hsection, 0, 0, section_handle::flag::read));
            // Convert exclusive whole file lock into lock in use
            BOOST_OUTCOME_TRY(mapinuse2, ret.lock(_mapinuseoffset, 1, false));
            BOOST_OUTCOME_TRY(lockinuse2, ret.lock(_lockinuseoffset, 1, false));
            mapinuse = std::move(mapinuse2);
            lockinuse = std::move(lockinuse2);
            return memory_map(std::move(ret), std::move(temph), std::move(lockinuse.get()), std::move(mapinuse), std::move(hmap), std::move(temphmap), fallbacklock);
          }
        }
        BOOST_OUTCOME_CATCH_ALL_EXCEPTION_TO_RESULT
      }

      //! Return the handle to file being used for this lock
      const file_handle &handle() const noexcept { return _h; }

    protected:
      struct _entity_idx
      {
        unsigned value : 31;
        unsigned exclusive : 1;
      };
      // Create a cache of entities to their indices, eliding collisions where necessary
      static span<_entity_idx> _hash_entities(_entity_idx *entity_to_idx, entities_type &entities)
      {
        _entity_idx *ep = entity_to_idx;
        for(size_t n = 0; n < entities.size(); n++)
        {
          ep->value = hasher_type()(entities[n].value) % _container_entries;
          ep->exclusive = entities[n].exclusive;
          bool skip = false;
          for(size_t m = 0; m < n; m++)
          {
            if(entity_to_idx[m].value == ep->value)
            {
              if(ep->exclusive && !entity_to_idx[m].exclusive)
                entity_to_idx[m].exclusive = true;
              skip = true;
            }
          }
          if(!skip)
            ++ep;
        }
        return span<_entity_idx>(entity_to_idx, ep - entity_to_idx);
      }
      virtual result<void> _lock(entities_guard &out, deadline d, bool spin_not_sleep) noexcept override final
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(this);
        if(is_degraded())
        {
          if(!_have_degraded)
          {
            // We have just become degraded, so release our shared lock of the map
            // being in use and lock it exclusively, this will gate us on all other
            // current users of the map so we don't unblock until all current users
            // reach this same point. If that lock times out, we will reenter here
            // next time until we succeed
            _hmapinuse.unlock();
            BOOST_OUTCOME_TRY(mapinuse2, _h.lock(_mapinuseoffset, 1, true, d));
            _have_degraded = true;
          }
          if(_fallbacklock)
            return _fallbacklock->_lock(out, d, spin_not_sleep);
          return make_errored_result<void>(stl11::errc::device_or_resource_busy);
        }
        stl11::chrono::steady_clock::time_point began_steady;
        stl11::chrono::system_clock::time_point end_utc;
        if(d)
        {
          if((d).steady)
            began_steady = stl11::chrono::steady_clock::now();
          else
            end_utc = (d).to_time_point();
        }
        // alloca() always returns 16 byte aligned addresses
        span<_entity_idx> entity_to_idx(_hash_entities((_entity_idx *) alloca(sizeof(_entity_idx) * out.entities.size()), out.entities));
        _hash_index_type &index = _index();
        // Fire this if an error occurs
        auto disableunlock = undoer([&] { out.release(); });
        size_t n;
        for(;;)
        {
          size_t was_contended = (size_t) -1;
          {
            auto undo = undoer([&] {
              // 0 to (n-1) need to be closed
              if(n > 0)
              {
                --n;
                // Now 0 to n needs to be closed
                for(; n > 0; n--)
                  entity_to_idx[n].exclusive ? index[entity_to_idx[n].value].unlock() : index[entity_to_idx[n].value].unlock_shared();
                entity_to_idx[0].exclusive ? index[entity_to_idx[0].value].unlock() : index[entity_to_idx[0].value].unlock_shared();
              }
            });
            for(n = 0; n < entity_to_idx.size(); n++)
            {
              if(!(entity_to_idx[n].exclusive ? index[entity_to_idx[n].value].try_lock() : index[entity_to_idx[n].value].try_lock_shared()))
              {
                was_contended = n;
                goto failed;
              }
            }
            // Everything is locked, exit
            undo.dismiss();
            disableunlock.dismiss();
            return make_valued_result<void>();
          }
        failed:
          if(d)
          {
            if((d).steady)
            {
              if(stl11::chrono::steady_clock::now() >= (began_steady + stl11::chrono::nanoseconds((d).nsecs)))
                return make_errored_result<void>(stl11::errc::timed_out);
            }
            else
            {
              if(stl11::chrono::system_clock::now() >= end_utc)
                return make_errored_result<void>(stl11::errc::timed_out);
            }
          }
          // Move was_contended to front and randomise rest of out.entities
          std::swap(entity_to_idx[was_contended], entity_to_idx[0]);
          auto front = entity_to_idx.begin();
          ++front;
          boost_lite::algorithm::small_prng::random_shuffle(front, entity_to_idx.end());
          if(!spin_not_sleep)
            std::this_thread::yield();
        }
        // return make_valued_result<void>();
      }

    public:
      virtual void unlock(entities_type entities, unsigned long long hint) noexcept override final
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(this);
        if(_have_degraded)
        {
          if(_fallbacklock)
            _fallbacklock->unlock(entities, hint);
          return;
        }
        span<_entity_idx> entity_to_idx(_hash_entities((_entity_idx *) alloca(sizeof(_entity_idx) * entities.size()), entities));
        _hash_index_type &index = _index();
        for(const auto &i : entity_to_idx)
        {
          i.exclusive ? index[i.value].unlock() : index[i.value].unlock_shared();
        }
        if(is_degraded())
          _hmapinuse.unlock();
      }
    };

  }  // namespace
}  // namespace

BOOST_AFIO_V2_NAMESPACE_END


#endif
