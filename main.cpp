#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/min_heap_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

#define SYNLOG //同步写日志
//#define ASYNLOG //异步写日志

#define listenfdET //边缘触发非阻塞
//#define listenfdLT //水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];

//创建定时器容器小根堆
MinHeap_timer timer_heap(100);

static int epollfd = 0;

//信号处理函数
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;

    //将信号值从管道写端写入，传输字符类型，而非整型
    send(pipefd[1], (char *)&msg, 1, 0);

    //将信号值从管道写端写入，传输字符类型，而非整型
    errno = save_errno;
}

//设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true)
{
    //创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    //信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;

    //将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);

    //执行sigaction函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    //定时处理任务
    timer_heap.tick();
    //重新定时以不断触发SIGALRM信号
    alarm(TIMESLOT);

}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    //删除非活动连接在socket上的注册事件
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);

    //关闭文件描述符
    close(user_data->sockfd);

    //减少连接数
    http_conn::m_user_count--;

    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif

    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);

    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "pig", "123456", "webserverdb", 3306, 8);

    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    //预先为每个可能的客户连接分配一个http_conn对象
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    /**************socket**************/
    //创建监听socket文件描述符
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    //创建监听socket的TCP/IP的IPv4 socket地址
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;                //地址族
    address.sin_addr.s_addr = htonl(INADDR_ANY); //IPv4地址，网络字节序表示，INADDR_ANY：将套接字绑定到所有可用的接口
    address.sin_port = htons(port);              //端口号 要用网络字节序表示

    int flag = 1;

    //SO_REUSEADDR 允许端口被重复使用，端口复用最常用的用途是:1.防止服务器重启时之前绑定的端口还未释放 2.程序突然退出而系统没有释放端口
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    //绑定socket和它的地址
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    //创建监听队列以存放待处理的客户连接，在这些客户连接被accept()之前
    ret = listen(listenfd, 5);
    assert(ret >= 0);
    /**********************************/



    /**************Epoll**************/
    //用于存储epoll事件表中就绪事件的event数组
    epoll_event events[MAX_EVENT_NUMBER];

    //创建一个额外的文件描述符来唯一标识内核中的epoll事件表
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    //主线程往epoll内核事件表中注册监听socket事件，当listen到新的客户连接时，listenfd变为就绪事件
    addfd(epollfd, listenfd, false);

    //将上述epollfd赋值给http类对象的m_epollfd属性
    http_conn::m_epollfd = epollfd;
    /**********************************/



    /**************定时器**************/

    //每个user（http请求）对应的timer
    client_data *users_timer = new client_data[MAX_FD];

    //每隔TIMESLOT时间触发SIGALRM信号
    bool timeout = false;
    alarm(TIMESLOT);

    //创建管道，注册pipefd[0]上的可读事件
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);

    //设置管道写端为非阻塞
    setnonblocking(pipefd[1]);

    //设置管道读端为ET非阻塞，并添加到epoll内核事件表
    addfd(epollfd, pipefd[0], false);

    //传递给主循环的信号值，这里只关注SIGALRM和SIGTERM
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;
    /**********************************/

    while (!stop_server)
    {
        //主线程调用epoll_wait等待一组文件描述符上的事件，并将当前所有就绪的epoll_event复制到events数组中
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 然后我们遍历这一数组以处理这些已经就绪的事件
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd; // 事件表中就绪的socket文件描述符

            //处理新到的客户连接
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_address);

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
               //创建定时器临时变量
                util_timer *timer = new util_timer(time(NULL) + 3 * TIMESLOT, cb_func, &users_timer[connfd]);

                //创建该连接对应的定时器，初始化为前述临时变量
                users_timer[connfd].timer = timer;

                //将该定时器添加到链表中
                timer_heap.AddTimer(timer);
#endif

#ifdef listenfdET
                while (1)
                {
                    // accept()返回一个新的socket文件描述符用于send()和recv()
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }

                    //将connfd注册到内核事件表中
                    users[connfd].init(connfd, client_address);

                    //初始化该连接对应的连接资源
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;

                    //创建定时器临时变量
                    util_timer *timer = new util_timer(time(NULL) + 3 * TIMESLOT, cb_func, &users_timer[connfd]);

                    //创建该连接对应的定时器，初始化为前述临时变量
                    users_timer[connfd].timer = timer;

                    //将该定时器添加到链表中
                    timer_heap.AddTimer(timer);
                }
                continue;
#endif
            }

            //处理异常事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_heap.DeleteTimer(timer);
                }
            }

            //处理信号，管道读端对应文件描述符发生读事件
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];

                //从管道读端读出信号值，成功返回字节数，失败返回-1
                //正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }

            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                //创建定时器临时变量，将该连接对应的定时器取出来
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();

                        //删除
                        timer_heap.DeleteTimer(timer);
                        //新加
                        util_timer *new_timer =  new util_timer(time(NULL) + 3 * TIMESLOT, cb_func, &users_timer[sockfd]);
                        users_timer[sockfd].timer = new_timer;
                        timer_heap.AddTimer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_heap.DeleteTimer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        //删除
                        timer_heap.DeleteTimer(timer);
                        //新加
                        util_timer *new_timer =  new util_timer(time(NULL) + 3 * TIMESLOT, cb_func, &users_timer[sockfd]);
                        users_timer[sockfd].timer = new_timer;
                        timer_heap.AddTimer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_heap.DeleteTimer(timer);
                    }
                }
            }
        }

        //处理定时器为非必须事件，收到信号并不是立马处理，完成读写事件后，再进行处理
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
