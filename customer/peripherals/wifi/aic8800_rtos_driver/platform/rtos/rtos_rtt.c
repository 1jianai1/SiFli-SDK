/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * RT-Thread RTOS port implementation for AIC8800 WiFi driver.
 * Based on rtos_freertos.c — provides equivalent API surface.
 */

/*
 * INCLUDE FILES
 ****************************************************************************************
 */
#include <string.h>
#include "rtos_port.h"
#include "aic_log.h"
#include "aic_assert.h"
#include "netif_port.h"

#include <rtthread.h>
#include <rthw.h>      /* [AIC8800-PORT][SiFli] rt_hw_interrupt_disable/enable */
#include <rtdevice.h>

/* Forward declarations for static functions used before definition */
rtos_task_handle aicwf_rtos_get_current_task(void);

#ifdef CONFIG_RTOS_RESLIST

// ASR plat need mem statistics
#define AIC_RTOS_MEM_MAX        ((1024+256)*1024)

static uint32_t rtos_mem_cnt = 0;
static uint32_t rtos_free_cnt = 0;
static uint32_t rtos_remain_cnt = 0;

static rtos_mutex rtos_mutexRef = NULL;
struct co_list res_list[RES_TYPE_MAX] = {NULL, NULL};

static uint32_t rtos_res_time_cnt = 0;

static __INLINE void aicwf_OsMemCal(uint8_t inc, uint32_t len)
{
    if (inc)
    {
        rtos_remain_cnt += len;
        rtos_mem_cnt += len;
    }
    else
    {
        rtos_remain_cnt -= len;
        rtos_free_cnt += len;
    }
}

static __INLINE void aicwf_OsResInit(OsResSt *st, char *name, void *ref)
{
    memset(st, 0, sizeof(OsResSt));
    if (name != NULL)
        memcpy(st->name, name, strlen(name) > OS_RES_NAME_MAX ? OS_RES_NAME_MAX : strlen(name));
#if !RTOS_INFO_SHORT
    else
    {
        sprintf(st->name, "%#x", ref);
    }
#endif
    st->ref = ref;
}

extern int aic_close(int s);
extern int aic_shutdown(int s, int how);
extern void rtos_socket_close_all(void);
void aicwf_rtos_del_by_type(OsResSt *st)
{
#if RTOS_AL_INFO_DUMP
    DBG_OSAL_INF("[rtos]del: type[%d]:%-16s, %#8x, %#8x, taskhdl=%#8x\n",
                 st->type, st->name, st->ref, st->para0, st->para1);
#endif

    switch (st->type)
    {
    case RES_TYPE_MUTEX:
        rt_mutex_delete((rtos_mutex)st->ref);
        break;
    case RES_TYPE_SEM:
        rt_sem_delete((rtos_semaphore)st->ref);
        break;
    case RES_TYPE_Q:
        rt_mq_delete((rtos_queue)st->ref);
        break;
    case RES_TYPE_TIME:
        if (st->ref)
            rt_timer_delete((rtos_timer)st->ref);
        break;
    case RES_TYPE_TASK:
        /* Delete RT-Thread thread object (only when not in CLOSE state) */
        if (st->ref)
        {
            if (RT_THREAD_CLOSE != ((rt_thread_t)st->ref)->stat)
                rt_thread_delete((rt_thread_t)st->ref);
        }
        if (st->para0)
            free((void *)st->para0);
        break;
    case RES_TYPE_MEM:

        if (rtos_remain_cnt < st->para0)
        {
            DBG_OSAL_WRN("[rtos][warning] rtos mem cnt:%d %d %d\n", rtos_remain_cnt, rtos_free_cnt, rtos_mem_cnt);
            //AIC_ASSERT_ERR(0);
        }

        aicwf_OsMemCal(0, st->para0);
        free(st->ref);
        break;

    case RES_TYPE_SOCKET:
        aic_shutdown((int)(st->para0), 2);
        aic_close((int)(st->para0));
        break;

    default:
        break;
    }
}

/**
 * @brief 获取RTOS资源互斥锁
 *
 * 保护资源列表的并发访问，首次调用时自动创建互斥信号量。
 *
 * @return RT_EOK=获取成功
 */
uint32_t aicwf_rtos_res_mutex_lock(void)
{
    if (rtos_mutexRef == NULL)
    {
        rtos_mutexRef = rt_mutex_create("rtos_res_mutex", RT_IPC_FLAG_FIFO);
        AIC_ASSERT_ERR(rtos_mutexRef != NULL);
    }

    return rt_mutex_take(rtos_mutexRef, RT_WAITING_FOREVER);
}

/**
 * @brief 释放RTOS资源互斥锁
 */
void aicwf_rtos_res_mutex_unlock(void)
{
    if (rtos_mutexRef)
        rt_mutex_release(rtos_mutexRef);
}

/**
 * @brief 将资源记录推入资源追踪列表
 * @param res 资源描述结构指针
 * @return 新分配的资源列表节点指针, NULL=失败
 */
OsResList *aicwf_rtos_res_list_push(OsResSt *res)
{
    uint32_t hs = sizeof(OsResList);
    uint32_t nl;
    OsResList *p;

    p = malloc(hs);

    if (p == NULL)
    {
        DBG_OSAL_ERR("[rtos] %s: fail\n", __func__);
        return 0;
    }

    nl = strlen(res->name);
    nl = (nl > OS_RES_NAME_MAX) ? OS_RES_NAME_MAX : nl;
    aicwf_rtos_res_mutex_lock();
    p->res.para0 = res->para0;
    p->res.para1 = res->para1;
    p->res.ref = res->ref;
    p->res.type = res->type;
    memcpy(p->res.name, res->name, nl);
    p->res.name[nl] = 0;

    aicwf_OsMemCal(1, hs);
    if ((res->type == RES_TYPE_MEM) || (res->type == RES_TYPE_MEM_NET_RX))
    {
        aicwf_OsMemCal(1, res->para0);
    }
    if (res->type == RES_TYPE_TIME)
    {
        p->res.para0 = rtos_res_time_cnt++;
    }

    co_list_push_back(&res_list[res->type], &(p->hdr));

    aicwf_rtos_res_mutex_unlock();

#if RTOS_AL_INFO_DUMP
    DBG_OSAL_INF("[rtos]push:type[%d]:%-16s, %#8x, %#8x, taskhdl=%#8x\n", res->type, res->name, res->ref, p, aicwf_rtos_get_current_task());
#endif
    return p;
}

/**
 * @brief 根据引用指针释放单个资源
 * @param ref  资源引用指针
 * @param type 资源类型（低8位）和参数（高8位）
 */
void aicwf_rtos_res_list_release(void *ref, uint16_t type)
{
    uint8_t rtype = type & 0xff;
    uint8_t rpara = (type >> 8) & 0xff;
    OsResList *pRes;
    struct co_list *plist;

    aicwf_rtos_res_mutex_lock();
    plist = &res_list[rtype];
    pRes = (OsResList *)co_list_pick(plist);

    while (pRes)
    {
        if (pRes->res.ref)
        {
            if (pRes->res.ref == ref)
            {
                break;
            }
        }
        else if (pRes->res.para0 == rpara)
        {
            break;
        }
        pRes = (OsResList *)co_list_next(&(pRes->hdr));
    }

    if (pRes != NULL)
    {
        if (pRes->res.type == RES_TYPE_TIME)
        {
            if (rtos_res_time_cnt)
                rtos_res_time_cnt--;
        }
        aicwf_rtos_del_by_type(&(pRes->res));
        co_list_extract(plist, &(pRes->hdr));
        free(pRes);
        aicwf_OsMemCal(0, sizeof(OsResList));
    }
    aicwf_rtos_res_mutex_unlock();

    if (pRes == NULL)
    {
        DBG_OSAL_INF("[rtos]rtos res no this ref?:%#8x type:%d, taskhdl=%#8x\r\n", ref, type, aicwf_rtos_get_current_task());
    }
}

