/* A handle to something
(C) 2015-2017 Niall Douglas <http://www.nedproductions.biz/> (11 commits)
File Created: Dec 2015


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

#include "../../../handle.hpp"

#include <fcntl.h>
#include <limits.h>   // for IOV_MAX
#include <sys/uio.h>  // for preadv etc
#include <unistd.h>
#if BOOST_AFIO_USE_POSIX_AIO
#include <aio.h>
#endif

BOOST_AFIO_V2_NAMESPACE_BEGIN

handle::~handle()
{
  if(_v)
  {
    // Call close() below
    auto ret = handle::close();
    if(ret.has_error())
    {
      BOOST_AFIO_LOG_FATAL(_v.fd, "handle::~handle() close failed");
      abort();
    }
  }
}

result<void> handle::close() noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.fd);
  if(_v)
  {
    if(are_safety_fsyncs_issued())
    {
      if(-1 == fsync(_v.fd))
        return make_errored_result<void>(errno);
    }
    if(-1 == ::close(_v.fd))
      return make_errored_result<void>(errno);
    _v = native_handle_type();
  }
  return make_valued_result<void>();
}

result<handle> handle::clone() const noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.fd);
  result<handle> ret(handle(native_handle_type(), _caching, _flags));
  ret.value()._v.behaviour = _v.behaviour;
  ret.value()._v.fd = ::dup(_v.fd);
  if(-1 == ret.value()._v.fd)
    return make_errored_result<handle>(errno, last190(path().native()));
  return ret;
}

result<void> handle::set_append_only(bool enable) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.fd);
  int attribs = fcntl(_v.fd, F_GETFL);
  if(-1 == attribs)
    return make_errored_result<void>(errno, last190(path().native()));
  if(enable)
  {
    // Set append_only
    attribs |= O_APPEND;
    if(-1 == fcntl(_v.fd, F_SETFL, attribs))
      return make_errored_result<void>(errno, last190(path().native()));
    _v.behaviour |= native_handle_type::disposition::append_only;
  }
  else
  {
    // Remove append_only
    attribs &= ~O_APPEND;
    if(-1 == fcntl(_v.fd, F_SETFL, attribs))
      return make_errored_result<void>(errno, last190(path().native()));
    _v.behaviour &= ~native_handle_type::disposition::append_only;
  }
  return make_valued_result<void>();
}

result<void> handle::set_kernel_caching(caching caching) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.fd);
  int attribs = fcntl(_v.fd, F_GETFL);
  if(-1 == attribs)
    return make_errored_result<void>(errno);
  attribs &= ~(O_SYNC | O_DIRECT
#ifdef O_DSYNC
               | O_DSYNC
#endif
               );
  switch(_caching)
  {
  case handle::caching::unchanged:
    break;
  case handle::caching::none:
    attribs |= O_SYNC | O_DIRECT;
    if(-1 == fcntl(_v.fd, F_SETFL, attribs))
      return make_errored_result<void>(errno, last190(path().native()));
    _v.behaviour |= native_handle_type::disposition::aligned_io;
    break;
  case handle::caching::only_metadata:
    attribs |= O_DIRECT;
    if(-1 == fcntl(_v.fd, F_SETFL, attribs))
      return make_errored_result<void>(errno, last190(path().native()));
    _v.behaviour |= native_handle_type::disposition::aligned_io;
    break;
  case handle::caching::reads:
    attribs |= O_SYNC;
    if(-1 == fcntl(_v.fd, F_SETFL, attribs))
      return make_errored_result<void>(errno, last190(path().native()));
    _v.behaviour &= ~native_handle_type::disposition::aligned_io;
    break;
  case handle::caching::reads_and_metadata:
#ifdef O_DSYNC
    attribs |= O_DSYNC;
#else
    attribs |= O_SYNC;
#endif
    if(-1 == fcntl(_v.fd, F_SETFL, attribs))
      return make_errored_result<void>(errno, last190(path().native()));
    _v.behaviour &= ~native_handle_type::disposition::aligned_io;
    break;
  case handle::caching::all:
  case handle::caching::safety_fsyncs:
  case handle::caching::temporary:
    if(-1 == fcntl(_v.fd, F_SETFL, attribs))
      return make_errored_result<void>(errno, last190(path().native()));
    _v.behaviour &= ~native_handle_type::disposition::aligned_io;
    break;
  }
  _caching = caching;
  return make_valued_result<void>();
}


/************************************* io_handle ****************************************/

io_handle::io_result<io_handle::buffers_type> io_handle::read(io_handle::io_request<io_handle::buffers_type> reqs, deadline d) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.fd);
  if(d)
    return make_errored_result<>(stl11::errc::not_supported);
  if(reqs.buffers.size() > IOV_MAX)
    return make_errored_result<>(stl11::errc::argument_list_too_long);
  struct iovec *iov = (struct iovec *) alloca(reqs.buffers.size() * sizeof(struct iovec));
  for(size_t n = 0; n < reqs.buffers.size(); n++)
  {
    iov[n].iov_base = reqs.buffers[n].first;
    iov[n].iov_len = reqs.buffers[n].second;
  }
  ssize_t bytesread = ::preadv(_v.fd, iov, reqs.buffers.size(), reqs.offset);
  if(bytesread < 0)
    return make_errored_result<io_handle::buffers_type>(errno, last190(path().native()));
  for(size_t n = 0; n < reqs.buffers.size(); n++)
  {
    if(reqs.buffers[n].second >= (size_t) bytesread)
      bytesread -= reqs.buffers[n].second;
    else if(bytesread > 0)
    {
      reqs.buffers[n].second = bytesread;
      bytesread = 0;
    }
    else
      reqs.buffers[n].second = 0;
  }
  return io_handle::io_result<io_handle::buffers_type>(std::move(reqs.buffers));
}

