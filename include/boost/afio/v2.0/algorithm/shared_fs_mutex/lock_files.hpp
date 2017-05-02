/* Compatibility read-write lock
(C) 2016-2017 Niall Douglas <http://www.nedproductions.biz/> (11 commits)
File Created: April 2016


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

#ifndef BOOST_AFIO_SHARED_FS_MUTEX_LOCK_FILES_HPP
#define BOOST_AFIO_SHARED_FS_MUTEX_LOCK_FILES_HPP

#include "../../file_handle.hpp"
#include "base.hpp"

#include "../../small_prng.hpp"

//! \file lock_files.hpp Provides algorithm::shared_fs_mutex::lock_files

BOOST_AFIO_V2_NAMESPACE_BEGIN

namespace algorithm
{
  namespace shared_fs_mutex
  {
    /*! \class lock_files
    \brief Many entity exclusive compatibility file system based lock

    This is a very simple many entity shared mutex likely to work almost anywhere without surprises.
    It works by trying to exclusively create a file called the hex of the entity id. If it fails to
    exclusively create any file, it deletes all previously created files, randomises the order
    and tries locking them again until success. The only real reason to use this implementation
    is its excellent compatibility with almost everything, most users will want byte_ranges instead.

    - Compatible with all networked file systems.
    - Linear complexity to number of concurrent users.
    - Exponential complexity to number of contended entities being concurrently locked.
    - Requests for shared locks are treated as if for exclusive locks.

    Caveats:
    - No ability to sleep until a lock becomes free, so CPUs are spun at 100%.
    - On POSIX only sudden process exit with locks held will deadlock all other users by leaving stale
    files around.
    - Costs a file descriptor per entity locked.
    - Sudden power loss during use will deadlock first user after reboot, again due to stale files.
    - Currently this implementation does not permit more than one lock() per instance as the lock
    information is stored as member data. Creating multiple instances referring to the same path
    works fine. This could be fixed easily, but it would require a memory allocation per lock and
    user demand that this is actually a problem in practice.
    - Leaves many 16 character long hexadecimal named files in the supplied directory which may
    confuse users. Tip: create a hidden lockfile directory.

    Fixing the stale lock file problem could be quite trivial - simply byte range lock the first byte
    in the lock file to detect when a lock file is stale. However in this situation using the
    byte_ranges algorithm would be far superior, so implementing stale lock file clean up is left up
    to the user.
    */
    class lock_files : public shared_fs_mutex
    {
      file_handle::path_type _path;
      std::vector<file_handle> _hs;

      lock_files(file_handle::path_type &&o)
          : _path(std::move(o))
      {
      }
      lock_files(const lock_files &) = delete;
      lock_files &operator=(const lock_files &) = delete;

    public:
      //! The type of an entity id
      using entity_type = shared_fs_mutex::entity_type;
      //! The type of a sequence of entities
      using entities_type = shared_fs_mutex::entities_type;

      //! Move constructor
      lock_files(lock_files &&o) noexcept : _path(std::move(o._path)), _hs(std::move(o._hs)) {}
      //! Move assign
      lock_files &operator=(lock_files &&o) noexcept
      {
        _path = std::move(o._path);
        _hs = std::move(o._hs);
        return *this;
      }

      //! Initialises a shared filing system mutex using the directory at \em lockdir
      //[[bindlib::make_free]]
      static result<lock_files> fs_mutex_lock_files(file_handle::path_type lockdir) noexcept
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(0);
        return lock_files(std::move(lockdir));
      }

      //! Return the path to the directory being used for this lock
      const file_handle::path_type &path() const noexcept { return _path; }

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
        size_t n;
        // Create a set of paths to files to exclusively create
        std::vector<fixme_path> entity_paths(out.entities.size());
        for(n = 0; n < out.entities.size(); n++)
        {
          auto v = out.entities[n].value;
          entity_paths[n] = _path / boost_lite::algorithm::string::to_hex_string(span<char>((char *) &v, 8));
        }
        _hs.resize(out.entities.size());
        do
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
                {
                  (void) _hs[n].close();  // delete on close semantics deletes the file
                }
                (void) _hs[0].close();
              }
            });
            for(n = 0; n < out.entities.size(); n++)
            {
              auto ret = file_handle::file(entity_paths[n], file_handle::mode::write, file_handle::creation::only_if_not_exist, file_handle::caching::temporary, file_handle::flag::unlink_on_close);
              if(ret.has_error())
              {
                const auto &ec = ret.get_error();
                if(ec.category() != std::generic_category() || (ec.value() != EAGAIN && ec.value() != EEXIST))
                  return ret.get_error();
                // Collided with another locker
                was_contended = n;
                break;
              }
              _hs[n] = std::move(ret.get());
            }
            if(n == out.entities.size())
              undo.dismiss();
          }
          if(n != out.entities.size())
          {
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
            // Sleep for a very short time
            if(!spin_not_sleep)
              std::this_thread::yield();
          }
        } while(n < out.entities.size());
        return make_valued_result<void>();
      }

    public:
      virtual void unlock(entities_type, unsigned long long) noexcept override final
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(this);
        for(auto &i : _hs)
        {
          (void) i.close();  // delete on close semantics deletes the file
        }
      }
    };

  }  // namespace
}  // namespace

BOOST_AFIO_V2_NAMESPACE_END


#endif