/**
 * @brief 释放所有已追踪的RTOS资源
 *
 * 遍历所有资源类型列表，逐一删除并释放内存。
 * 通常在WiFi去初始化时调用。
 */
void aicwf_rtos_res_release_all(void)
{
    OsResList *pRes, *pres1;
    struct co_list *plist;
    int i = 0;

    aicwf_rtos_res_mutex_lock();

    for (i = 0; i < RES_TYPE_MAX; i++)
    {
        plist = &res_list[i];
        pRes = (OsResList *)co_list_pick(plist);

        while (pRes)
        {
            DBG_OSAL_INF("[rtos]del:type[%d]:%-16s, %#8x, taskhdl=%#8x\n",
                         pRes->res.type, pRes->res.name, pRes->res.ref, aicwf_rtos_get_current_task());
            aicwf_rtos_del_by_type(&pRes->res);
            pres1 = pRes;
            pRes = (OsResList *)co_list_next(&(pRes->hdr));
            free((void *)pres1);
            aicwf_OsMemCal(0, sizeof(OsResList));
        }

        co_list_init(plist);
    }

    aicwf_rtos_res_mutex_unlock();

    /* [backport EC] 初始化失败提前释放时 rtos_pval 可能尚未由平台赋值,
     * 判空避免 hardfault */
    if (rtos_pval_1)
        *rtos_pval_1 = NULL;
    if (rtos_pval_2)
        *rtos_pval_2 = NULL;
}

/**
 * @brief 从资源列表中移出节点（不删除底层资源）
 *
 * 与 _release 的区别：仅从链表中摘除，不调用底层删除函数。
 * 用于socket等需要手动管理的资源。
 *
 * @param ref  资源引用指针
 * @param type 资源类型和参数
 */
void aicwf_rtos_res_list_rellist(void *ref, uint16_t type)
{
    uint8_t rtype = type & 0x00ff;
    uint8_t rpara = (type >> 8) & 0xff;
    OsResList *pRes;
    struct co_list *plist;

    aicwf_rtos_res_mutex_lock();
    plist = &res_list[rtype];
    pRes = (OsResList *)co_list_pick(plist);

    while (pRes)
    {
        if (pRes->res.ref)
        {
            if (pRes->res.ref == ref)
            {
                break;
            }
        }
        else if (pRes->res.para0 == rpara)
        {
            break;
        }
        pRes = (OsResList *)co_list_next(&(pRes->hdr));
    }


    if (pRes != NULL)
    {
        if (rtype == RES_TYPE_SOCKET)
            DBG_OSAL_INF("[rtos]del list:type[%d]:%-16s, %x, taskhdl=%#8x\n",
                         pRes->res.type, pRes->res.name, pRes->res.ref, aicwf_rtos_get_current_task());
        co_list_extract(plist, &(pRes->hdr));
        aicwf_OsMemCal(0, sizeof(OsResList));
        free(pRes);
    }

    aicwf_rtos_res_mutex_unlock();
}


/**
 * @brief 在资源列表中查找指定资源
 * @param ref  资源引用指针
 * @param type 资源类型和参数
 * @return 找到的资源结构指针, NULL=未找到
 */
OsResSt   *aicwf_rtos_res_find_res(void *ref, uint16_t type)
{

    uint8_t rtype = type & 0xff;
    uint8_t rpara = (type >> 8) & 0xff;
    OsResList *pRes;
    struct co_list *plist;

    aicwf_rtos_res_mutex_lock();
    plist = &res_list[rtype];
    pRes = (OsResList *)co_list_pick(plist);

    while (pRes)
    {
        if (pRes->res.ref)
        {
            if (pRes->res.ref == ref)
            {
                break;
            }
        }
        else if (pRes->res.para0 == rpara)
        {
            break;
        }
        pRes = (OsResList *)co_list_next(&(pRes->hdr));
    }

    aicwf_rtos_res_mutex_unlock();

    if (pRes)
        return &(pRes->res);
    else
    {
        DBG_OSAL_INF("[[rtos]]rtos find none:%#8x type:%d, taskhdl=%#8x\r", ref, type, aicwf_rtos_get_current_task());
        return NULL;
    }

}

static OsResSt   *__aicwf_rtos_res_find_res_by_para(uint16_t type, uintptr_t para)
{

    uint8_t rtype = type & 0xff;
    uint8_t rpara = (type >> 8) & 0xff;
    OsResList *pRes;
    struct co_list *plist;

    aicwf_rtos_res_mutex_lock();
    plist = &res_list[rtype];
    pRes = (OsResList *)co_list_pick(plist);

    while (pRes)
    {
        if (rpara == 0)
        {
            if (pRes->res.para0 == para)
            {
                break;
            }
        }
        else
        {
            if (pRes->res.para1 == para)
                break;
        }
        pRes = (OsResList *)co_list_next(&(pRes->hdr));
    }

    aicwf_rtos_res_mutex_unlock();

    if (pRes)
        return &(pRes->res);
    else
    {
        DBG_OSAL_INF("[[rtos]]rtos find none:%#8x type:%d, taskhdl=%#8x\r\n", para, type, aicwf_rtos_get_current_task());
        return NULL;
    }

}
#endif /* CONFIG_RTOS_RESLIST */

/**
 * @brief 显示资源列表信息（调试用）
 *
 * 打印所有类型的资源信息，包括名称、引用、参数等。
 */
void aicwf_rtos_res_list_info_show(void)
{
#ifdef CONFIG_RTOS_RESLIST
    OsResList *pRes;
    struct co_list *plist;
    int i = 0;

    aicwf_rtos_res_mutex_lock();

    for (i = 0; i < RES_TYPE_MAX; i++)
    {
        plist = &res_list[i];
        pRes = (OsResList *)co_list_pick(plist);

        while (pRes)
        {
            DBG_OSAL_VRB("[rtos]type[%d] name:%-16s, ref:%#8x para0:%#8x, para1:%#8x\n",
                         pRes->res.type, pRes->res.name, pRes->res.ref, pRes->res.para0, pRes->res.para1);
            pRes = (OsResList *)co_list_next(&(pRes->hdr));
        }

    }

    aicwf_rtos_res_mutex_unlock();

    DBG_OSAL_INF("[rtos][show] rtos mem cnt:%d %d %d %d\n",
                 co_list_cnt(&res_list[RES_TYPE_MEM]), rtos_remain_cnt, rtos_free_cnt, rtos_mem_cnt);
#endif
}


__STATIC_INLINE uint32_t rtos_ms_to_tick(unsigned long ms)
{
    return rt_tick_from_millisecond(ms);
}

/**
 * @brief 将tick数转换为毫秒
 * @param tick RT-Thread tick计数值
 * @return 对应的毫秒数
 */
unsigned long aicwf_rtos_tick_to_ms(uint32_t tick)
{
    return (uint32_t)tick * (1000 / RT_TICK_PER_SECOND);
}

__STATIC_INLINE  rt_tick_t rtos_timeout_2_tickcount(int timeout_ms)
{
    if (timeout_ms < 0)
    {
        return RT_WAITING_FOREVER;
    }
    else
    {
        return rt_tick_from_millisecond(timeout_ms);
    }
}

