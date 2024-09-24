#include "kernel/types.h"
#include "user.h"

int main(int argc, char *argv[]) {
  if (argc != 1) {
    fprintf(2, "pingpong receive non arg\n");
    exit(1);
  }

  // [0] read, [1] write
  int c2f[2], f2c[2];
  pipe(c2f);
  pipe(f2c);

  char send_buf[4] = {0};
  char recv_buf[4] = {0};

  int pid;

  int _pid = fork();
  switch (_pid) {
    case -1:
      fprintf(2, "fatal: fork: %s:%d\n", __FILE__, __LINE__);
      /* code */
      break;

    case 0:           // child
      close(c2f[0]);  // close c2f read
      close(f2c[1]);  // close f2c write

      pid = getpid();

      read(f2c[0], recv_buf, 2);
      close(f2c[0]);
      printf("%d: received ping from pid %s\n", pid, recv_buf);

      itoa(pid, send_buf);
      write(c2f[1], send_buf, 2);
      close(c2f[1]);

      break;

    default:          // father
      close(c2f[1]);  // close c2f write
      close(f2c[0]);  // close f2c read

      pid = getpid();

      itoa(pid, send_buf);
      write(f2c[1], send_buf, 2);
      close(f2c[1]);

      read(c2f[0], recv_buf, 2);
      close(c2f[0]);
      printf("%d: received pong from pid %s\n", pid, recv_buf);

      break;
  }

  exit(0);
}