//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.

/// @brief
/// @param n fd 位于 n-th param
/// @param pfd (return) fd
/// @param pf (return)
/// @return
static int argfd(int n, int *pfd, struct file **pf) {
  int fd;
  if (argint(n, &fd) < 0) return -1;
  struct file *f = myproc()->ofile[fd];  // 打开文件列表
  if (fd < 0 || fd >= NOFILE || f == 0) return -1;
  if (pfd) *pfd = fd;
  if (pf) *pf = f;
  return 0;
}

/// @brief Allocate a file descriptor for the given file.
/// Takes over file reference from caller on success.
/// @param f
/// @return
static int fdalloc(struct file *f) {  // alloc a fd
  for (int fd = 0; fd < NOFILE; fd++) {
    struct proc *p = myproc();
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64 sys_dup(void) {
  struct file *f;
  if (argfd(0, 0, &f) < 0) return -1;
  int fd = fdalloc(f);
  if (fd < 0) return -1;
  filedup(f);
  return fd;
}

// ssize_t read(int fd, void buf[.count], size_t count);
uint64 sys_read(void) {
  struct file *f;
  uint64 p;
  int n;
  if (argfd(0, 0, &f) < 0 || argaddr(1, &p) < 0 || argint(2, &n) < 0) return -1;
  return fileread(f, p, n);
}

uint64 sys_write(void) {
  struct file *f;
  uint64 p;
  int n;
  if (argfd(0, 0, &f) < 0 || argaddr(1, &p) < 0 || argint(2, &n) < 0) return -1;
  return filewrite(f, p, n);
}

// int close(int fd);
uint64 sys_close(void) {
  int fd;
  struct file *f;
  if (argfd(0, &fd, &f) < 0) return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

// int fstat(int fd, struct stat *statbuf);
uint64 sys_fstat(void) {
  struct file *f;
  uint64 st;  // user pointer to struct stat
  if (argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0) return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
// int link(const char *oldpath, const char *newpath);
uint64 sys_link(void) {
  char new[MAXPATH], old[MAXPATH];
  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0) return -1;

  struct inode *ip;

  begin_op();
  if ((ip = namei(old)) == 0) {  // failed
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {  // 硬链接不能是 dir
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  char name[DIRSIZ];
  struct inode *dp;                          // directory of parent
  if ((dp = nameiparent(new, name)) == 0) {  // failed
    ilock(ip);
    ip->nlink--;  // 状态回退
    iupdate(ip);
    iunlockput(ip);
    end_op();
    return -1;
  };
  ilock(dp);
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
    iunlockput(dp);
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;
}

/// @brief 判断: directory 是否空
/// @param dp
/// @return bool
static int isdirempty(struct inode *dp) {
  for (int off = 2 * sizeof(struct dirent); off < dp->size; off += sizeof(struct dirent)) {
    struct dirent de;
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) panic("isdirempty: readi");
    if (de.inum != 0) return 0;
  }
  return 1;
}

// int unlink(const char *pathname);
uint64 sys_unlink(void) {
  char path[MAXPATH];

  if (argstr(0, path, MAXPATH) < 0) return -1;

  struct inode *dp;
  char name[DIRSIZ];

  begin_op();
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0) {
    iunlockput(dp);
    end_op();
    return -1;
  }

  struct inode *ip;
  uint off;
  if ((ip = dirlookup(dp, name, &off)) == 0) {  // 找不到 ?
    iunlockput(dp);
    end_op();
    return -1;
  }
  ilock(ip);

  if (ip->nlink < 1) panic("unlink: nlink < 1");
  if (ip->type == T_DIR && !isdirempty(ip)) {  // 是 dir 并且非空
    iunlockput(ip);
    iunlockput(dp);
    end_op();
    return -1;
  }

  struct dirent de;
  kmemset(&de, 0, sizeof(struct dirent));
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) panic("unlink: writei");
  if (ip->type == T_DIR) {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;
}

/// @brief
/// @param path
/// @param type
/// @param major
/// @param minor
/// @return
static struct inode *create(char *path, short type, short major, short minor) {
  char name[DIRSIZ];
  struct inode *dp;  // directory of parent
  if ((dp = nameiparent(path, name)) == 0) return 0;

  ilock(dp);

  struct inode *ip = dirlookup(dp, name, 0);
  if (ip != 0) {  // 说明已经存在了, 再次创建就有问题
    iunlockput(dp);
    ilock(ip);
    // 如果要 create 的文件是 T_FILE, 并且 path 对应的类型是 T_FILE/F_DEVICE, 那么就返回
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE)) return ip;
    iunlockput(ip);
    return 0;
  }

  ip = ialloc(dp->dev, type);
  if (ip == 0) panic("create: ialloc");  // 分配失败

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR) {  // Create . and .. entries.
    dp->nlink++;        // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0) panic("create dots");
  }

  // directory 写一条记录
  if (dirlink(dp, name, ip->inum) < 0) panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

// int open(const char * path, int flags);
uint64 sys_open(void) {
  char path[MAXPATH];
  int n = argstr(0, path, MAXPATH);

  int omode;
  if (n < 0 || argint(1, &omode) < 0) return -1;

  begin_op();

  struct inode *ip;
  if (omode & O_CREATE) {  // 不存在就 create
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {  // 创建文件失败
      end_op();
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {  // 没有找到
      end_op();
      return -1;
    }
    ilock(ip);
    if (ip->type == T_DIR && omode != O_RDONLY) {  // 不是 read only, 而且还是 dir
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {  // 打开的是 device, 并且编号不在有效的范围内
    iunlockput(ip);
    end_op();
    return -1;
  }

  int fd;
  struct file *f;
  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {  // filealloc 失败 || fdalloc 失败
    if (f) fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

// int mkdir(const char *path);
uint64 sys_mkdir(void) {
  char path[MAXPATH];
  struct inode *ip;
  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

// int mknod(const char *path, int major, int minor);
uint64 sys_mknod(void) {
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if ((argstr(0, path, MAXPATH)) < 0 || argint(1, &major) < 0 || argint(2, &minor) < 0 || (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

// int chdir(const char * path);
uint64 sys_chdir(void) {
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

// int exec(char* path, char* argv[]);
uint64 sys_exec(void) {
  char path[MAXPATH];
  char *argv[MAXARG];  // 可以看到, 这里定义了参数的长度
  uint64 uargv;

  if (argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0) {
    return -1;
  }
  kmemset(argv, 0, sizeof(argv));
  for (int i = 0;; i++) {
    if (i >= NELEM(argv)) {                                              // 参数超过长度
      for (i = 0; i < NELEM(argv) && argv[i] != 0; i++) kfree(argv[i]);  // 注意, 下面有 kalloc
      return -1;
    }
    uint64 straddr;
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&straddr) < 0) {  // 没有正确获取参数
      for (i = 0; i < NELEM(argv) && argv[i] != 0; i++) kfree(argv[i]);
      return -1;
    }
    if (straddr == 0) {  // for exit here
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0) {  // 分配失败
      for (i = 0; i < NELEM(argv) && argv[i] != 0; i++) kfree(argv[i]);
      return -1;
    };
    if (fetchstr(straddr, argv[i], PGSIZE) < 0) {
      for (i = 0; i < NELEM(argv) && argv[i] != 0; i++) kfree(argv[i]);
      return -1;
    };
  }

  int ret = kexec(path, argv);

  for (int i = 0; i < NELEM(argv) && argv[i] != 0; i++) kfree(argv[i]);

  return ret;
}

// int pipe(int fdarray[2]);
uint64 sys_pipe(void) {
  uint64 fdarray;  // user pointer to array of two integers
  if (argaddr(0, &fdarray) < 0) return -1;
  struct file *rf, *wf;
  if (pipealloc(&rf, &wf) < 0) return -1;

  int fd0 = -1;
  int fd1;
  struct proc *p = myproc();
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0) p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  // 返回的句柄是: 用户空间下的
  if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 || copyout(p->pagetable, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0) {
    // clean up
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
