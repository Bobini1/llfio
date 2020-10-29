/* Information about the volume storing a file
(C) 2016-2017 Niall Douglas <http://www.nedproductions.biz/> (5 commits)
File Created: Jan 2016


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
#include "../../../statfs.hpp"

#include <sys/mount.h>
#ifdef __linux__
#include <mntent.h>
#include <sys/statfs.h>
#endif

#include <iostream>

LLFIO_V2_NAMESPACE_BEGIN

LLFIO_HEADERS_ONLY_MEMFUNC_SPEC result<size_t> statfs_t::fill(const handle &h, statfs_t::want wanted) noexcept
{
  size_t ret = 0;
#ifdef __linux__
  struct statfs64 s
  {
  };
  memset(&s, 0, sizeof(s));
  if(-1 == fstatfs64(h.native_handle().fd, &s))
  {
    return posix_error();
  }
  if(!!(wanted & want::bsize))
  {
    f_bsize = s.f_bsize;
    ++ret;
  }
  if(!!(wanted & want::iosize))
  {
    f_iosize = s.f_frsize;
    ++ret;
  }
  if(!!(wanted & want::blocks))
  {
    f_blocks = s.f_blocks;
    ++ret;
  }
  if(!!(wanted & want::bfree))
  {
    f_bfree = s.f_bfree;
    ++ret;
  }
  if(!!(wanted & want::bavail))
  {
    f_bavail = s.f_bavail;
    ++ret;
  }
  if(!!(wanted & want::files))
  {
    f_files = s.f_files;
    ++ret;
  }
  if(!!(wanted & want::ffree))
  {
    f_ffree = s.f_ffree;
    ++ret;
  }
  if(!!(wanted & want::namemax))
  {
    f_namemax = s.f_namelen;
    ++ret;
  }
  //            if(!!(wanted&&want::owner))       { f_owner      =s.f_owner;  ++ret; }
  if(!!(wanted & want::fsid))
  {
    f_fsid[0] = static_cast<unsigned>(s.f_fsid.__val[0]);
    f_fsid[1] = static_cast<unsigned>(s.f_fsid.__val[1]);
    ++ret;
  }
  if(!!(wanted & want::flags) || !!(wanted & want::fstypename) || !!(wanted & want::mntfromname) || !!(wanted & want::mntonname))
  {
    try
    {
      struct mountentry
      {
        std::string mnt_fsname, mnt_dir, mnt_type, mnt_opts;
        mountentry(const char *a, const char *b, const char *c, const char *d)
            : mnt_fsname(a)
            , mnt_dir(b)
            , mnt_type(c)
            , mnt_opts(d)
        {
        }
      };
      std::vector<std::pair<mountentry, struct statfs64>> mountentries;
      {
        // Need to parse mount options on Linux
        FILE *mtab = setmntent("/etc/mtab", "r");
        if(mtab == nullptr)
        {
          mtab = setmntent("/proc/mounts", "r");
        }
        if(mtab == nullptr)
        {
          return posix_error();
        }
        auto unmtab = make_scope_exit([mtab]() noexcept { endmntent(mtab); });
        struct mntent m
        {
        };
        char buffer[32768];
        while(getmntent_r(mtab, &m, buffer, sizeof(buffer)) != nullptr)
        {
          struct statfs64 temp
          {
          };
          memset(&temp, 0, sizeof(temp));
          // std::cout << m.mnt_fsname << "," << m.mnt_dir << "," << m.mnt_type << "," << m.mnt_opts << std::endl;
          if(0 == statfs64(m.mnt_dir, &temp))
          {
            // std::cout << "   " << temp.f_fsid.__val[0] << temp.f_fsid.__val[1] << " =? " << s.f_fsid.__val[0] << s.f_fsid.__val[1] << std::endl;
            if(temp.f_type == s.f_type && (memcmp(&temp.f_fsid, &s.f_fsid, sizeof(s.f_fsid)) == 0))
            {
              mountentries.emplace_back(mountentry(m.mnt_fsname, m.mnt_dir, m.mnt_type, m.mnt_opts), temp);
            }
          }
        }
      }
#ifndef LLFIO_COMPILING_FOR_GCOV
      if(mountentries.empty())
      {
        return errc::no_such_file_or_directory;
      }
      /* Choose the mount entry with the most closely matching statfs. You can't choose
      exclusively based on mount point because of bind mounts. Note that we have a
      particular problem in Docker:

      rootfs    /          rootfs  rw                       0 0
      overlay   /          overlay rw,relatime,lowerdir=... 0 0
      /dev/sda3 /etc       xfs     rw,relatime,...          0 0
      tmpfs     /dev       tmpfs   rw,nosuid,...            0 0
      tmpfs     /proc/acpi tmpfs   rw,nosuid,...            0 0
      ...

      If f_type and f_fsid are identical for the statfs for the mount as for our file,
      then you will get multiple mountentries, and there is no obvious way of
      disambiguating them. What we do is match mount based on the longest match
      of the mount point with the current path of our file.
      */
      if(mountentries.size() > 1)
      {
        OUTCOME_TRY(auto currentfilepath_, h.current_path());
        string_view currentfilepath(currentfilepath_.native());
        std::vector<std::pair<size_t, size_t>> scores(mountentries.size());
        //std::cout << "*** For matching mount entries to file with path " << currentfilepath << ":\n";
        for(size_t n = 0; n < mountentries.size(); n++)
        {
          scores[n].first =
          (currentfilepath.substr(0, mountentries[n].first.mnt_dir.size()) == mountentries[n].first.mnt_dir) ? mountentries[n].first.mnt_dir.size() : 0;
          //std::cout << "***    Mount entry " << mountentries[n].first.mnt_dir << " get score " << scores[n].first << std::endl;
          scores[n].second = n;
        }
        std::sort(scores.begin(), scores.end());
        // Choose the item whose mnt_dir most matched the current path for our file.
        auto temp(std::move(mountentries[scores.back().second]));
        mountentries.clear();
        mountentries.push_back(std::move(temp));
      }