/**
 * @brief 获取当前系统时间（毫秒）
 * @param isr 是否在中断上下文中调用（RT-Thread 中 tick 获取 ISR 安全）
 * @return 自系统启动以来的毫秒数
 */
unsigned long aicwf_rtos_now(bool isr)
{
    unsigned long tick = 0;
    tick = rt_tick_get();
    return aicwf_rtos_tick_to_ms(tick);
}

/**
 * @brief 毫秒级延时
 * @param time_in_ms 延时毫秒数
 */
void aicwf_rtos_msleep(uint32_t time_in_ms)
{
    if (time_in_ms == 0)
        time_in_ms = 1;

    rt_thread_mdelay(time_in_ms);
}

/**
 * @brief 微秒级延时
 * @param us 延时的微秒数
 * [AIC8800-PORT][SiFli] 基准用 extern udelay(); SiFli/RT-Thread 无此符号,
 * 改用 rthw.h 的 rt_hw_us_delay(rt_uint32_t) (板级 SysTick/定时器实现)。
 */
void aicwf_rtos_udelay(unsigned int us)
{
    rt_hw_us_delay((rt_uint32_t)us);
}

/**
 * @brief RTOS堆内存分配（带资源追踪）
 *
 * 分配内存并注册到资源追踪列表。
 * 开启 CONFIG_RTOS_RESLIST 时会检查内存上限。
 *
 * @param size 分配大小（字节）
 * @return 内存指针, NULL=失败
 */
void *aicwf_rtos_malloc(uint32_t size)
{
#ifdef CONFIG_RTOS_RESLIST
    void *pdata = NULL;
    OsResSt st;

    if (rtos_remain_cnt > AIC_RTOS_MEM_MAX)
    {
        DBG_OSAL_INF("[rtos]rtos malloc none space\r\n");
        return NULL;
    }
    if (size == 0)
    {
        DBG_OSAL_INF("[rtos]rtos malloc size 0\r\n");
        return NULL;
    }

    pdata = malloc(size);
    if (pdata)
    {
        void *tref = NULL;
        char *name = NULL;
        tref = aicwf_rtos_get_current_task();
#if !RTOS_INFO_SHORT
        name = aicwf_rtos_get_task_name(tref);
#endif
        aicwf_OsResInit(&st, name, pdata);
        st.para0 = size;
        st.para1 = (uint32_t)tref;
        st.ref = pdata;
        st.type = RES_TYPE_MEM;
        aicwf_rtos_res_list_push(&st);
    }
    else
    {
        DBG_OSAL_ERR("[rtos]malloc fail\r\n");
    }

#else /* CONFIG_RTOS_RESLIST */

    void *pdata = malloc(size);
    if (!pdata)
    {
        DBG_OSAL_ERR("[rtos]malloc fail\r\n");
    }

#endif /* CONFIG_RTOS_RESLIST */

    return pdata;
}

/**
 * @brief RTOS堆内存分配并清零（calloc语义）
 * @param nb_elt 元素个数
 * @param size   每个元素的大小
 * @return 已清零的内存指针, NULL=失败
 */
void *aicwf_rtos_calloc(uint32_t nb_elt, uint32_t size)
{
    if (nb_elt == 0 || size == 0)
    {
        DBG_OSAL_INF("[rtos]rtos calloc size 0\r\n");
        return NULL;
    }

    void *pdata = aicwf_rtos_malloc(nb_elt * size);
    if (pdata)
    {
        memset(pdata, 0, nb_elt * size);
    }

    return pdata;
}

/**
 * @brief RTOS堆内存释放
 *
 * 释放内存并从资源追踪列表中移除。
 *
 * @param ptr 要释放的内存指针
 */
void aicwf_rtos_free(void *ptr)
{
#ifdef CONFIG_RTOS_RESLIST
    if (ptr != NULL)
    {
        aicwf_rtos_res_list_release(ptr, RES_TYPE_MEM);
    }

#else

    free(ptr);

#endif
}



/**
 * @brief 获取当前内存使用统计
 * @param mtot 输出：总分配量
 * @param ftot 输出：总释放量
 * @param rtot 输出：当前剩余量
 */
void aicwf_rtos_mem_cur_cnt(uint32_t *mtot, uint32_t *ftot, uint32_t *rtot)
{
#ifdef CONFIG_RTOS_RESLIST
    *mtot = rtos_mem_cnt;
    *ftot = rtos_free_cnt;
    *rtot = rtos_remain_cnt;
#endif
}

/**
 * @brief 移除所有RTOS资源并重置内存统计
 */
void aicwf_rtos_remove_all(void)
{
#ifdef CONFIG_RTOS_RESLIST
    aicwf_rtos_res_release_all();

    if (rtos_remain_cnt != 0)
    {
        DBG_OSAL_WRN("[rtos][warning] rtos mem cnt:%d %d %d\n", rtos_remain_cnt, rtos_free_cnt, rtos_mem_cnt);
    }

    rtos_remain_cnt = 0;
    rtos_free_cnt = 0;
    rtos_mem_cnt = 0;

#ifdef CONFIG_SOCK_RTOS_AL
    rtos_socket_close_all();
#endif
#endif
}

/**
 * @brief 释放指定任务关联的所有内存
 * @param taskref 任务句柄
 */
void aicwf_rtos_mem_free_task(void *taskref)
{
#ifdef CONFIG_RTOS_RESLIST
    uintptr_t para = (uintptr_t)taskref;
    OsResSt *resp = NULL;

    do
    {
        resp = __aicwf_rtos_res_find_res_by_para(RES_TYPE_MEM | 0x0100, para);
        if (resp)
            aicwf_rtos_free(resp->ref);
    }
    while (resp);
#endif
}

/**
 * @brief 网络缓冲区入队追踪
 * @param pbuf  pbuf指针
 * @param size  缓冲区大小
 * @param pmsg  消息引用
 * @param type  资源类型
 */
void aicwf_rtos_net_mempush(void *pbuf, uint32_t size, void *pmsg, uint16_t type)
{
#ifdef CONFIG_RTOS_RESLIST
    OsResSt st;
    if (pbuf)
    {
        aicwf_OsResInit(&st, NULL, pmsg);
        st.para0 = size;
        st.ref = pmsg;
        st.para1 = (uint32_t)pbuf;
        st.type = type;
        aicwf_rtos_res_list_push(&st);

#if RTOS_AL_INFO_DUMP
        DBG_OSAL_INF("[rtos]type[%d] name:%-16s, ref:%#8x, para0:%#8x, para1:%#8x\n",
                     st.type, st.name, st.ref, st.para0, st.para1);
#endif
    }
#endif
}

/**
 * @brief 网络缓冲区出队追踪
 * @param pmsg 消息引用
 * @param type 资源类型
 */
void aicwf_rtos_net_mempop(void *pmsg, uint16_t type)
{
#ifdef CONFIG_RTOS_RESLIST
    aicwf_rtos_res_list_rellist(pmsg, type);
#endif
}

/**
 * @brief 释放网络缓冲区资源
 * @param pmsg 消息引用
 * @param type 资源类型
 */
void aicwf_rtos_net_mem_free(void *pmsg, uint16_t type)
{
#ifdef CONFIG_RTOS_RESLIST
    aicwf_rtos_res_list_release(pmsg, type);
#endif
}

/**
 * @brief 将socket注册到资源追踪列表
 * @param sock socket文件描述符
 * @param port 绑定端口号
 */
