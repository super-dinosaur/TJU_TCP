#ifndef TIME_H
#define TIME_H

#include "global.h"
#include "log.h"
#include "tju_packet.h"
#include "kernel.h"

// 用于在发送缓冲区发送数据的线程
void* sending_thread(void* arg);
// 用于进行超时重传的线程
void* retrans_thread(void* arg);

// 计时器函数
void startTimer(tju_tcp_t *sock);   // 开启计时器
void stopTimer(void);               // 关闭计时器
void timeout_handler(int signo);    // 超时处理函数

void CalTimeout(tju_tcp_t *sock);   // 计算 SampleRTT 


#endif
