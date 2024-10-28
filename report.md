# threads

## Uthread: switching between threads

### 内容分析

这个直接参考 kernel/swtch.S, kernel/proc.c:sched, kernel/proc.c:scheduler 就行了

### 代码实现

thread_init, 初始化线程, 那么需要初始化栈顶指针, 线程创建的时候的 pc, 然后进入一个线程, 实际上是从: thread_scheduler 返回的, 所以设置 pc, 也就是设置 ra.

```c
// user/uthread.c
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
```

保存的上下文的话, 因为我们是通过调用 yield, 主动的放弃 cpu, 对于调用一个函数, 我们只需要保存他的 callee-saved 就行了.

```c
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
```

接下来就是一个线程的 switch, switch 的调用时机就是在 thread_schedule, 这个可以参考 kernel/proc.c:scheduler

```c
void thread_scheduler(void) {
  // ...
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
  // ...
}
```

thread_switch 可以参考 kernel/swtch.S, 我这里几乎与 kernel/swtch.S 一模一样, 但是采用了 c inline asm 的写法

```c
__attribute__((naked)) void thread_switch(struct context *cur, struct context *next) {
  __asm__ volatile(
      "sd ra, 0(a0)\n"
      "sd sp, 8(a0)\n"
      "sd s0, 16(a0)\n"
      "sd s1, 24(a0)\n"
      "sd s2, 32(a0)\n"
      "sd s3, 40(a0)\n"
      "sd s4, 48(a0)\n"
      "sd s5, 56(a0)\n"
      "sd s6, 64(a0)\n"
      "sd s7, 72(a0)\n"
      "sd s8, 80(a0)\n"
      "sd s9, 88(a0)\n"
      "sd s10, 96(a0)\n"
      "sd s11, 104(a0)\n");

  __asm__ volatile(
      "ld ra, 0(a1)\n"
      "ld sp, 8(a1)\n"
      "ld s0, 16(a1)\n"
      "ld s1, 24(a1)\n"
      "ld s2, 32(a1)\n"
      "ld s3, 40(a1)\n"
      "ld s4, 48(a1)\n"
      "ld s5, 56(a1)\n"
      "ld s6, 64(a1)\n"
      "ld s7, 72(a1)\n"
      "ld s8, 80(a1)\n"
      "ld s9, 88(a1)\n"
      "ld s10, 96(a1)\n"
      "ld s11, 104(a1)\n");

  __asm__ volatile("ret");
}
```

## Using threads

### 内容分析

他是要并发的访问一个 hash 表, 这个 hash 表采用了: 链地址法.
我这里就直接让他每个 bucket 有一个 lock

### 代码实现

```c
pthread_mutex_t locks[NBUCKET];

static struct entry *get(int key) {
  int i = key % NBUCKET;
  struct entry *e = 0;
  pthread_mutex_lock(&locks[i]);
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key) break;
  }
  pthread_mutex_unlock(&locks[i]);

  return e;
}

static void put(int key, int value) {
  int i = key % NBUCKET;

  pthread_mutex_lock(&locks[i]);
  // is the key already present?
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key) break;
  }
  if (e) {
    // update the existing key.
    e->value = value;
  } else {
    // the new is new.
    insert(key, value, &table[i], table[i], i);
  }
  pthread_mutex_unlock(&locks[i]);
}
```

## Barrier

### 内容分析

这个实验就是考察同步

### 代码实现

```c
struct barrier {
  pthread_mutex_t barrier_mutex;
  pthread_cond_t barrier_cond;
  int nthread;  // Number of threads that have reached this round of the barrier
  int round;    // Barrier round
} bstate;

static void barrier_init(void) {
  assert(pthread_mutex_init(&bstate.barrier_mutex, NULL) == 0);
  assert(pthread_cond_init(&bstate.barrier_cond, NULL) == 0);
  bstate.nthread = 0;
}

static void barrier() {
  // YOUR CODE HERE
  //
  // Block until all threads have called barrier() and
  // then increment bstate.round.
  //
  pthread_mutex_lock(&bstate.barrier_mutex);
  if (++bstate.nthread == nthread) {
    bstate.round++;
    bstate.nthread = 0;
    pthread_cond_broadcast(&bstate.barrier_cond);
  } else {
    int current_round = bstate.round;
    if (current_round == bstate.round) {
      pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
    }
  }

  pthread_mutex_unlock(&bstate.barrier_mutex);
}
```