void aicwf_rtos_socket_push(int sock, uint32_t port)
{
#ifdef CONFIG_RTOS_RESLIST
    OsResSt st;
    char name[10];

    if (sock != -1)
    {
        sprintf(name, "sock%d", sock);
        aicwf_OsResInit(&st, name, NULL);
        st.para0 = sock;
        st.para1 = port;
        st.type = RES_TYPE_SOCKET;
        aicwf_rtos_res_list_push(&st);
        DBG_OSAL_INF("[rtos]type[%d] name:%-16s, para0:%#8x\n", st.type, st.name, st.para0);
    }
#endif
}

/**
 * @brief 将socket从资源追踪列表移除
 * @param sock socket文件描述符
 */
void aicwf_rtos_socket_pop(int sock)
{
#ifdef CONFIG_RTOS_RESLIST
    uint16_t rsock = (uint16_t)sock << 8;
    rsock &= 0xff00;

    aicwf_rtos_res_list_rellist(NULL, rsock | RES_TYPE_SOCKET);
#endif
}

/**
 * @brief 在资源列表中查找socket绑定的端口
 * @param sock socket文件描述符
 * @return 绑定的端口号, 0=未找到
 */
uint32_t aicwf_rtos_socket_find(int sock)
{
    uint32_t port = 0;

#ifdef CONFIG_RTOS_RESLIST
    OsResList *pRes;
    struct co_list *plist = &res_list[RES_TYPE_SOCKET];

    aicwf_rtos_res_mutex_lock();
    pRes = (OsResList *)co_list_pick(plist);

    while (pRes)
    {
        if (pRes->res.para0 == sock)
        {
            port = pRes->res.para1;
            break;
        }
        pRes = (OsResList *)co_list_next(&(pRes->hdr));
    }
    aicwf_rtos_res_mutex_unlock();
#endif

    return port;
}

/**
 * @brief 获取任务状态
 * @param ref 任务句柄
 * @return 任务状态码（rt_thread_t->stat）
 */
uint32_t aicwf_rtos_get_task_status(rtos_task_handle ref)
{
    uint32_t st = ((rt_thread_t)ref)->stat;

    DBG_OSAL_INF("[rtos]%s: handle=%p st=%d\n", __func__, ref, st);

    return st;
}

/**
 * @brief 等待任务进入挂起状态
 *
 * 轮询任务状态直到任务挂起/完成，未自行挂起时强制挂起。
 *
 * @param ref 任务句柄
 * @return 0
 */
uint32_t aicwf_rtos_wait_task_suspend(rtos_task_handle ref)
{
    uint32_t st;

    while (1)
    {
        st = aicwf_rtos_get_task_status(ref);
#if !RTOS_DEL_NO_WAIT
        if ((st == RT_THREAD_RUNNING) || (st == RT_THREAD_READY))
            aicwf_rtos_msleep(1);
        else
#endif
            break;
    };

    if ((st != RT_THREAD_CLOSE))
        rt_thread_suspend((rt_thread_t)ref);

    return 0;
}

/**
 * @brief 仅等待任务挂起（不强制挂起）
 * @param ref 任务句柄
 * @return 0
 */
uint32_t aicwf_rtos_wait_task_suspend_only(rtos_task_handle ref)
{
    uint32_t st;

    while (1)
    {
        st = aicwf_rtos_get_task_status(ref);

        if (((st == RT_THREAD_READY) || (st == RT_THREAD_RUNNING)))
            aicwf_rtos_msleep(1);
        else
            break;
    };

    return 0;
}

/**
 * @brief 等待所有已追踪的任务挂起
 *
 * 遍历 RES_TYPE_TASK 列表，逐一等待各任务挂起。
 */
void aicwf_rtos_wait_all_task_suspend(void)
{
#ifdef CONFIG_RTOS_RESLIST
    OsResList *pRes;
    struct co_list *plist;

    aicwf_rtos_res_mutex_lock();
    plist = &res_list[RES_TYPE_TASK];
    pRes = (OsResList *)co_list_pick(plist);
    aicwf_rtos_res_mutex_unlock();

    while (pRes)
    {
        if (pRes->res.ref)
        {
            aicwf_rtos_wait_task_suspend(pRes->res.ref);
        }

        pRes = (OsResList *)co_list_next(&(pRes->hdr));
    }
#endif
}

/**
 * @brief 获取当前正在执行的任务句柄
 * @return 当前任务句柄
 */
rtos_task_handle aicwf_rtos_get_current_task(void)
{
    rtos_task_handle taskref;
    taskref = rt_thread_self();

    return (rtos_task_handle)taskref;
}

/**
 * @brief 获取任务名称
 * @param ref 任务句柄
 * @return 任务名称字符串
 */
char *aicwf_rtos_get_task_name(void *ref)
{
    rt_thread_t th = (rt_thread_t)ref;
#if (defined(RTTHREAD_VERSION) && (RTTHREAD_VERSION >= 50002))  /* [AIC8800-PORT][SiFli] RT-Thread 3.1.3 无 RT_VERSION_CHECK 宏(预处器解析报错); RTTHREAD_VERSION 编码=major*10000+minor*100+patch, 5.0.2=50002; SiFli=30103 故走 #else (th->name) */
    return th->parent.name;
#else
    return th->name;
#endif
}

/**
 * @brief 创建RTOS任务（带资源追踪）
 *
 * 创建任务并注册到资源追踪列表。
 *
 * @param func         任务入口函数
 * @param name         任务名称
 * @param task_id      任务ID（未使用）
 * @param stack_depth  栈大小（字节）
 * @param params       任务参数
 * @param prio         优先级
 * @param task_handle  输出的任务句柄
 * @return 0=成功, -3=创建失败
 */
int aicwf_rtos_task_create(rtos_task_fct func,
                           const char *const name,
                           int task_id,
                           const uint16_t stack_depth,
                           void *const params,
                           rtos_prio prio,
                           rtos_task_handle *task_handle)
{
    rt_err_t res;
    OsResSt st;
    rt_thread_t handle;
    void *stack_base = NULL;

    handle = rt_thread_create(name, func, params, stack_depth, prio, 20);
    if (handle == RT_NULL)
    {
        return -3;
    }

    res = rt_thread_startup(handle);
    if (res != RT_EOK)
    {
        return -3;
    }

    if (task_handle)
        *task_handle = (rtos_task_handle)handle;

#ifdef CONFIG_RTOS_RESLIST
    aicwf_OsResInit(&st, (char *)name, *task_handle);
    st.para0 = (uintptr_t)stack_base;
    st.para1 = stack_depth;
    st.type = RES_TYPE_TASK;

    DBG_OSAL_INF("[rtos]%s '%s', handle=%#8x, stack_base=%#8x, stack_size=%#8x\n",
                 __func__, name, *task_handle, stack_base, stack_depth);

    aicwf_rtos_res_list_push(&st);
#endif

    return 0;
}

/**
 * @brief 创建RTOS任务（不带资源追踪）
 *
 * 与 task_create 类似但不注册到资源列表。
 * 调用者需确保 task_handle 非NULL。
 *
 * @return 0=成功, -3=创建失败
 */
int aicwf_rtos_task_create_only(rtos_task_fct func,
                                const char *const name,
                                int task_id,
                                const uint16_t stack_depth,
                                void *const params,
                                rtos_prio prio,
                                rtos_task_handle *task_handle)
{
    rt_thread_t handle;

    AIC_ASSERT_ERR(task_handle != NULL);
    handle = rt_thread_create(name, func, params, stack_depth, prio, 20);
    if (handle == RT_NULL)
    {
        return -3;
    }

    *task_handle = (rtos_task_handle)handle;

    rt_thread_startup(handle);

    return 0;
}

/**
 * @brief 删除RTOS任务
 *
 * 删除指定任务。RT-Thread 不允许线程自我删除，
 * 若传入NULL或以当前任务句柄调用则仅从资源列表移除。
 *
 * @param task_handle 任务句柄（NULL=删除当前任务）
 */
