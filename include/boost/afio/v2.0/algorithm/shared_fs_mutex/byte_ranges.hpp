/* Efficient small actor read-write lock
(C) 2016-2017 Niall Douglas <http://www.nedproductions.biz/> (12 commits)
File Created: March 2016


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

#ifndef BOOST_AFIO_SHARED_FS_MUTEX_BYTE_RANGES_HPP
#define BOOST_AFIO_SHARED_FS_MUTEX_BYTE_RANGES_HPP

#include "../../file_handle.hpp"
#include "base.hpp"

#include "../../small_prng.hpp"

//! \file byte_ranges.hpp Provides algorithm::shared_fs_mutex::byte_ranges

BOOST_AFIO_V2_NAMESPACE_BEGIN

namespace algorithm
{
  namespace shared_fs_mutex
  {
    /*! \class byte_ranges
    \brief Many entity shared/exclusive file system based lock

    This is a simple many entity shared mutex. It works by locking in the same file the byte at the
    offset of the entity id. If it fails to lock a byte, it backs out all preceding locks, randomises the order
    and tries locking them again until success. Needless to say this algorithm puts a lot of strain on
    your byte range locking implementation, some NFS implementations have been known to fail to cope.

    - Compatible with networked file systems, though be cautious with older NFS.
    - Linear complexity to number of concurrent users.
    - Exponential complexity to number of entities being concurrently locked, though some OSs
    provide linear complexity so long as total concurrent waiting processes is CPU core count or less.
    - Does a reasonable job of trying to sleep the thread if any of the entities are locked.
    - Sudden process exit with lock held is recovered from.
    - Sudden power loss during use is recovered from.
    - Safe for multithreaded usage of the same instance.

    Caveats:
    - When entities being locked is more than one, the algorithm places the contending lock at the
    front of the list during the randomisation after lock failure so we can sleep the thread until
    it becomes free. However, under heavy churn the thread will generally spin, consuming 100% CPU.
    - Byte range locks need to work properly on your system. Misconfiguring NFS or Samba
    to cause byte range locks to not work right will produce bad outcomes.
    - If your OS doesn't have sane byte range locks (OS X, BSD, older Linuxes) and multiple
    objects in your process use the same lock file, misoperation will occur. Use lock_files
    or share a single instance of this class per lock file in this case.
    */
    class byte_ranges : public shared_fs_mutex
    {
      file_handle _h;

      byte_ranges(file_handle &&h)
          : _h(std::move(h))
      {
      }
      byte_ranges(const byte_ranges &) = delete;
      byte_ranges &operator=(const byte_ranges &) = delete;

    public:
      //! The type of an entity id
      using entity_type = shared_fs_mutex::entity_type;
      //! The type of a sequence of entities
      using entities_type = shared_fs_mutex::entities_type;

      //! Move constructor
      byte_ranges(byte_ranges &&o) noexcept : _h(std::move(o._h)) {}
      //! Move assign
      byte_ranges &operator=(byte_ranges &&o) noexcept
      {
        _h = std::move(o._h);
        return *this;
      }

      //! Initialises a shared filing system mutex using the file at \em lockfile
      //[[bindlib::make_free]]
      static result<byte_ranges> fs_mutex_byte_ranges(file_handle::path_type lockfile) noexcept
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(0);
        BOOST_OUTCOME_TRY(ret, file_handle::file(std::move(lockfile), file_handle::mode::write, file_handle::creation::if_needed, file_handle::caching::temporary));
        return byte_ranges(std::move(ret));
      }

      //! Return the handle to file being used for this lock
      const file_handle &handle() const noexcept { return _h; }

    protected:
      virtual result<void> _lock(entities_guard &out, deadline d, bool spin_not_sleep) noexcept override final
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(this);
        stl11::chrono::steady_clock::time_point began_steady;
        stl11::chrono::system_clock::time_point end_utc;
        if(d)
        {
          if((d).steady)
            began_steady = stl11::chrono::steady_clock::now();
          else
            end_utc = (d).to_time_point();
        }
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
                  _h.unlock(out.entities[n].value, 1);
                _h.unlock(out.entities[0].value, 1);
              }
            });
            for(n = 0; n < out.entities.size(); n++)
            {
              deadline nd;
              // Only for very first entity will we sleep until its lock becomes available
              if(n)
                nd = deadline(stl11::chrono::seconds(0));
              else
              {
                nd = deadline();
                if(d)
                {
                  if((d).steady)
                  {
                    stl11::chrono::nanoseconds ns = stl11::chrono::duration_cast<stl11::chrono::nanoseconds>((began_steady + stl11::chrono::nanoseconds((d).nsecs)) - stl11::chrono::steady_clock::now());
                    if(ns.count() < 0)
                      (nd).nsecs = 0;
                    else
                      (nd).nsecs = ns.count();
                  }
                  else
                    (nd) = (d);
                }
              }
              auto outcome = _h.lock(out.entities[n].value, 1, out.entities[n].exclusive, nd);
              if(!outcome)
              {
                was_contended = n;
                goto failed;
              }
              outcome.get().release();
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
          std::swap(out.entities[was_contended], out.entities[0]);
          auto front = out.entities.begin();
          ++front;
          boost_lite::algorithm::small_prng::random_shuffle(front, out.entities.end());
          if(!spin_not_sleep)
            std::this_thread::yield();
        }
        // return make_valued_result<void>();
      }

    public:
      virtual void unlock(entities_type entities, unsigned long long) noexcept override final
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(this);
        for(const auto &i : entities)
        {
          _h.unlock(i.value, 1);
        }
      }
    };

  }  // namespace
}  // namespace

BOOST_AFIO_V2_NAMESPACE_END


#endif
