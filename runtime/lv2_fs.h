// File system emulation shared by sys_fs syscalls and the cellFs HLE functions.
#pragma once
#include "kernel.h"

namespace fs {
s32 open(const std::string& path, s32 flags, u32 fd_out);
s32 read(u32 fd, u32 buf, u64 size, u32 nread_out);
s32 write(u32 fd, u32 buf, u64 size, u32 nwrite_out);
s32 close(u32 fd);
s32 lseek(u32 fd, s64 offset, u32 whence, u32 pos_out);
s32 stat(const std::string& path, u32 stat_out);
s32 fstat(u32 fd, u32 stat_out);
s32 opendir(const std::string& path, u32 fd_out);
s32 readdir(u32 fd, u32 dirent_out, u32 nread_out);
s32 closedir(u32 fd);
s32 mkdir(const std::string& path);
s32 rename(const std::string& from, const std::string& to);
s32 rmdir(const std::string& path);
s32 unlink(const std::string& path);
s32 truncate(const std::string& path, u64 size);
s32 ftruncate(u32 fd, u64 size);
}  // namespace fs
