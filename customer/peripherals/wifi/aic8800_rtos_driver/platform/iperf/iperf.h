/**
 * iperf2-style network performance tool (TCP/UDP, server/client).
 * 实现移植自 t3x 平台树, 详见 iperf.c 文件头注释。
 *
 * 入口由 wifi_port_cmd.c 的 iwpriv iperf start/stop 命令调用:
 *   handle = iperf_parse_argv(argc, argv)   0=成功 -1=参数错 -2=已就地处理
 *   iperf_start(nif, handle)                启动流量/报告任务
 *   iperf_stop("a")                         停止运行中的实例
 */

#ifndef _IPERF_H_
#define _IPERF_H_

#if 1 //PRJCONF_NET_EN

#include "lwip/netif.h"
#include "lwip/inet.h"
#include "rtos_port.h"

#ifdef __cplusplus
extern "C" {
#endif

int iperf_start(struct netif *nif, int handle);
int iperf_stop(char *arg);
int iperf_parse_argv(int argc, char *argv[]);
int iperf_handle_free(int handle);
int iperf_handle_start(struct netif *nif, int handle);

#ifdef __cplusplus
}
#endif

#endif /* PRJCONF_NET_EN */
#endif /* _IPERF_H_ */
