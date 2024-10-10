#include "tju_tcp.h"


/*
创建 TCP socket 
初始化对应的结构体
设置初始状态为 CLOSED
*/

tju_tcp_t* tju_socket(){

    // 初始化event log
    init_log();

    // 确定超时处理函数
    signal(SIGALRM, timeout_handler);

    tju_tcp_t* sock = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;
    
    // 初始化发送缓冲区
    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = (char *)malloc(MAX_SOCK_BUF_SIZE);
    sock->sending_len = 0;
    sock->have_send_len=0;
    // 初始化接收缓冲区
    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = (char *)malloc(MAX_SOCK_BUF_SIZE);
    sock->received_len = 0;
    
    if(pthread_cond_init(&sock->wait_cond, NULL) != 0){
        perror("ERROR condition variable not set\n");
        exit(-1);
    }

    // 初始化发送窗口
    sock->window.wnd_send = (sender_window_t*)malloc(sizeof(sender_window_t));
    sock->window.wnd_send->window_size=MAX_DLEN;    // 1 个MSS
    sock->window.wnd_send->ack_cnt=0;
    sock->window.wnd_send->base=1;
    sock->window.wnd_send->nextseq=1;
    sock->window.wnd_send->same_ack_cnt=0;
    sock->window.wnd_send->estmated_rtt=TCP_RTO_MIN;
    sock->window.wnd_send->dev_rtt=0;
    sock->window.wnd_send->is_estimating_rtt=FALSE;
    sock->window.wnd_send->rtt_expect_ack=0;  // (sending_thread线程中更新)
    // sock->window.wnd_send->send_time
    sock->window.wnd_send->timeout.it_value.tv_sec = 0;
    sock->window.wnd_send->timeout.it_value.tv_usec = 500000;
    sock->window.wnd_send->timeout.it_interval.tv_sec = 0;
    sock->window.wnd_send->timeout.it_interval.tv_usec = 0;
    sock->window.wnd_send->rwnd=MAX_RWINDOW_SIZE;
    sock->window.wnd_send->window_status=SLOW_START;
    sock->window.wnd_send->cwnd=MAX_DLEN;
    sock->window.wnd_send->ssthresh=MAX_SWINDOW_SIZE>>1;

    // 初始化接收窗口
    sock->window.wnd_recv = (receiver_window_t *)malloc(sizeof(receiver_window_t));
    sock->window.wnd_recv->remain_size=MAX_RWINDOW_SIZE;
    sock->window.wnd_recv->recv_buf=(char *)malloc(MAX_RWINDOW_SIZE);
    sock->window.wnd_recv->mark=(uint8_t *)malloc(MAX_RWINDOW_SIZE);
    memset(sock->window.wnd_recv->mark,0,MAX_RWINDOW_SIZE);
    sock->window.wnd_recv->expect_seq=1;

    sock->is_retransing=FALSE;

    sock->seq = 0;
    sock->ack = 0;

    // sock->half_queue = init_q();
    // sock->full_queue = init_q();

    return sock;
}

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t* sock, tju_sock_addr bind_addr){
    if(bind_port[bind_addr.port] != 0){
        perror("ERROR port already in use\n");
        exit(-1);
    }
    sock->bind_addr = bind_addr;
    bind_port[bind_addr.port] = 1;
    return 0;
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t* sock){
    sock->state = LISTEN;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    listen_socks[hashval] = sock;
    sock->half_queue = init_q();
    sock->full_queue = init_q();

    return 0;
}

