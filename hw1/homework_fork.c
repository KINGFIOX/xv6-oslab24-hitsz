#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
  pid_t pid;

  pid = fork();
  if (pid < 0) {
    perror("fork");
  } else if (pid == 0) {
    sleep(5);
    exit(5);
  } else {
    pid = fork();
    if (pid < 0) {
      perror("fork");
    } else if (pid == 0) {
      sleep(1);
      exit(1);
    } else {
      while (1) {
        int status;
        pid = wait(&status);
        if (pid == -1) {
          if (errno == ECHILD) {
            break;
          } else {
            perror("wait");
          }
        }
        printf("Child %d terminated with exit status %d\n", pid, WEXITSTATUS(status));
      }
    }
  }
}
