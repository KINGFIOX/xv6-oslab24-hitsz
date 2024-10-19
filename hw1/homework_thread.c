#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NULL ((void *)0)

pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;  // mutex for stdout and I LIKE OS

int I = 0;
int LIKE = 0;
int OS = 0;

#define I_STR "I "
#define LIKE_STR "LIKE "
#define OS_STR "OS "

#define CONDITION                                                                                                                       \
  ((strcmp(arg, I_STR) == 0 && (0 == I && 0 == LIKE && 0 == OS)) || (strcmp(arg, LIKE_STR) == 0 && (1 == I && 0 == LIKE && 0 == OS)) || \
   (strcmp(arg, OS_STR) == 0 && (1 == I && 1 == LIKE && 0 == OS)))

void *mythread(void *arg) {
  pthread_mutex_lock(&mtx);
  while (!CONDITION) {
    pthread_cond_wait(&cv, &mtx);
  }
  assert(CONDITION);

  if (strcmp(arg, I_STR) == 0) I = 1;
  if (strcmp(arg, LIKE_STR) == 0) LIKE = 1;
  if (strcmp(arg, OS_STR) == 0) OS = 1;

  printf("%s\n", (char *)arg);

  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mtx);

  return NULL;
}

int main(int argc, char *argv[]) {
  pthread_t p1, p2, p3;
  int rc;
  printf("main: begin\n");
  rc = pthread_create(&p1, NULL, mythread, I_STR);
  assert(rc == 0);
  rc = pthread_create(&p2, NULL, mythread, LIKE_STR);
  assert(rc == 0);
  rc = pthread_create(&p3, NULL, mythread, OS_STR);
  assert(rc == 0);
  // join waits for the threads to finish
  rc = pthread_join(p1, NULL);
  assert(rc == 0);
  rc = pthread_join(p2, NULL);
  assert(rc == 0);
  rc = pthread_join(p3, NULL);
  assert(rc == 0);
  printf("\nmain: end\n");
  return 0;
}
