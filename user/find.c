#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void ls(char *path);

/// @brief return the base name of the path, lifetime from path or static buf
/// @param path
/// @return
char *base_name(const char *path) {
  static char buf[DIRSIZ + 1];
  const char *p;

  // Find first character after last slash.
  for (p = path + strlen(path); p >= path && *p != '/'; p--);
  p++;

  // Return blank-padded name.
  if (strlen(p) >= DIRSIZ) return (char *)p;
  memmove(buf, p, strlen(p));
  memset(buf + strlen(p), ' ', DIRSIZ - strlen(p));
  return buf;
}

/// @brief
/// @param cur_st
/// @param cur_path
/// @param name the name to find
/// @return true if found
static bool __dfs(const struct stat *cur_st, const char *cur_path, const char *name) {
  bool flag = false;

  if (cur_st->type == T_FILE || cur_st->type == T_DEVICE) {
    if (strcmp(base_name(cur_path), name) == 0) {
      printf("%s\n", cur_path);
      return true;
    }
  } else {
    char *base = base_name(cur_path);
    if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
      return false;
    }
    char buf[512], *p;
    strcpy(buf, cur_path);
    p = buf + strlen(buf);
    *p++ = '/';
    int fd;
    if ((fd = open(cur_path, 0)) < 0) {
      fprintf(2, "find: cannot open %s\n", cur_path);
      return false;
    }
    if (fstat(fd, cur_st) < 0) {
      fprintf(2, "find: cannot stat %s\n", cur_path);
      close(fd);
      return false;
    }
    struct dirent de;
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      if (de.inum == 0) continue;
      memmove(p, de.name, DIRSIZ);  // buf += de.name
      p[DIRSIZ] = 0;
      struct stat st;
      if (stat(buf, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", buf);
        continue;
      }
      if (__dfs(&st, buf, name)) {
        flag = true;
      }
    }
  }

  return flag;
}

void find(const char *path, const char *name) {
  struct stat st;
  if (stat(path, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    return;
  }
  __dfs(&st, path, name);
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("usage: find <path> <name>\n");
    exit(0);
  }
  char *path = argv[1];
  char *name = argv[2];

  find(path, name);
  exit(0);
}