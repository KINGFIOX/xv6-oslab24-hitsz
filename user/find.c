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
/// @param cur_path
/// @param name the name to find
/// @return true if found
static bool __dfs(const char *cur_path, const char *name) {
  int fd;  // open a dir

  if ((fd = open(cur_path, 0)) < 0) {
    // fprintf(2, "find: cannot open %s\n", cur_path);
    return false;
  }

  struct stat st;
  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", cur_path);
    close(fd);
    return false;
  }

  bool flag = false;
  if (strcmp(base_name(cur_path), name) == 0) {
    printf("%s\n", cur_path);
    flag = true;
  }

  // 叶子节点
  if (st.type == T_FILE || st.type == T_DEVICE) {
    return flag;
  }

  // 非叶子节点

  char buf[512] = {0};
  strcpy(buf, cur_path);
  char *p = buf + strlen(buf);
  *p++ = '/';  // a/b/c/

  struct dirent de;
  while (read(fd, &de, sizeof(de)) == sizeof(de)) {
    if (de.inum == 0) continue;
    if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) continue;
    memmove(p, de.name, DIRSIZ);  // a/b/c/target
    p[DIRSIZ] = 0;
    if (__dfs(buf, name)) {
      flag = true;
    }
  }

  return flag;
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("usage: find <path> <name>\n");
    exit(0);
  }
  char *path = argv[1];
  char *name = argv[2];

  __dfs(path, name);
  exit(0);
}