#endif
      if(!!(wanted & want::flags))
      {
        f_flags.rdonly = static_cast<uint32_t>((s.f_flags & MS_RDONLY) != 0);
        f_flags.noexec = static_cast<uint32_t>((s.f_flags & MS_NOEXEC) != 0);
        f_flags.nosuid = static_cast<uint32_t>((s.f_flags & MS_NOSUID) != 0);
        f_flags.acls = static_cast<uint32_t>(std::string::npos != mountentries.front().first.mnt_opts.find("acl") &&
                                             std::string::npos == mountentries.front().first.mnt_opts.find("noacl"));
        f_flags.xattr = static_cast<uint32_t>(std::string::npos != mountentries.front().first.mnt_opts.find("xattr") &&
                                              std::string::npos == mountentries.front().first.mnt_opts.find("nouser_xattr"));
        //                out.f_flags.compression=0;
        // Those filing systems supporting FALLOC_FL_PUNCH_HOLE
        f_flags.extents = static_cast<uint32_t>(mountentries.front().first.mnt_type == "btrfs" || mountentries.front().first.mnt_type == "ext4" ||
                                                mountentries.front().first.mnt_type == "xfs" || mountentries.front().first.mnt_type == "tmpfs");
        ++ret;
      }
      if(!!(wanted & want::fstypename))
      {
        f_fstypename = mountentries.front().first.mnt_type;
        ++ret;
      }
      if(!!(wanted & want::mntfromname))
      {
        f_mntfromname = mountentries.front().first.mnt_fsname;
        ++ret;
      }
      if(!!(wanted & want::mntonname))
      {
        f_mntonname = mountentries.front().first.mnt_dir;
        ++ret;
      }
    }
    catch(...)
    {
      return error_from_exception();
    }
  }
#else
  struct statfs s;
  if(-1 == fstatfs(h.native_handle().fd, &s))
    return posix_error();
  if(!!(wanted & want::flags))
  {
    f_flags.rdonly = !!(s.f_flags & MNT_RDONLY);
    f_flags.noexec = !!(s.f_flags & MNT_NOEXEC);
    f_flags.nosuid = !!(s.f_flags & MNT_NOSUID);
    f_flags.acls = 0;
#if defined(MNT_ACLS) && defined(MNT_NFS4ACLS)
    f_flags.acls = !!(s.f_flags & (MNT_ACLS | MNT_NFS4ACLS));
#endif
    f_flags.xattr = 1;  // UFS and ZFS support xattr. TODO FIXME actually calculate this, zfs get xattr <f_mntfromname> would do it.
    f_flags.compression = !strcmp(s.f_fstypename, "zfs");
    f_flags.extents = !strcmp(s.f_fstypename, "ufs") || !strcmp(s.f_fstypename, "zfs");
    ++ret;
  }
  if(!!(wanted & want::bsize))
  {
    f_bsize = s.f_bsize;
    ++ret;
  }
  if(!!(wanted & want::iosize))
  {
    f_iosize = s.f_iosize;
    ++ret;
  }
  if(!!(wanted & want::blocks))
  {
    f_blocks = s.f_blocks;
    ++ret;
  }
  if(!!(wanted & want::bfree))
  {
    f_bfree = s.f_bfree;
    ++ret;
  }
  if(!!(wanted & want::bavail))
  {
    f_bavail = s.f_bavail;
    ++ret;
  }
  if(!!(wanted & want::files))
  {
    f_files = s.f_files;
    ++ret;
  }
  if(!!(wanted & want::ffree))
  {
    f_ffree = s.f_ffree;
    ++ret;
  }
#ifdef __APPLE__
  if(!!(wanted & want::namemax))
  {
    f_namemax = 255;
    ++ret;
  }
#else
  if(!!(wanted & want::namemax))
  {
    f_namemax = s.f_namemax;
    ++ret;
  }
#endif
  if(!!(wanted & want::owner))
  {
    f_owner = s.f_owner;
    ++ret;
  }
  if(!!(wanted & want::fsid))
  {
    f_fsid[0] = (unsigned) s.f_fsid.val[0];
    f_fsid[1] = (unsigned) s.f_fsid.val[1];
    ++ret;
  }
  if(!!(wanted & want::fstypename))
  {
    f_fstypename = s.f_fstypename;
    ++ret;
  }
  if(!!(wanted & want::mntfromname))
  {
    f_mntfromname = s.f_mntfromname;
    ++ret;
  }
  if(!!(wanted & want::mntonname))
  {
    f_mntonname = s.f_mntonname;
    ++ret;
  }
#endif
  return ret;
}

LLFIO_V2_NAMESPACE_END
