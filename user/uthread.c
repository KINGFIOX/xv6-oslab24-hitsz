#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// /* Possible states of a thread: */
// #define FREE 0x0
// #define RUNNING 0x1
// #define RUNNABLE 0x2

enum state { FREE, RUNNING, READY };

#define STACK_SIZE 8192
#define MAX_THREAD 4

struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

struct thread {
  char stack[STACK_SIZE]; /* the thread's stack */
  enum state state;       /* FREE, RUNNING, RUNNABLE */
  struct context ctx;
};
struct thread all_thread[MAX_THREAD];
struct thread *current_thread;
// extern void thread_switch(uint64, uint64);
extern void thread_switch(struct context *cur, struct context *next);

void thread_init(void) {
  // main() is thread 0, which will make the first invocation to
  // thread_schedule(). It needs a stack so that the first thread_switch() can
  // save thread 0's state.
  current_thread = &all_thread[0];
  current_thread->state = RUNNING;
}

void thread_schedule(void) {
  /* Find another runnable thread. */
  struct thread *next_thread = 0;
  struct thread *t = current_thread + 1;
  for (int i = 0; i < MAX_THREAD; i++) {
    if (t >= all_thread + MAX_THREAD) t = all_thread;  // 循环了
    if (t->state == READY) {
      next_thread = t;
      break;
    }
    t = t + 1;
  }

  if (next_thread == 0) {
    printf("thread_schedule: no runnable threads\n");
    exit(-1);
  }

  if (current_thread != next_thread) { /* switch threads?  */
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    /* YOUR CODE HERE
     * Invoke thread_switch to switch from t to next_thread:
     * thread_switch(??, ??);
     */
    thread_switch(&t->ctx, &current_thread->ctx);
  } else {
    next_thread = 0;
  }
}

void thread_create(void (*func)()) {
  struct thread *t;
  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == FREE) break;
  }
  t->state = READY;
  // YOUR CODE HERE
  t->ctx.sp = (uint64)t->stack + STACK_SIZE;
  t->ctx.ra = (uint64)func;
}

void thread_yield(void) {
  current_thread->state = READY;
  thread_schedule();
}

volatile int a_started, b_started, c_started;
volatile int a_n, b_n, c_n;

void thread_a(void) {
  printf("thread_a started\n");
  a_started = 1;
  while (b_started == 0 || c_started == 0) thread_yield();

  for (int i = 0; i < 100; i++) {
    printf("thread_a %d\n", i);
    a_n += 1;
    thread_yield();
  }
  printf("thread_a: exit after %d\n", a_n);

  current_thread->state = FREE;
  thread_schedule();
}

void thread_b(void) {
  printf("thread_b started\n");
  b_started = 1;
  while (a_started == 0 || c_started == 0) thread_yield();

  for (int i = 0; i < 100; i++) {
    printf("thread_b %d\n", i);
    b_n += 1;
    thread_yield();
  }
  printf("thread_b: exit after %d\n", b_n);

  current_thread->state = FREE;
  thread_schedule();
}

void thread_c(void) {
  printf("thread_c started\n");
  c_started = 1;
  while (a_started == 0 || b_started == 0) thread_yield();

  for (int i = 0; i < 100; i++) {
    printf("thread_c %d\n", i);
    c_n += 1;
    thread_yield();
  }
  printf("thread_c: exit after %d\n", c_n);

  current_thread->state = FREE;
  thread_schedule();
}

int main(int argc, char *argv[]) {
  a_started = b_started = c_started = 0;
  a_n = b_n = c_n = 0;
  thread_init();
  thread_create(thread_a);
  thread_create(thread_b);
  thread_create(thread_c);
  current_thread->state = FREE;
  thread_schedule();
  printf("main: exit\n");
  exit(0);
}
