#include "global.h"
#include "log.h"

long getCurrentTime(){      // 返回调用时间
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

FILE* getEventlog(){    // 返回将要写入的日志文件
    char hostname[8];
    gethostname(hostname, 8);
    if (strcmp(hostname,"server")==0) return server_event_log;
    else if (strcmp(hostname,"client")==0) return client_event_log;
    else{
        printf("event日志函数: getEventlog获取hostneme失败\n");
        exit(-1);
    }
}

// 模块初始化
void init_log(){
    server_event_log=fopen("./server.event.trace","w");
    client_event_log=fopen("./client.event.trace","w");
}

// 关闭模块
void close_log(){
    fclose(server_event_log);
    fclose(client_event_log);
}
