#include "min_heap_timer.h"

MinHeap_timer::MinHeap_timer(int maxSize)
{
    if(maxSize <= 0)
    {
        LOG_INFO("%s", "Heap size <= 0");
        exit(1);
    }
    this->maxSize = maxSize;
    heapArr = new util_timer* [maxSize];
    CurrentSize = 0;
}

MinHeap_timer::MinHeap_timer(util_timer* a[], int maxSize, int n)
{
    if(maxSize <= 0 || n <= 0)
    {
        LOG_INFO("%s", "Heap size <= 0");
        exit(1);
    }
    this->maxSize = maxSize;
    heapArr = new util_timer* [maxSize];
    for(int i = 0; i < n; i++)
        heapArr[i] = a[i];
    CurrentSize = n;
    int i = (CurrentSize - 2)/2; //
    while(i >= 0)
    {
        FilterDown(i);
        i--;
    }
}

void MinHeap_timer::AddTimer(util_timer* timer)
{
    if(timer == nullptr)
        return;
    if(IsFull())
    {
        resize();
    }
    heapArr[CurrentSize] = timer;
    FilterUp(CurrentSize);
    ++CurrentSize;
}

void MinHeap_timer::DeleteTopTimer()
{
    if(IsEmpty())
        return;
    if(heapArr[0])
    {
        heapArr[0] = heapArr[CurrentSize - 1];
        CurrentSize--;
        FilterDown(0);
    }
}

util_timer* MinHeap_timer::GetTopTimer()
{
    if(IsEmpty())
    {
        exit(1);
    }
    return heapArr[0];
}

void cb_fnc_del(client_data *user_data)
{
    return;
}

void MinHeap_timer::DeleteTimer(util_timer* timer)
{
    if(!timer)
        return;
    timer->cb_func = cb_fnc_del;
}

void MinHeap_timer::tick()
{
    time_t cur_time = time(NULL);
    while(!IsEmpty())
    {
        if(heapArr[0]->expire > cur_time)
        //当前定时器到期，则调用回调函数，执行定时事件
        heapArr[0]->cb_func(heapArr[0]->user_data);
        DeleteTopTimer();
    }
}

void MinHeap_timer::FilterUp(int end)
{
    int j = end, i;
    util_timer* temp = heapArr[j];
    i = (j - 1) / 2;
    while(j > 0)
    {
        if(heapArr[i] <= temp)
            break;
        else
        {
            heapArr[j] = heapArr[i];
            j = i;
            i = (j - 1) / 2;
        }
        heapArr[j] = temp;
    }
}

void MinHeap_timer::FilterDown(int Start)
{
    int i  = Start, j;
    util_timer* temp = heapArr[i];
    j = 2*i + 1;
    while(j <= CurrentSize - 1)
    {
        if(j < CurrentSize - 1 && heapArr[j] > heapArr[j+1])
            j++;
        if(temp <= heapArr[j])
            break;
        else
        {
            heapArr[i] = heapArr[j];
            i = j;
            j = 2*j + 1;
        }
    }
    heapArr[i] = temp;
}

void MinHeap_timer::resize()
{
    util_timer ** tmp = new util_timer*[2 * maxSize];
    for(int i = 0; i < 2 * maxSize; ++i)
        tmp[i] = NULL;
    maxSize *= 2;
    for(int i = 0; i < CurrentSize; ++i)
        tmp[i] = heapArr[i];
    delete []heapArr;
    heapArr = tmp;
}
