#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define NULL ((void *)0)

#define N 5

pid_t pid[N];

void handle_sigusr1(int sig) {
  // do nothing
}

void homework_wait() {
  struct sigaction sa;
  sa.sa_handler = handle_sigusr1;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, NULL) == -1) {
    perror("sigaction");
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < N; i++) {
    if ((pid[i] = fork()) == 0) {
      pause();
      exit(100 + i);
    } else if (pid[i] < 0) {
      perror("fork");
      exit(EXIT_FAILURE);
    }
  }

  printf("hello!\n");

  for (int i = 0; i < N; i++) {
    if (kill(pid[i], SIGUSR1) == -1) {
      perror("kill");
      exit(EXIT_FAILURE);
    }

    int child_status;
    pid_t wpid = waitpid(pid[i], &child_status, 0);
    if (wpid == -1) {
      perror("waitpid");
      exit(EXIT_FAILURE);
    }

    if (WIFEXITED(child_status)) {
      printf("Child %d terminated with exit status %d\n", wpid, WEXITSTATUS(child_status));
    } else {
      printf("Child %d terminated abnormally\n", wpid);
    }
  }
}

int main() {
  homework_wait();
  return 0;
}