void aicwf_rtos_task_delete(rtos_task_handle task_handle)
{
    rtos_task_handle hdl = task_handle;
    if ((hdl == NULL) || (hdl == aicwf_rtos_get_current_task()))
    {
        hdl = aicwf_rtos_get_current_task();
        {
            DBG_OSAL_ERR("aicwf_rtos_task_delete self\n");
            // RT-Thread: 线程不能自我删除，仅从资源列表移除
            // TODO: [AIC8800-PORT] 若确有自删需求，通知其他线程代为删除
#ifdef CONFIG_RTOS_RESLIST
            aicwf_rtos_res_list_rellist(task_handle, RES_TYPE_TASK);
#endif
            return;
        }
    }

#ifdef CONFIG_RTOS_RESLIST
    aicwf_rtos_res_list_release(task_handle, RES_TYPE_TASK);
#else
    rt_thread_delete((rt_thread_t)task_handle);
#endif
}

/**
 * @brief 检查任务是否有效
 * @param task_handle 任务句柄
 * @return true=有效, false=无效
 */
bool aicwf_rtos_task_isvalid(rtos_task_handle task_handle)
{
    if (task_handle == NULL)
        return false;

#ifdef CONFIG_RTOS_RESLIST
    OsResSt *res = aicwf_rtos_res_find_res(task_handle, RES_TYPE_TASK);
    return (res != NULL);
#else
    return true;
#endif
}

/**
 * @brief 任务延时挂起
 * @param duration 延时毫秒数（<=0则不延时）
 */
void aicwf_rtos_task_suspend(int duration)
{
    if (duration <= 0)
        return;

    rt_thread_mdelay(duration);
}

/**
 * @brief 恢复挂起的任务
 * @param task_handle 任务句柄
 */
void aicwf_rtos_task_resume(rtos_task_handle task_handle)
{
    rt_thread_resume((rt_thread_t)task_handle);
}

/**
 * @brief 获取任务优先级
 * @param task_handle 任务句柄
 * @return 优先级值
 */
uint32_t aicwf_rtos_task_get_priority(rtos_task_handle task_handle)
{
    uint8_t priority = 0;
    priority = ((rt_thread_t)task_handle)->current_priority;

    return priority;
}

/**
 * @brief 设置任务优先级
 * @param task_handle 任务句柄
 * @param priority    优先级值
 */
void aicwf_rtos_task_set_priority(rtos_task_handle task_handle, uint32_t priority)
{
    rt_thread_control((rt_thread_t)task_handle, RT_THREAD_CTRL_CHANGE_PRIORITY, &priority);
}

/**
 * @brief 创建消息队列
 *
 * 创建指定元素大小和数量的消息队列，开启资源追踪时记录到资源列表。
 *
 * @param elt_size  单个元素的字节数
 * @param nb_elt    队列最大元素数量
 * @param queue     输出队列句柄
 * @param name      队列名称（用于调试和资源追踪）
 * @return 0=成功, -1=创建失败
 */
int aicwf_rtos_queue_create(int elt_size, int nb_elt, rtos_queue *queue, const char *const name)
{
    OsResSt st;

    AIC_ASSERT_ERR(queue != NULL);

    *queue = rt_mq_create(name, elt_size, nb_elt, RT_IPC_FLAG_FIFO);

    if (*queue == NULL)
        return -1;

#ifdef CONFIG_RTOS_RESLIST
    aicwf_OsResInit(&st, (char *)name, *queue);
    st.type = RES_TYPE_Q;
    st.para0 = nb_elt;
    st.para1 = elt_size;

#if RTOS_AL_INFO_DUMP
    DBG_OSAL_INF("[rtos]%s '%s', queue=%#x\n", __func__, name, *queue);
#endif

    aicwf_rtos_res_list_push(&st);
#endif

    return 0;
}

/**
 * @brief 删除消息队列
 *
 * 释放队列资源，开启资源追踪时从列表中移除。
 *
 * @param queue 队列句柄
 */
void aicwf_rtos_queue_delete(rtos_queue queue)
{
#ifdef CONFIG_RTOS_RESLIST
    aicwf_rtos_res_list_release(queue, RES_TYPE_Q);
#else
    rt_mq_delete((rt_mq_t)queue);
#endif
}

/**
 * @brief 检查队列是否为空
 * @param queue 队列句柄
 * @return true=空, false=非空
 */
bool aicwf_rtos_queue_is_empty(rtos_queue queue)
{
    rt_mq_t q = (rt_mq_t)queue;
    return (q->entry == 0);
}

/**
 * @brief 检查队列是否已满
 * @param queue 队列句柄
 * @return true=满, false=未满
 */
bool aicwf_rtos_queue_is_full(rtos_queue queue)
{
    rt_mq_t q = (rt_mq_t)queue;
    return (q->entry >= q->max_msgs);
}

/**
 * @brief 获取队列中待处理消息数量
 * @param queue 队列句柄
 * @return 当前队列中的消息数量（RT-Thread 不支持则返回 -1）
 */
int aicwf_rtos_queue_cnt(rtos_queue queue)
{
    rt_mq_t q = (rt_mq_t)queue;
    return (int)q->entry;
}

/**
 * @brief 向队列写入消息
 *
 * 将消息追加到队列尾部，支持超时等待（任务上下文）。
 * RT-Thread 的 rt_mq_send 不支持 ISR，中断上下文直接返回失败。
 *
 * @param queue   队列句柄
 * @param size    消息大小
 * @param msg     指向要发送的消息数据
 * @param timeout 超时毫秒数（仅任务上下文有效）
 * @param isr     true=ISR上下文调用
 * @return 1=失败, 0=成功
 */
int aicwf_rtos_queue_write(rtos_queue queue, uint32_t size, void *msg, int timeout, bool isr)
{
    rt_err_t res;

    if (queue == NULL)
    {
        return 1;
    }

    if (isr)
    {
        /* ISR 上下文不支持等待，非阻塞发送 */
        res = rt_mq_send((rt_mq_t)queue, msg, size);
    }
    else
    {
        /* [AIC8800-PORT][SiFli] RT-Thread 3.1.3 无 rt_mq_send_wait(4.x+才有); 用 rt_mq_send
         * (非阻塞, 队满 -RT_EFULL)。TODO[HW]: 需阻塞超时语义时加重试+mdelay 循环。 */
        (void)timeout;
        res = rt_mq_send((rt_mq_t)queue, msg, size);
    }

    return (res != RT_EOK);
}

/**
 * @brief 在定时器回调中向队列写入消息
 *
 * 定时器回调在任务上下文执行，与 aicwf_rtos_queue_write 逻辑一致。
 *
 * @param queue   队列句柄
 * @param size    消息大小
 * @param msg     指向要发送的消息数据
 * @param timeout 超时毫秒数
 * @param isr     true=ISR上下文调用
 * @return 1=失败, 0=成功
 */
int aicwf_rtos_queue_write_intimer(rtos_queue queue, uint32_t size, void *msg, int timeout, bool isr)
{
    /* 定时器回调在任务上下文执行，支持超时等待 */
    rt_err_t res;
    (void)isr;
    (void)timeout;   /* [AIC8800-PORT][SiFli] RT-Thread 3.1.3 无 rt_mq_send_wait; 见下 */

    if (queue == NULL)
    {
        return 1;
    }

    /* [AIC8800-PORT][SiFli] rt_mq_send_wait(4.x+) → rt_mq_send(非阻塞)。TODO[HW]: 需阻塞语义时加重试 */
    res = rt_mq_send((rt_mq_t)queue, msg, size);

    return (res != RT_EOK);
}