io_handle::io_result<io_handle::const_buffers_type> io_handle::write(io_handle::io_request<io_handle::const_buffers_type> reqs, deadline d) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.fd);
  if(d)
    return make_errored_result<>(stl11::errc::not_supported);
  if(reqs.buffers.size() > IOV_MAX)
    return make_errored_result<>(stl11::errc::argument_list_too_long);
  struct iovec *iov = (struct iovec *) alloca(reqs.buffers.size() * sizeof(struct iovec));
  for(size_t n = 0; n < reqs.buffers.size(); n++)
  {
    iov[n].iov_base = const_cast<char *>(reqs.buffers[n].first);
    iov[n].iov_len = reqs.buffers[n].second;
  }
  ssize_t byteswritten = ::pwritev(_v.fd, iov, reqs.buffers.size(), reqs.offset);
  if(byteswritten < 0)
    return make_errored_result<io_handle::const_buffers_type>(errno, last190(path().native()));
  for(size_t n = 0; n < reqs.buffers.size(); n++)
  {
    if(reqs.buffers[n].second >= (size_t) byteswritten)
      byteswritten -= reqs.buffers[n].second;
    else if(byteswritten > 0)
    {
      reqs.buffers[n].second = byteswritten;
      byteswritten = 0;
    }
    else
      reqs.buffers[n].second = 0;
  }
  return io_handle::io_result<io_handle::const_buffers_type>(std::move(reqs.buffers));
}

result<io_handle::extent_guard> io_handle::lock(io_handle::extent_type offset, io_handle::extent_type bytes, bool exclusive, deadline d) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.fd);
  if(d && d.nsecs > 0)
    return make_errored_result<io_handle::extent_guard>(stl11::errc::not_supported);
  bool failed = false;
#if !defined(__linux__) && !defined(F_OFD_SETLK)
  if(0 == bytes)
  {
    // Non-Linux has a sane locking system in flock() if you are willing to lock the entire file
    int operation = ((d && !d.nsec) ? LOCK_NB : 0) | (exclusive ? LOCK_EX : LOCK_SH);
    if(-1 == flock(_v.fd, operation))
      failed = true;
  }
  else
#endif
  {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = exclusive ? F_WRLCK : F_RDLCK;
    constexpr extent_type extent_topbit = (extent_type) 1 << (8 * sizeof(extent_type) - 1);
    if(offset & extent_topbit)
    {
      BOOST_AFIO_LOG_WARN(_v.fd, "io_handle::lock() called with offset with top bit set, masking out");
    }
    if(bytes & extent_topbit)
    {
      BOOST_AFIO_LOG_WARN(_v.fd, "io_handle::lock() called with bytes with top bit set, masking out");
    }
    fl.l_whence = SEEK_SET;
    fl.l_start = offset & ~extent_topbit;
    fl.l_len = bytes & ~extent_topbit;
#ifdef F_OFD_SETLK
    if(-1 == fcntl(_v.fd, (d && !d.nsecs) ? F_OFD_SETLK : F_OFD_SETLKW, &fl))
    {
      if(EINVAL == errno)  // OFD locks not supported on this kernel
      {
        if(-1 == fcntl(_v.fd, (d && !d.nsecs) ? F_SETLK : F_SETLKW, &fl))
          failed = true;
        else
          _flags |= flag::byte_lock_insanity;
      }
      else
        failed = true;
    }
#else
    if(-1 == fcntl(_v.fd, (d && !d.nsecs) ? F_SETLK : F_SETLKW, &fl))
      failed = true;
    else
      _flags |= flag::byte_lock_insanity;
#endif
  }
  if(failed)
  {
    if(d && !d.nsecs && (EACCES == errno || EAGAIN == errno || EWOULDBLOCK == errno))
      return make_errored_result<void>(stl11::errc::timed_out);
    else
      return make_errored_result<void>(errno, last190(path().native()));
  }
  return extent_guard(this, offset, bytes, exclusive);
}

void io_handle::unlock(io_handle::extent_type offset, io_handle::extent_type bytes) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.fd);
  bool failed = false;
#if !defined(__linux__) && !defined(F_OFD_SETLK)
  if(0 == bytes)
  {
    if(-1 == flock(_v.fd, LOCK_UN))
      failed = true;
  }
  else
#endif
  {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    constexpr extent_type extent_topbit = (extent_type) 1 << (8 * sizeof(extent_type) - 1);
    fl.l_whence = SEEK_SET;
    fl.l_start = offset & ~extent_topbit;
    fl.l_len = bytes & ~extent_topbit;
#ifdef F_OFD_SETLK
    if(-1 == fcntl(_v.fd, F_OFD_SETLK, &fl))
    {
      if(EINVAL == errno)  // OFD locks not supported on this kernel
      {
        if(-1 == fcntl(_v.fd, F_SETLK, &fl))
          failed = true;
      }
      else
        failed = true;
    }
#else
    if(-1 == fcntl(_v.fd, F_SETLK, &fl))
      failed = true;
#endif
  }
  if(failed)
  {
    auto ret = make_errored_result<void>(errno);
    (void) ret;
    BOOST_AFIO_LOG_FATAL(_v.fd, "io_handle::unlock() failed");
    std::terminate();
  }
}


BOOST_AFIO_V2_NAMESPACE_END