/*
接受连接 
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t* tju_accept(tju_tcp_t* listen_sock){
    while(!listen_sock->full_queue->size);
    printf("全连接队列中有新的连接\n");
    tju_tcp_t * new_conn = pop_q(listen_sock->full_queue);

    tju_sock_addr local_addr, remote_addr;
    local_addr = new_conn->established_local_addr;
    remote_addr = new_conn->established_remote_addr;
    
    // pthread_mutex_init(&(new_conn->recv_lock), NULL); //初始化接受区的互斥锁（unlock）

    // 将新的conn放到内核建立连接的socket哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, remote_addr.ip, remote_addr.port);
    established_socks[hashval] = new_conn;

    // 创建发送线程
    pthread_t sending_thread_id=555;
    int rst1=pthread_create(&sending_thread_id,NULL,sending_thread,(void *)new_conn);
    if (rst1<0){
        printf("sending thread 创建失败\n");
        exit(-1);
    }
    printf("sending thread 创建成功\n");

    // 创建重传线程
    pthread_t retrans_thread_id=556;
    int rst2=pthread_create(&retrans_thread_id,NULL,retrans_thread,(void *)new_conn);
    if (rst2<0){
        printf("retrans thread 创建失败\n");
        exit(-1);
    }
    printf("retrans thread 创建成功\n");

    return new_conn;
}


/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t* sock, tju_sock_addr target_addr){
    printf("开始建立连接\n"); 
    sock->established_remote_addr = target_addr;

    tju_sock_addr local_addr;
    local_addr.ip = inet_network(CLIENT_IP);
    local_addr.port = 5678; // 连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr = local_addr;
 
    // int hashval = cal_hash(local_addr.ip, local_addr.port, 0, 0);
    // listen_socks[hashval] = sock;
    // 将建立了连接的socket放入内核 已建立连接哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, target_addr.ip, target_addr.port);
    established_socks[hashval] = sock;
    
    tju_packet_t* pkt = create_packet(
    sock->established_local_addr.port, sock->established_remote_addr.port,
    0, 0,
    DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
    SYN_FLAG_MASK, 1, 0, NULL, 0);
    printf("成功创建syn_pkt\n"); 
    char* pkt_buf = packet_to_buf(pkt);
    printf("成功创建syn_pkt_buf\n"); 
    sendToLayer3(pkt_buf, DEFAULT_HEADER_LEN);
    _SEND_LOG_(pkt_buf);
    clock_t timer = clock();
    printf("成功发送syn_pkt_buf\n"); 
    
    printf("成功释放syn_pkt_buf\n"); 
    // 第一次挥手的发送pkt
    sock->seq++;// 下一次自己发pkt时的seq+1
    sock->state = SYN_SENT;
    
    while(sock->state != ESTABLISHED){
        if ((clock()-timer)>=5000000){    //触发计时器--超时重传   (CLOCK_PER_SEC)
            sendToLayer3(pkt_buf,DEFAULT_HEADER_LEN);
            _SEND_LOG_(pkt_buf);
            printf("客户端重新发送SYN请求\n");
            timer=clock(); //重新开始计时
        }
    }
    free(pkt_buf);

    // 创建发送线程
    pthread_t sending_thread_id=557;
    int rst1=pthread_create(&sending_thread_id,NULL,sending_thread,(void *)sock);
    if (rst1<0){
        printf("sending thread 创建失败\n");
        exit(-1);
    }
    printf("sending thread 创建成功\n");

    // 创建重传线程
    pthread_t retrans_thread_id=558;
    int rst2=pthread_create(&retrans_thread_id,NULL,retrans_thread,(void *)sock);
    if (rst2<0){
        printf("retrans thread 创建失败\n");
        exit(-1);
    }
    printf("retrans thread 创建成功\n");

    // 将建立了连接的socket放入内核 已建立连接哈希表中
    hashval = cal_hash(local_addr.ip, local_addr.port, target_addr.ip, target_addr.port);
    established_socks[hashval] = sock;
    return 0;
}

int tju_send(tju_tcp_t* sock, const void *buffer, int len){
    char* tmp_buffer=(char *)buffer;    // 当前待拷贝数据指针
    int tmp_len=len;    // 剩余待拷贝数据长度
    while (MAX_SOCK_BUF_SIZE-sock->sending_len>0||tmp_len){
        if (MAX_SOCK_BUF_SIZE-sock->sending_len==0) continue;
        pthread_mutex_lock(&sock->send_lock);   // 加锁
        if (MAX_SOCK_BUF_SIZE-sock->sending_len>=tmp_len){  // 剩余缓冲区空间足够存放待发送数据
            // 拷贝数据至缓冲区
            memcpy(sock->sending_buf+sock->sending_len,tmp_buffer,tmp_len);
            sock->sending_len+=tmp_len;
            pthread_mutex_unlock(&sock->send_lock); // 解锁
            break;
        }
        else{   // 缓冲区空间不够存放待发送数据
            memcpy(sock->sending_buf+sock->sending_len,tmp_buffer,MAX_SOCK_BUF_SIZE-sock->sending_len);
            sock->sending_len=MAX_SOCK_BUF_SIZE;
            tmp_buffer+=MAX_SOCK_BUF_SIZE-sock->sending_len;
            tmp_len-=MAX_SOCK_BUF_SIZE-sock->sending_len;
            pthread_mutex_unlock(&sock->send_lock); // 解锁
        }
    }
    return 0;
}

int tju_recv(tju_tcp_t* sock, void *buffer, int len){
    // printf("调用tju_recv函数\n");
    while(sock->received_len<=0){
        // 阻塞
    }

    while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

    int read_len = 0;
    if (sock->received_len >= len){ // 从中读取len长度的数据
        read_len = len;
    }else{
        read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
    }

    memcpy(buffer, sock->received_buf, read_len);

    if(read_len < sock->received_len) { // 还剩下一些
        char* new_buf =(char *)malloc(MAX_SOCK_BUF_SIZE);
        memcpy(new_buf, sock->received_buf + read_len, sock->received_len - read_len);
        free(sock->received_buf);
        sock->received_len -= read_len;
        sock->received_buf = new_buf;
    }else{
        sock->received_len = 0;
    }
    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    // for (int i=0;i<100000;i++) ; // 降速用

    return read_len;
}

/* 服务端或者客户端在自己的sock下对接收到的pkt进行的处理 */
int tju_handle_packet(tju_tcp_t* sock, char* pkt){
    
    /* uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;

    // 把收到的数据放到接受缓冲区
    while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

    if(sock->received_buf == NULL){
        sock->received_buf = malloc(data_len);
    }else {
        sock->received_buf = realloc(sock->received_buf, sock->received_len + data_len);
    }
    memcpy(sock->received_buf + sock->received_len, pkt + DEFAULT_HEADER_LEN, data_len);
    sock->received_len += data_len;

    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    */
    _RECV_LOG_(pkt);
    uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;
    uint8_t flag = get_flags(pkt);
    uint32_t seq = get_seq(pkt);
    uint32_t ack = get_ack(pkt);
    uint16_t src_port = get_src(pkt);
    uint16_t dst_port = get_dst(pkt);
    uint16_t adv_wnd = get_advertised_window(pkt);
     switch(sock->state){
        case LISTEN: // server发送syn_ack前的状态，等待着client的syn
            if(flag == SYN_FLAG_MASK){ // 检查标志位为SYN
                printf("服务端收到了SYN\n");

                sock->state = SYN_RECV;  // server进入syn_recv状态
                sock->seq = 0; // 初始的seq设置为0
                tju_packet_t *pkt = create_packet(dst_port, src_port, sock->seq, seq + 1,
                                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, SYN_FLAG_MASK | ACK_FLAG_MASK,
                                            1, 0, NULL, 0);// 创建syn_ack包
                char* pkt_buf = packet_to_buf(pkt);
                sendToLayer3(pkt_buf, DEFAULT_HEADER_LEN); /* 发送syn_ack包 */
                _SEND_LOG_(pkt_buf)
                free(pkt_buf);
                sock->seq++;
                sock->ack = seq + 1;
                // 新建socket放入半连接队列
                tju_tcp_t *new_conn = tju_socket();
                new_conn->established_local_addr = sock->bind_addr;
                new_conn->established_remote_addr.port = src_port;
                new_conn->established_remote_addr.ip = inet_network(CLIENT_IP);
                new_conn->state = SYN_RECV;
                new_conn->seq = sock->seq;
                push_q(sock->half_queue, new_conn);// 放入半连接队列
            }
            break;

        case SYN_SENT: // client发送syn后，等待着server的syn_ack
            if (flag == (SYN_FLAG_MASK | ACK_FLAG_MASK)){// 检查标志位为syn_ack
                printf("客户端收到了SYN+ACK\n");
                sock->state = ESTABLISHED; // client进入建立连接状态
                tju_packet_t *pkt = create_packet(dst_port, src_port, sock->seq, seq + 1,
                                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK,
                                            1, 0, NULL, 0);// 创建ack包
                char* pkt_buf = packet_to_buf(pkt);
                sendToLayer3(pkt_buf, DEFAULT_HEADER_LEN); /* 发送ack包 */
                _SEND_LOG_(pkt_buf);
                free(pkt_buf);
                sock->seq++;
                sock->ack = seq + 1;
                
            }
            break;

        case SYN_RECV: // server发送syn_ack后，等待着client的ack
            if (flag == ACK_FLAG_MASK){// 检查标志位为ack
                printf("服务端收到了ACK\n");
                sock->state = ESTABLISHED;// server进入建立连接状态
                tju_tcp_t *new_conn = pop_q(sock->half_queue);
                new_conn->state = ESTABLISHED;
                new_conn->seq = sock->seq;
                push_q(sock->full_queue, new_conn);// 放入全连接队列
            }
            break;

        case LAST_ACK: // server发送fin_ack后，等待着client的ack
            if(flag == ACK_FLAG_MASK){// 检查标志位为ack
                
                sock->state = CLOSED;// server进入closed
            }
            break;
        // case CLOSE_WAIT: // server在发送完剩余数据后，发送fin并进入ack
        //     // 发送完数据后
        //     // tju_packet_t* pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
        //     //                                     sock->seq, seq + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
        //     //                                     FIN_FLAG_MASK | ACK_FLAG_MASK, 1, 0, NULL, 0);/* 创建fin包 */
        //     // char* pkt_buf = packet_to_buf(pkt);
        //     // sendToLayer3(pkt_buf, DEFAULT_HEADER_LEN); // 发送fin包  
        //     // free(pkt_buf);
        //     // sock->seq++;
        //     // sock->ack = seq + 1;                              
        //     sock->state = LAST_ACK;// server进入last_ack
        //     break; 
        case FIN_WAIT_1: 
            if((flag == FIN_FLAG_MASK | ACK_FLAG_MASK) || (flag == FIN_FLAG_MASK)){// 双方同时关闭，双方都收到了fin+ack或fin
                tju_packet_t* pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                                                sock->seq, seq + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                                ACK_FLAG_MASK, 1, 0, NULL, 0);/* 创建ack包 */
                char* pkt_buf = packet_to_buf(pkt);
                sendToLayer3(pkt_buf, DEFAULT_HEADER_LEN); // 发送ack包
                free(pkt_buf);
                _SEND_LOG_(pkt_buf);
                sock->seq++;
                sock->ack = seq + 1;
                sock->state = CLOSING;// 进入closing
            }
            else if(flag == ACK_FLAG_MASK){// 双方先后关闭，主动关闭方收到了被动关闭方发来的ack
                sock->state = FIN_WAIT_2;// 进入fin_wait_2
            }
            break;

        case FIN_WAIT_2:
            if(flag == FIN_FLAG_MASK | ACK_FLAG_MASK){// 进入fin_wait_2的关闭方收到了对方发来的fin_ack
                
                tju_packet_t* pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                                            sock->seq, sock->ack, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                            ACK_FLAG_MASK, 1, 0, NULL, 0);/* 创建ack包 */
                char* pkt_buf = packet_to_buf(pkt);
                sendToLayer3(pkt_buf, DEFAULT_HEADER_LEN); // 发送ack包 
                _SEND_LOG_(pkt_buf);
                free(pkt_buf); 
                sock->seq++;
                sock->state = TIME_WAIT;// 进入Time_wait
            }
            break;

        case CLOSING:
            if(flag == ACK_FLAG_MASK){// 双方同时关闭时收到了对方的ack
                sock->state = TIME_WAIT;// 进入time_wait
            }
            break;
            
        case ESTABLISHED: // server被动接受fin
            if(flag == FIN_FLAG_MASK){// 检查标志位为fin
                tju_packet_t* pkt_ack = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                                                sock->seq, seq + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                                ACK_FLAG_MASK, 1, 0, NULL, 0);/* 创建ack包 */
                char* pkt_ack_buf = packet_to_buf(pkt_ack);
                sendToLayer3(pkt_ack_buf, DEFAULT_HEADER_LEN);  // 发送ack包
                _SEND_LOG_(pkt_ack_buf);
                free(pkt_ack_buf);
                sock->seq++;
                sock->ack = seq + 1;
                sock->state = CLOSE_WAIT;// server进入close_wait
                sleep(1);// 等待1s
                tju_packet_t* pkt_finack = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                                                sock->seq, seq + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                                FIN_FLAG_MASK | ACK_FLAG_MASK, 1, 0, NULL, 0);/* 创建fin_ack包 */
                char* pkt_finack_buf = packet_to_buf(pkt_finack);
                sendToLayer3(pkt_finack_buf, DEFAULT_HEADER_LEN);  // 发送fin_ack包
                _SEND_LOG_(pkt_finack_buf);
                free(pkt_finack_buf);
                sock->seq++;
                sock->state = LAST_ACK; //被动关闭方进入last_ack
            }
            else if(flag == NO_FLAG) { // 接收方收到了发送方发来的数据报文
                if(seq < sock->window.wnd_recv->expect_seq) {// 收到的seq比期待的seq还小，直接发回ack包
                    //log
                    tju_packet_t* pkt_ack = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                                                sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                                ACK_FLAG_MASK, sock->window.wnd_recv->remain_size, 0, NULL, 0);/* 创建ack包 */
                    char* pkt_ack_buf = packet_to_buf(pkt_ack);
                    sendToLayer3(pkt_ack_buf, DEFAULT_HEADER_LEN);  // 发送ack包
                    _SEND_LOG_(pkt_ack_buf);
                    free(pkt_ack_buf);
                }
                else if(seq >= sock->window.wnd_recv->expect_seq){ // 收到的seq大于等于期待的seq，在可以容忍乱序到达的前提下划分为一类情况
                    // 发回ack，窗口滑动，释放已记录的可释放长度
                    // sock->window.wnd_send->free_length += data_len;
                    // sock->window.wnd_send->base = sock->window.wnd_send->base + sock->window.wnd_send->free_length;
                    // sock->window.wnd_send->free_length = 0;
                    uint32_t expt_seq = sock->window.wnd_recv->expect_seq; //
                    char* recv_buf = sock->window.wnd_recv->recv_buf;
                    uint8_t* mark = sock->window.wnd_recv->mark;
                    if (seq + data_len < expt_seq + MAX_RWINDOW_SIZE){ // 到达的seq加上数据长度比接收方期待收到的seq加上最大窗口长度小就可以放下
                        // 把数据报文的数据拷贝到接收方的接收缓冲区，并将拷贝好的区域mark为1
                        memcpy(recv_buf + seq - expt_seq, pkt + get_hlen(pkt), data_len);
                        memset(mark + seq - expt_seq, 1, data_len);
                    }

                    if (mark[0]!=0){ // 发送窗口的第一个字节可以滑动了
                        uint16_t free_len = 0;
                        while(mark[free_len]!=0){
                            free_len++;
                        }
                        if (MAX_SOCK_BUF_SIZE - sock->received_len < free_len){
                            // printf("接收缓冲区装不下，丢弃该报文\n");
                            // 更新接收窗口剩余大小
                            sock->window.wnd_recv->remain_size=MAX_RWINDOW_SIZE-free_len;
                        }
                        else{
                            sock->window.wnd_recv->remain_size=MAX_RWINDOW_SIZE;
                            // 缓冲区能够装下该数据报
                            pthread_mutex_lock(&(sock->recv_lock));     // 加锁
                            memcpy(sock->received_buf + sock->received_len, recv_buf, free_len);
                            sock->received_len += free_len;
                            pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

                            // 更新接收窗口
                            sock->window.wnd_recv->recv_buf = (char *)malloc(MAX_RWINDOW_SIZE);
                            memcpy(sock->window.wnd_recv->recv_buf, recv_buf + free_len, MAX_RWINDOW_SIZE - free_len);
                            free(recv_buf);
                            sock->window.wnd_recv->mark = (uint8_t *)malloc(MAX_RWINDOW_SIZE);
                            memset(sock->window.wnd_recv->mark, 0, MAX_RWINDOW_SIZE);
                            memcpy(sock->window.wnd_recv->mark, mark + free_len, MAX_RWINDOW_SIZE - free_len);
                            free(mark);
                            sock->window.wnd_recv->expect_seq += free_len;
                            _DELV_LOG_(expt_seq,free_len);
                        }
                    }
                    tju_packet_t* pkt_ack = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                                                sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                                ACK_FLAG_MASK, sock->window.wnd_recv->remain_size, 0, NULL, 0);/* 创建ack包 */
                    char* pkt_ack_buf = packet_to_buf(pkt_ack);
                    sendToLayer3(pkt_ack_buf, DEFAULT_HEADER_LEN);  // 发送ack包
                    _SEND_LOG_(pkt_ack_buf);
                    free(pkt_ack_buf);
                }

            }
            else if(flag == ACK_FLAG_MASK){ // 发送方收到了接受方发回的ACK
                // 根据advertised window值更新发送窗口的大小
                sock->window.wnd_send->rwnd = adv_wnd;
                _RWND_LOG_(sock);
                if(ack < sock->window.wnd_send->base){ // 收到的ACK的ack值比发送窗口的第一个字节都小
                    printf("收到的ack报文在发送窗口外 丢弃报文 \n");
                    // 更新发送窗口
                    sock->window.wnd_send->window_size=min(sock->window.wnd_send->cwnd,sock->window.wnd_send->rwnd);
                    _SWND_LOG_(sock);
                }
                else if(ack == sock->window.wnd_send->base){ // 收到的ACK等于发送窗口的第一个字节，相当于重复ACK了,意味着发生了丢包需要重传
                     if (sock->window.wnd_send->window_status==SLOW_START||sock->window.wnd_send->window_status==CONGESTION_AVOIDANCE){
                        sock->window.wnd_send->same_ack_cnt++;
                    }
                    // 快速恢复
                    else if (sock->window.wnd_send->window_status==FAST_RECOVERY){
                        sock->window.wnd_send->cwnd+=MAX_DLEN;
                        _CWND_LOG_(sock,FAST_RECOVERY);
                    }

                    // 连续收到 3 个重复的ack
                    if (sock->window.wnd_send->same_ack_cnt==3&&sock->window.wnd_send->window_status!=FAST_RECOVERY){
                        // 拥塞阈值更新为发送窗口的一半
                        sock->window.wnd_send->ssthresh=sock->window.wnd_send->cwnd>>1;
                        // 当前拥塞窗口更新为拥塞阈值
                        sock->window.wnd_send->cwnd=sock->window.wnd_send->ssthresh;
                        _CWND_LOG_(sock,FAST_RECOVERY);
                        // 更新发送窗口状态为 快速恢复
                        sock->window.wnd_send->window_status=FAST_RECOVERY;

                        printf("收到三个重复ack，开始快速重传\n");
                        // 开启快速重传
                        sock->is_retransing = true;
                        RETRANS=TRUE;
                    }
                    // 更新发送窗口
                    sock->window.wnd_send->window_size=min(sock->window.wnd_send->cwnd,sock->window.wnd_send->rwnd);
                    _SWND_LOG_(sock);
                }
                else { // 收到了正常的ACK
                    printf("收到有效ACK报文 ack=%d\n", ack);
                    sock->window.wnd_send->same_ack_cnt = 0;  // 刷新快速重传计数

                    // 更新拥塞窗口大小、状态
                    if (sock->window.wnd_send->window_status==SLOW_START){
                        sock->window.wnd_send->cwnd*=2;
                        if (sock->window.wnd_send->cwnd>=sock->window.wnd_send->ssthresh){
                            sock->window.wnd_send->cwnd=sock->window.wnd_send->ssthresh;

                            sock->window.wnd_send->window_status=CONGESTION_AVOIDANCE;
                        }
                        _CWND_LOG_(sock,SLOW_START);
                    }
                    else if (sock->window.wnd_send->window_status==CONGESTION_AVOIDANCE){
                        sock->window.wnd_send->cwnd+=MAX_DLEN;
                        _CWND_LOG_(sock,CONGESTION_AVOIDANCE);
                    }
                    else if (sock->window.wnd_send->window_status==FAST_RECOVERY){
                        sock->window.wnd_send->window_status=CONGESTION_AVOIDANCE;
                    }

                    // 更新发送窗口
                    sock->window.wnd_send->window_size=min(sock->window.wnd_send->cwnd,sock->window.wnd_send->rwnd);
                    _SWND_LOG_(sock);

                    // 开始计算 SampleRTT
                    if (sock->window.wnd_send->is_estimating_rtt==TRUE){
                        if (sock->window.wnd_send->rtt_expect_ack==ack){
                            CalTimeout(sock);
                        }
                        sock->window.wnd_send->is_estimating_rtt=FALSE;
                    }

                    
                    // 发送窗口滑动
                    sock->window.wnd_send->ack_cnt += ack - sock->window.wnd_send->base;
                    sock->window.wnd_send->base = ack;

                    // 启动定时器

                    if (sock->window.wnd_send->base == sock->window.wnd_send->nextseq){
                        // 缓冲区中没有尚未发送的数据
                        stopTimer();
                        sock->is_retransing=FALSE;
                    }
                    else{
                        // 重新开始计时
                        startTimer(sock);
                        sock->is_retransing=TRUE;
                    }

                    // 清理发送缓冲区中已收到确认的数据
                    if (sock->sending_len && (sock->window.wnd_send->ack_cnt == sock->sending_len || sock->window.wnd_send->ack_cnt > 4*MAX_SOCK_BUF_SIZE/5))
                    {
                        printf("正在清理发送缓冲区:\n");
                        pthread_mutex_lock(&sock->send_lock);   // 加锁
                        char* new_sending_buf=(char *)malloc(MAX_SOCK_BUF_SIZE);
                        memcpy(new_sending_buf, sock->sending_buf + sock->window.wnd_send->ack_cnt, sock->sending_len - sock->window.wnd_send->ack_cnt);
                        free(sock->sending_buf);
                        sock->sending_buf = new_sending_buf;
                        sock->sending_len = sock->sending_len - sock->window.wnd_send->ack_cnt;
                        sock->have_send_len = sock->have_send_len - sock->window.wnd_send->ack_cnt;
                        sock->window.wnd_send->ack_cnt = 0;
                        pthread_mutex_unlock(&sock->send_lock);  // 解锁
                        printf("清理完毕\n");
                    }
                    
                }
                if (sock->window.wnd_send->window_size==0){
                    
                    startTimer(sock);
                    sock->is_retransing=FALSE;      // 表示当前为 0 窗口探测阶段
                }
            }
            break;

        default:
            perror("Wrong state\n");
    }
    return 0;
}

int tju_close (tju_tcp_t* sock){
    tju_packet_t* fin_pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                                        sock->seq, 0, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                        FIN_FLAG_MASK, 1, 0, NULL, 0);
    char* fin_pkt_buf = packet_to_buf(fin_pkt);
    sendToLayer3(fin_pkt_buf, DEFAULT_HEADER_LEN); // 发送fin_pkt
    _SEND_LOG_(fin_pkt_buf);
    free(fin_pkt_buf);
    sock->seq++;
    sock->state = FIN_WAIT_1;
    // while(pthread_mutex_lock(&sock->sending_queue->q_lock)!=0);
    while(sock->state != TIME_WAIT);
    
    sock->state = CLOSED;
    printf("成功关闭连接");
    
    return 0;
}