/**
 * @brief 从队列读取消息
 *
 * 从队列头部取出一条消息，支持超时等待（任务上下文）。
 *
 * @param queue   队列句柄
 * @param size    消息大小
 * @param msg     输出缓冲区，用于存放读取到的消息
 * @param timeout 超时毫秒数（仅任务上下文有效）
 * @param isr     true=ISR上下文调用
 * @return 1=失败（超时/空）, 0=成功
 */
int aicwf_rtos_queue_read(rtos_queue queue, uint32_t size, void *msg, int timeout, bool isr)
{
    if (queue == NULL)
    {
        return 1;
    }

    if (isr)
    {
        /* ISR 上下文不支持等待，非阻塞读取 */
        rt_err_t res = rt_mq_recv((rt_mq_t)queue, msg, size, 0);
        return (res != RT_EOK);
    }
    else
    {
#if (defined(RTTHREAD_VERSION) && (RTTHREAD_VERSION >= 50001))  /* [AIC8800-PORT][SiFli] 同上, 5.0.1=50001; SiFli RT-Thread 3.1.3 走 #else (rt_mq_recv 返回 rt_err_t) */
        /* RT-Thread 5.0.1+ : rt_mq_recv 返回 rt_ssize_t (>0=成功) */
        rt_ssize_t ret_len = rt_mq_recv((rt_mq_t)queue, msg, size, rtos_timeout_2_tickcount(timeout));
        return (ret_len <= 0);
#else
        rt_err_t res = rt_mq_recv((rt_mq_t)queue, msg, size, rtos_timeout_2_tickcount(timeout));
        return (res != RT_EOK);
#endif
    }
}


/**
 * @brief 创建信号量
 *
 * 使用 rt_sem_create 创建，RT-Thread 信号量初始值即为驱动期望值。
 *
 * @param semaphore 输出信号量句柄
 * @param name      信号量名称（用于调试和资源追踪）
 * @param max_count 最大计数值
 * @param init_count 初始计数值
 * @return 0=成功, -1=创建失败
 */
int aicwf_rtos_semaphore_create(rtos_semaphore *semaphore, const char *const name, int max_count, int init_count)
{
    OsResSt st;
    int res = -1;

    if (semaphore == NULL)
    {
        DBG_OSAL_ERR("[rtos]semaphore_create: NULL output pointer\n");
        return -1;
    }

    // RT-Thread rt_sem_create 直接设置初始计数值
    *semaphore = rt_sem_create(name, init_count, RT_IPC_FLAG_FIFO);
    if (*semaphore != RT_NULL)
    {
        res = 0;
    }

#ifdef CONFIG_RTOS_RESLIST
    if (res == 0)
    {
        aicwf_OsResInit(&st, (char *)name, *semaphore);
        st.type = RES_TYPE_SEM;

#if RTOS_AL_INFO_DUMP
        DBG_OSAL_INF("[rtos]%s '%s', sema=%#x\n", __func__, name, *semaphore);
#endif

        aicwf_rtos_res_list_push(&st);
    }
#endif

    return res;
}

/**
 * @brief 创建信号量（无资源追踪）
 *
 * 与 aicwf_rtos_semaphore_create 相同逻辑，但不注册到资源列表。
 *
 * @param semaphore 输出信号量句柄
 * @param name      信号量名称
 * @param max_count 最大计数值
 * @param init_count 初始计数值
 * @return 0=成功, -1=创建失败
 */
int aicwf_rtos_semaphore_create_only(rtos_semaphore *semaphore, const char *const name, int max_count, int init_count)
{
    int res = -1;

    *semaphore = rt_sem_create(name, init_count, RT_IPC_FLAG_PRIO);

    if (*semaphore != RT_NULL)
    {
        res = 0;
    }

    return res;
}

/**
 * @brief 删除信号量
 *
 * 释放信号量资源，开启资源追踪时从列表中移除。
 *
 * @param semaphore 信号量句柄
 */
void aicwf_rtos_semaphore_delete(rtos_semaphore semaphore)
{
    if (semaphore == NULL)
        return;

#ifdef CONFIG_RTOS_RESLIST
    aicwf_rtos_res_list_release(semaphore, RES_TYPE_SEM);
#else
    rt_sem_delete((rt_sem_t)semaphore);
#endif
}

/**
 * @brief 获取信号量当前计数值
 * @param semaphore 信号量句柄
 * @return 当前可用令牌数（直接读 rt_sem_t->value）
 */
int aicwf_rtos_semaphore_get_count(rtos_semaphore semaphore)
{
    if (semaphore == NULL)
        return 0;

    return (int)((rt_sem_t)semaphore)->value;
}

/**
 * @brief 获取信号量当前计数值（中断安全版本）
 * @note  RTT 直接读 rt_sem_t->value 为纯读操作, 天然 ISR 安全,
 *        与 get_count 同实现, 仅为统一 port 接口
 * @param semaphore 信号量句柄
 * @return 当前计数值
 */
int aicwf_rtos_semaphore_get_count_fromisr(rtos_semaphore semaphore)
{
    if (semaphore == NULL)
        return 0;

    return (int)((rt_sem_t)semaphore)->value;
}

/**
 * @brief 等待信号量
 *
 * 尝试获取信号量，在指定超时时间内等待。
 *
 * @param semaphore 信号量句柄
 * @param timeout   超时毫秒数
 * @return 1=超时失败, 0=成功获取
 */
int aicwf_rtos_semaphore_wait(rtos_semaphore semaphore, int timeout)
{
    rt_err_t res = RT_EOK;

    if (semaphore == NULL)
    {
        DBG_OSAL_INF("[rtos]wait semaphore null\n");
        rt_thread_mdelay(1);   /* 防 NULL 信号量紧密空转刷 CPU */
        return 1;
    }

    res = rt_sem_take((rt_sem_t)semaphore, rtos_timeout_2_tickcount(timeout));

    return (res == -RT_ETIMEOUT);
}

/**
 * @brief 释放信号量
 *
 * 增加信号量计数值。RT-Thread 的 rt_sem_release ISR 安全。
 *
 * @param semaphore 信号量句柄
 * @param isr       true=ISR上下文调用（RT-Thread 中 rt_sem_release 本身 ISR 安全）
 * @return 1=失败, 0=成功
 */
int aicwf_rtos_semaphore_signal(rtos_semaphore semaphore, bool isr)
{
    rt_err_t res;

    if (semaphore == NULL)
    {
        DBG_OSAL_INF("[rtos]signal semaphore null\n");
        return 1;
    }

    // rt_sem_release 在 ISR 和非 ISR 上下文均可调用
    res = rt_sem_release((rt_sem_t)semaphore);
    (void)isr;

    return (res != RT_EOK);
}

/**
 * @brief 创建软件定时器
 *
 * 初始化定时器资源，开启资源追踪时注册到资源列表。
 * 实际 RT-Thread 定时器对象在 start 时延迟创建。
 *
 * @param timer 输出定时器句柄
 * @param name  定时器名称（用于调试）
 * @return 0=成功
 */
int aicwf_rtos_timer_create(rtos_timer *timer, char *name)
{
    int ret = 0;

    *timer = NULL;

#ifdef CONFIG_RTOS_RESLIST
    if (ret == 0)
    {
        OsResSt st;
        aicwf_OsResInit(&st, (char *)name, *timer);
        rtos_res_time_cnt++;
        st.type = RES_TYPE_TIME;
        *timer = (rtos_timer)(aicwf_rtos_res_list_push(&st));
        DBG_OSAL_INF("create timer %s, timer=%#x\n", name, *timer);
    }
#endif

    return 0;
}

