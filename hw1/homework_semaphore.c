#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
  pthread_mutex_t mtx;
  int cnt;
} sem_t;

/**
 * @brief
 *
 * @param sem (return)
 * @param value
 * @return int
 */
int sem_init(sem_t **sem, int value) {
  sem_t *sem_p = malloc(sizeof(sem_t));
  if (pthread_mutex_init(&sem_p->mtx, NULL) != 0) {
    perror("pthread_mutex_init");
    return -1;
  }
  sem_p->cnt = value;
  *sem = sem_p;
  return 0;
}

int sem_p(sem_t *sem) {
  if (sem == NULL) {
    errno = EINVAL;
    perror("sem_p: sem is NULL");
    return -1;
  }

  while (1) {
    if (pthread_mutex_lock(&sem->mtx) != 0) {
      perror("pthread_mutex_lock");
      return -1;
    }

    if (sem->cnt > 0) {
      sem->cnt--;
      if (pthread_mutex_unlock(&sem->mtx) != 0) {
        perror("pthread_mutex_unlock");
        return -1;
      }
      break;
    }

    if (pthread_mutex_unlock(&sem->mtx) != 0) {
      perror("pthread_mutex_unlock");
      return -1;
    }

    usleep(1000);
  }

  return 0;
}

int sem_v(sem_t *sem) {
  if (pthread_mutex_lock(&sem->mtx) != 0) {
    perror("pthread_mutex_lock");
    return -1;
  }
  sem->cnt++;
  if (pthread_mutex_unlock(&sem->mtx) != 0) {
    perror("pthread_mutex_unlock");
    return -1;
  }
  return 0;
}

int sem_destroy(sem_t *sem) {
  if (pthread_mutex_destroy(&sem->mtx) != 0) {
    perror("pthread_mutex_destroy");
    return -1;
  }
  free(sem);
  return 0;
}

sem_t *g_sem;

#define RESOURCE_COUNT 3
#define THREAD_COUNT 10

void *thread_func(void *arg) {
  int thread_num = *(int *)arg;
  printf("thread %d trying to acquire resource...\n", thread_num);

  sem_p(g_sem);

  printf("thread %d has gotten resource! sem->cnt=%d\n", thread_num, g_sem->cnt);

  sleep(rand() % 3 + 1);

  printf("thread %d release resource.\n", thread_num);
  sem_v(g_sem);

  free(arg);
  return NULL;
}

int main() {
  pthread_t threads[THREAD_COUNT];
  int res;

  res = sem_init(&g_sem, RESOURCE_COUNT);
  if (res != 0) {
    perror("init failed");
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < THREAD_COUNT; i++) {
    int *thread_num = malloc(sizeof(int));
    if (thread_num == NULL) {
      perror("malloc failed");
      exit(EXIT_FAILURE);
    }
    *thread_num = i + 1;
    res = pthread_create(&threads[i], NULL, thread_func, thread_num);
    if (res != 0) {
      perror("pthread_create failed");
      exit(EXIT_FAILURE);
    }
  }

  for (int i = 0; i < THREAD_COUNT; i++) {
    pthread_join(threads[i], NULL);
  }

  sem_destroy(g_sem);

  printf("success!\n");
  return 0;
}
