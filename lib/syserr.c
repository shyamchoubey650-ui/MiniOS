#include "include/syscall.h"

const char *sys_errname(int c) {
    switch (c) {
        case SYS_OK: return "OK";           case SYS_ENOENT: return "ENOENT";     case SYS_EEXIST: return "EEXIST";
        case SYS_ENOTDIR: return "ENOTDIR"; case SYS_EISDIR: return "EISDIR";     case SYS_ENOTEMPTY: return "ENOTEMPTY";
        case SYS_EINVAL: return "EINVAL";   case SYS_ENAMETOOLONG: return "ENAMETOOLONG"; case SYS_EPATHTOOLONG: return "EPATHTOOLONG";
        case SYS_ENOSPC: return "ENOSPC";   case SYS_EFBIG: return "EFBIG";       case SYS_EACCES: return "EACCES";
        case SYS_EBUSY: return "EBUSY";     case SYS_EINTOSELF: return "EINTOSELF"; case SYS_ESTALE: return "ESTALE";
        case SYS_EBADF: return "EBADF";     case SYS_EMFILE: return "EMFILE";     case SYS_EFAULT: return "EFAULT";
        case SYS_ENOSYS: return "ENOSYS";   case SYS_ESRCH: return "ESRCH";       case SYS_ECHILD: return "ECHILD";
        case SYS_EINTR: return "EINTR";     case SYS_ENOMEM: return "ENOMEM";     case SYS_EPERM: return "EPERM";
        case SYS_EAGAIN: return "EAGAIN";   case SYS_ELIMIT: return "ELIMIT";     case SYS_ESTATE: return "ESTATE";
        case SYS_EAUTH: return "EAUTH";     case SYS_ELOCKED: return "ELOCKED";   case SYS_ENOUSER: return "ENOUSER";
        case SYS_EFLOCK: return "EFLOCK"; case SYS_EPROTECT: return "EPROTECT"; case SYS_EBLOCK: return "EBLOCK";
        default: return "?";
    }
}

const char *sys_strerror(int c) {
    switch (c) {
        case SYS_OK:           return "success";
        case SYS_ENOENT:       return "no such file or directory";
        case SYS_EEXIST:       return "already exists";
        case SYS_ENOTDIR:      return "not a directory";
        case SYS_EISDIR:       return "is a directory";
        case SYS_ENOTEMPTY:    return "directory not empty";
        case SYS_EINVAL:       return "invalid argument";
        case SYS_ENAMETOOLONG: return "name too long";
        case SYS_EPATHTOOLONG: return "path too long or nested too deeply";
        case SYS_ENOSPC:       return "file system full (node limit or out of memory)";
        case SYS_EFBIG:        return "file too large";
        case SYS_EACCES:       return "permission denied";
        case SYS_EBUSY:        return "busy (root or contains the working directory)";
        case SYS_EINTOSELF:    return "cannot move or copy a directory into itself";
        case SYS_ESTALE:       return "stale descriptor: the file was removed or replaced";
        case SYS_EBADF:        return "bad file descriptor";
        case SYS_EMFILE:       return "too many open files";
        case SYS_EFAULT:       return "bad address";
        case SYS_ENOSYS:       return "unknown system call";
        case SYS_ESRCH:        return "no such process";
        case SYS_ECHILD:       return "not a child process, or no children";
        case SYS_EINTR:        return "interrupted";
        case SYS_ENOMEM:       return "out of kernel memory";
        case SYS_EPERM:        return "operation not permitted on this process";
        case SYS_EAGAIN:       return "process table is full";
        case SYS_ELIMIT:       return "per-process limit reached";
        case SYS_ESTATE:       return "process is not in a suitable state";
        case SYS_EAUTH:        return "authentication failed";
        case SYS_ELOCKED:      return "account is locked";
        case SYS_ENOUSER:      return "no such user (or nobody is logged in)";
        case SYS_EFLOCK:       return "file is locked by another user (ask them to unlock it)";
        case SYS_EPROTECT:     return "protected: unprotect it first";
        case SYS_EBLOCK:       return "bad block number, or no RAM-disk";
        default:               return "unknown error";
    }
}