/**
 * @brief 启动软件定时器
 *
 * 若实际定时器对象未创建则延迟创建，然后启动。
 * RT-Thread 使用一次性定时器模拟。
 *
 * @param timer           定时器句柄
 * @param initialTime     初始延迟时间（未使用，period 直接作为延迟）
 * @param rescheduleTime  定时器周期（毫秒）
 * @param func            定时器回调函数
 * @param args            回调参数
 * @return 0=成功, -1=失败
 */
int aicwf_rtos_timer_start(rtos_timer timer, uint32_t initialTime, uint32_t rescheduleTime, rtos_timer_fct func, void *const args)
{
    rt_err_t res = RT_EOK;
    rt_timer_t xTimer = NULL;

#ifdef CONFIG_RTOS_RESLIST
    OsResList *p = (OsResList *)timer;
    if (p->res.ref == RT_NULL)
    {
        DBG_OSAL_INF("aicwf_rtos_timer_start %s\n", p->res.name);
        xTimer = rt_timer_create(p->res.name, (void (*)(void *))func, args,
                                 rtos_timeout_2_tickcount(rescheduleTime), RT_TIMER_FLAG_ONE_SHOT);
        if (xTimer == RT_NULL)
        {
            DBG_OSAL_ERR("%s: create timer %s fail\n", __func__, p->res.name);
            return -1;
        }
        p->res.ref = xTimer;
    }
    else
    {
        xTimer = (rt_timer_t)p->res.ref;
    }
#else
    xTimer = (rt_timer_t)timer;
#endif
    res = rt_timer_start(xTimer);
    if (res != RT_EOK)
    {
        DBG_OSAL_ERR("%s: start timer %s fail\n", __func__, res == RT_EOK ? "ok" : "fail");
        return -1;
    }

    return (res != RT_EOK);
}

/**
 * @brief 获取定时器状态
 *
 * @param timer        定时器句柄
 * @param timer_status 输出定时器状态
 * @return 始终返回 AIC_RTOS_TIMER_STATUS_ACTIVE
 */
int aicwf_rtos_timer_get_status(rtos_timer timer, rtos_timer_status *timer_status)
{
    (void)timer;
    (void)timer_status;
    return AIC_RTOS_TIMER_STATUS_ACTIVE;
}

/**
 * @brief 停止软件定时器
 *
 * @param timer 定时器句柄
 * @return 0=成功, -1=失败或无效句柄
 */
int aicwf_rtos_timer_stop(rtos_timer timer)
{
    if (timer == NULL)
    {
        DBG_OSAL_INF("%s: NULL timer\n", __func__);
        return -1;
    }
    rt_timer_t xTimer = NULL;
#ifdef CONFIG_RTOS_RESLIST
    OsResList *p = (OsResList *)timer;
    if (p->res.ref != NULL)
    {
        xTimer = (rt_timer_t)p->res.ref;
    }
    else
    {
        DBG_OSAL_ERR("%s: invalid timer %s\n", __func__, p->res.name);
        return -1;
    }
#else
    xTimer = (rt_timer_t)timer;
#endif

    rt_err_t res = RT_EOK;
    res = rt_timer_stop(xTimer);

    if (res != RT_EOK)
    {
        DBG_OSAL_ERR("%s: stop timer fail\n", __func__);
        return -1;
    }

    return (res != RT_EOK);
}

/**
 * @brief 在ISR中停止软件定时器
 *
 * @param timer 定时器句柄
 * @return 0=成功, -1=失败或无效句柄
 */
int aicwf_rtos_timer_stop_isr(rtos_timer timer)
{
    rt_err_t res = RT_EOK;

    if (timer == NULL)
    {
        DBG_OSAL_INF("%s: NULL timer\n", __func__);
        return -1;
    }

    rt_timer_t xTimer = NULL;
#ifdef CONFIG_RTOS_RESLIST
    OsResList *p = (OsResList *)timer;
    if (p->res.ref != NULL)
    {
        xTimer = (rt_timer_t)p->res.ref;
    }
#endif
    (void)timer;

    res = rt_timer_stop(xTimer);
    return (res != RT_EOK);
}

/**
 * @brief 删除软件定时器
 *
 * 停止并释放定时器资源，开启资源追踪时从列表中移除。
 *
 * @param timer 定时器句柄
 * @return 0=成功, -1=无效句柄
 */
int aicwf_rtos_timer_delete(rtos_timer timer)
{
    if (timer == NULL)
    {
        DBG_OSAL_INF("%s: NULL timer\n", __func__);
        return -1;
    }
    rt_timer_t xTimer = NULL;

#ifdef CONFIG_RTOS_RESLIST
    OsResList *p = (OsResList *)timer;
    if (p->res.ref != NULL)
    {
        xTimer = (rt_timer_t)p->res.ref;
        rt_timer_delete(xTimer);
        aicwf_rtos_res_list_release(xTimer, RES_TYPE_TIME);
    }
    else
    {
        uint16_t type = p->res.para0 << 8 | RES_TYPE_TIME;
        aicwf_rtos_res_list_release(xTimer, type);
    }
#else
    // timer 本身可能已经是 rt_timer_t 句柄
    xTimer = (rt_timer_t)timer;
    rt_timer_delete(xTimer);
#endif

    return 0;
}

/**
 * @brief 创建互斥量
 *
 * 创建互斥锁，开启资源追踪时记录到资源列表。
 *
 * @param mutex 输出互斥量句柄
 * @param name  互斥量名称（用于调试和资源追踪）
 * @return 0=成功, -1=创建失败
 */
int aicwf_rtos_mutex_create(rtos_mutex *mutex, const char *const name)
{
    OsResSt st;

    int ret = -1;

    *mutex = rt_mutex_create(name, RT_IPC_FLAG_PRIO);
    if (*mutex != 0)
    {
        ret = 0;
    }

#ifdef CONFIG_RTOS_RESLIST
    if (ret == 0)
    {
        aicwf_OsResInit(&st, (char *)name, *mutex);
        st.type = RES_TYPE_MUTEX;

#if RTOS_AL_INFO_DUMP
        DBG_OSAL_INF("[rtos]%s '%s', mutex=%#x\n", __func__, name, *mutex);
#endif

        aicwf_rtos_res_list_push(&st);
    }
#endif

    return ret;
}

/**
 * @brief 创建互斥量（无资源追踪）
 *
 * 与 aicwf_rtos_mutex_create 相同逻辑，但不注册到资源列表。
 *
 * @param mutex 输出互斥量句柄
 * @param name  互斥量名称
 * @return 0=成功, -1=创建失败
 */
int aicwf_rtos_mutex_create_only(rtos_mutex *mutex, const char *const name)
{

    int ret = -1;

    *mutex = rt_mutex_create(name, RT_IPC_FLAG_PRIO);
    if (*mutex != 0)
    {
        ret = 0;
    }

    return ret;
}

/**
 * @brief 删除互斥量
 *
 * 释放互斥量资源，开启资源追踪时从列表中移除。
 *
 * @param mutex 互斥量句柄
 */
void aicwf_rtos_mutex_delete(rtos_mutex mutex)
{
#ifdef CONFIG_RTOS_RESLIST
    aicwf_rtos_res_list_release(mutex, RES_TYPE_MUTEX);
#else
    rt_mutex_delete((rt_mutex_t)mutex);
#endif
}

/**
 * @brief 锁定互斥量
 *
 * @param mutex   互斥量句柄
 * @param timeout 超时毫秒数
 * @return 0=成功, -1=NULL句柄, 1=超时失败
 */
