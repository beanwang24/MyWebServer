#ifndef MIN_HEAP_TIMER
#define MIN_HEAP_TIMER

#include <time.h>
#include "../log/log.h"
#include <netinet/in.h>
class util_timer;

//连接资源
struct client_data
{
    //客户端socket地址
    sockaddr_in address;

    //socket文件描述符
    int sockfd;

    //定时器
    util_timer *timer;
};

//定时器类
class util_timer
{
public:
    util_timer(time_t expire = NULL, void (*cb_func)(client_data *) = NULL, client_data *user_data = NULL)
    {
        this->expire = expire;
        this->cb_func = cb_func;
        this->user_data = user_data;
    }

public:
    //超时时间
    time_t expire;

    //回调函数
    void (*cb_func)(client_data *);

    //连接资源
    client_data *user_data;

    bool operator < (const util_timer &a)
    {
        return expire < a.expire;
    }

};


class MinHeap_timer
{
private:
    util_timer **heapArr;
    int CurrentSize;
    int maxSize;

    void FilterDown(int Start);
    void FilterUp(int end);
    void resize();

public:
    MinHeap_timer(int maxsize);
    MinHeap_timer(util_timer *a[], int maxSize, int n);
    ~MinHeap_timer(){ delete []heapArr; }
    void AddTimer(util_timer *timer);
    void DeleteTopTimer();
    util_timer * GetTopTimer();
    void DeleteTimer(util_timer *timer);
    void tick();


    bool IsEmpty() const { return CurrentSize == 0; }
    bool IsFull() const { return CurrentSize == maxSize; }
    int SizeOfHeap() const { return CurrentSize; }
    void SetEmpty() { CurrentSize = 0; }

};


#endif
