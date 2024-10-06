#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char *base(const char *path) {
  const char *p;

  // Find first character after last slash.
  for (p = path + strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  return (char *)p;
}

static void close_cleanup(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    // fprintf(2, "call close cleanup\n");
  }
}

static bool __dfs(const char *cur_path, const char *name_to_find) {
  int fd __attribute__((cleanup(close_cleanup))) = -1;  // open a dir (raii)

  if ((fd = open(cur_path, 0)) < 0) {
    // fprintf(2, "find: cannot open %s\n", cur_path);
    return false;
  }

  struct stat st;
  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", cur_path);
    // close(fd);
    return false;
  }

  bool flag = false;
  if (strcmp(base(cur_path), name_to_find) == 0) {
    printf("%s\n", cur_path);
    flag = true;
  }

  // 叶子节点
  if (T_FILE == st.type || T_DEVICE == st.type) {
    // close(fd);  // 关闭文件描述符
    return flag;
  }

  // 非叶子节点

  char buf[512];
  strcpy(buf, cur_path);
  char *p = buf + strlen(buf);
  *p++ = '/';  // 添加斜杠

  struct dirent de;
  while (read(fd, &de, sizeof(de)) == sizeof(de)) {
    if (de.inum == 0) continue;

    char name[DIRSIZ + 1];
    memmove(name, de.name, DIRSIZ);
    name[DIRSIZ] = '\0';  // 确保字符串以'\0'结尾

    // 去除可能存在的多余空白字符
    char *trimmed_name = name;
    while (*trimmed_name == ' ' && *trimmed_name != '\0') {
      trimmed_name++;
    }

    if (strcmp(trimmed_name, ".") == 0 || strcmp(trimmed_name, "..") == 0) continue;

    // 检查路径长度是否过长
    if (strlen(buf) + strlen(trimmed_name) + 1 > sizeof(buf)) {
      fprintf(2, "find: path too long\n");
      continue;
    }

    strcpy(p, trimmed_name);  // 拼接路径

    // 递归调用
    flag |= __dfs(buf, name_to_find);
  }

  // close(fd);  // 在函数结束前关闭文件描述符
  return flag;
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("usage: find <path> <name>\n");
    exit(0);
  }
  char *path = argv[1];
  char *name = argv[2];

  bool flag = __dfs(path, name);
  if (flag == false) {
    fprintf(2, "find: cannot find %s in %s\n", name, path);
  }
  exit(0);
}