int aicwf_rtos_mutex_lock(rtos_mutex mutex, int timeout)
{
    if (mutex == NULL)
    {
        DBG_OSAL_INF("[rtos]lock mutex null\n");
        return -1;
    }
    rt_err_t res = RT_EOK;

    res = rt_mutex_take((rt_mutex_t)mutex, rtos_timeout_2_tickcount(timeout));

    return (res != RT_EOK);
}

/**
 * @brief 解锁互斥量
 *
 * @param mutex 互斥量句柄
 * @return 0=成功, -1=NULL句柄, 1=失败
 */
int aicwf_rtos_mutex_unlock(rtos_mutex mutex)
{
    if (mutex == NULL)
    {
        DBG_OSAL_INF("[rtos]unlock mutex null\n");
        return -1;
    }

    rt_err_t res = RT_EOK;

    res = rt_mutex_release((rt_mutex_t)mutex);

    return (res != RT_EOK);
}

/**
 * @brief 创建事件组
 *
 * @param event_group 输出事件组句柄
 * @return 0=成功, -1=创建失败
 */
int aicwf_rtos_event_group_create(rtos_event_group *event_group)
{
    int res = -1;

    *event_group = rt_event_create("ev_grp", RT_IPC_FLAG_FIFO);
    if (*event_group != 0)
    {
        res = 0;
    }

    return res;
}

/**
 * @brief 删除事件组
 * @param event_group 事件组句柄
 */
void aicwf_rtos_event_group_delete(rtos_event_group event_group)
{
    if (event_group)
        rt_event_delete((rt_event_t)event_group);
}

/**
 * @brief 获取事件组当前位标志
 *
 * @param event_group 事件组句柄
 * @param isr         true=ISR上下文调用（rt_event_recv 不支持 ISR，但 timeout=0 仅快照当前值）
 * @return 当前事件位标志值
 */
uint32_t aicwf_rtos_event_group_get_bits(rtos_event_group event_group, bool isr)
{
    uint32_t recved = 0;
    (void)isr;

    if (event_group)
    {
        rt_event_recv((rt_event_t)event_group, 0xFFFFFFFF,
                      RT_EVENT_FLAG_OR, 0, &recved);
    }
    return recved;
}

/**
 * @brief 等待事件组指定位标志
 *
 * @param event_group    事件组句柄
 * @param val            要等待的事件位掩码
 * @param clear_on_exit  true=退出时自动清除匹配位
 * @param wait_all_bits  true=等待所有位置位, false=任意位置位即可
 * @param timeout        超时毫秒数
 * @return 返回时的事件位标志值
 */
uint32_t aicwf_rtos_event_group_wait_bits(rtos_event_group event_group, const uint32_t val,
        const bool clear_on_exit, const bool wait_all_bits, int timeout)
{
    if (!event_group)
        return 0;

    rt_uint8_t opt = 0;
    uint32_t recved = 0;
    rt_err_t res;

    if (wait_all_bits)
        opt |= RT_EVENT_FLAG_AND;
    else
        opt |= RT_EVENT_FLAG_OR;

    if (clear_on_exit)
        opt |= RT_EVENT_FLAG_CLEAR;

    res = rt_event_recv((rt_event_t)event_group, val, opt,
                        rtos_timeout_2_tickcount(timeout), &recved);

    if (res == RT_EOK)
        return recved;
    return 0;
}

/**
 * @brief 设置事件组位标志
 *
 * @param event_group 事件组句柄
 * @param val         要置位的位掩码
 * @param isr         true=ISR上下文调用
 * @return 设置后的事件位标志值
 */
uint32_t aicwf_rtos_event_group_set_bits(rtos_event_group event_group, const uint32_t val, bool isr)
{
    if (event_group)
    {
        rt_event_send((rt_event_t)event_group, val);
        return aicwf_rtos_event_group_get_bits(event_group, isr);
    }
    return 0;
}


/**
 * @brief 获取当前时间
 *
 * @param origin 时间原点（未使用，总是使用基于 tick 的时间）
 * @param sec    输出秒数
 * @param usec   输出微秒数
 * @return 0=成功
 */
int aicwf_time_get(enum time_origin_t origin, uint32_t *sec, uint32_t *usec)
{
    rt_tick_t tick;
    tick = rt_tick_get();

    *sec = tick / RT_TICK_PER_SECOND;
    *usec = (tick % RT_TICK_PER_SECOND) * (1000000 / RT_TICK_PER_SECOND);

    return 0;
}

/**
 * @brief 初始化随机数生成器
 *
 * 使用当前时间戳作为随机种子（需启用 CONFIG_MBEDTLS_V3）。
 */
void aicwf_random_init(void)
{
#if defined(CONFIG_MBEDTLS_V3)
    srand((unsigned int)aicwf_rtos_now(0));
#endif
}

/**
 * @brief 生成随机字节
 *
 * @return 0~255 的随机值（未启用 CONFIG_MBEDTLS_V3 时始终返回 0）
 */
uint8_t aicwf_random_byte(void)
{
#if defined(CONFIG_MBEDTLS_V3)
    int min = 0, max = 255;
    return rand() % (max - min + 1) + min;
#else
    return 0;
#endif
}

/* ============================================================
 * Missing RTOS functions - stub implementations
 * ============================================================ */

/**
 * @brief 初始化任务通知机制（存根）
 *
 * @param task 任务句柄
 * @return 0（始终成功，当前为空实现）
 */
int aicwf_rtos_task_init_notification(rtos_task_handle task)
{
    (void)task;
    return 0;
}

/**
 * @brief 等待任务通知（存根）
 *
 * @param timeout 超时毫秒数
 * @return 0（当前为空实现）
 */
uint32_t aicwf_rtos_task_wait_notification(int timeout)
{
    (void)timeout;
    return 0;
}

/**
 * @brief 发送任务通知（存根）
 *
 * @param task_handle 目标任务句柄
 * @param value       通知值
 * @param isr         true=ISR上下文调用
 */
void aicwf_rtos_task_notify(rtos_task_handle task_handle, uint32_t value, bool isr)
{
    (void)task_handle;
    (void)value;
    (void)isr;
}

/**
 * @brief 设置任务通知位（存根）
 *
 * @param task_handle 目标任务句柄
 * @param value       通知位掩码
 * @param isr         true=ISR上下文调用
 */
void aicwf_rtos_task_notify_setbits(rtos_task_handle task_handle, uint32_t value, bool isr)
{
    (void)task_handle;
    (void)value;
    (void)isr;
}

/**
 * @brief 进入 ISR-safe 临界区
 *
 * 用于 USB URB complete(ISR) 与 task 共享队列的短临界区保护。
 * 返回进入前的 CPU 中断掩码，退出时传给 aicwf_rtos_critical_exit 恢复。
 * RT-Thread: rt_hw_interrupt_disable 关闭全局中断, task 与 ISR 上下文均安全。
 *
 * @return 进入临界区前的中断掩码 (rt_base_t)
 */
uint32_t aicwf_rtos_critical_enter(void)
{
    return (uint32_t)rt_hw_interrupt_disable();
}

/**
 * @brief 退出 ISR-safe 临界区
 *
 * @param level aicwf_rtos_critical_enter 返回的中断掩码
 */
void aicwf_rtos_critical_exit(uint32_t level)
{
    rt_hw_interrupt_enable((rt_base_t)level);
}

/**
 * @brief 打印内存资源列表信息（存根）
 *
 * 当前为空实现，预留用于调试输出资源使用情况。
 */
void aicwf_rtos_mem_list_info(void)
{
    /* No-op stub */
}
