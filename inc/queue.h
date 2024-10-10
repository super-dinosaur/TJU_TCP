#ifndef _QUEUE_H_
#define _QUEUE_H_


#include "global.h"

typedef struct myNode{
  void *data;
  struct myNode *next;
}myNode;

typedef struct myQueue{
  int size;
  struct myNode *head;
  struct myNode *tail;
  /* pthread_mutex_t q_lock; */
}myQueue;

myQueue* init_q();

int push_q(myQueue* q, void *data);

void *pop_q(myQueue* q);

#endif