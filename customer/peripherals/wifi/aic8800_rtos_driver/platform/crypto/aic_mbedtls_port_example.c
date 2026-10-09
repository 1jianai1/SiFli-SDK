/*
 * ================================================================
 * aic_mbedtls_port_example.c — mbedTLS crypto 接口外部实现示例
 *
 * 用途 (USE_BUILTIN_MBEDTLS=0, 见 platform_config.mk "mbedTLS Crypto 库选择"):
 *   当平台 SDK 已自带标准 mbedTLS (≥3.6) 时, 无需链接内置 libaic_mbedtls.a,
 *   只需提供 35 个 aic_mbedtls_* 符号 (22 MPI + 13 ECP)。
 *
 * 接口映射关系 (核心思路):
 *   aic_mbedtls_* 接口 = 标准 mbedtls API + "aic_" 前缀, 函数签名完全一致。
 *   因此本示例的每个函数都是"去掉 aic_ 前缀直接转发"标准 mbedtls 接口:
 *     aic_mbedtls_mpi_init(X)      → mbedtls_mpi_init(X)
 *     aic_mbedtls_ecp_group_load() → mbedtls_ecp_group_load()
 *   完整契约见 host/thirdparty/mbedtls_v3_6_2/include/aic_mbedtls_crypto_if.h
 *
 * 使用方法:
 *   1. platform_config.mk: USE_BUILTIN_MBEDTLS=0
 *   2. SRC_PLATFORM += $(PLATFORM_PATH)/crypto/aic_mbedtls_port_example.c
 *      (USE_BUILTIN_MBEDTLS=0 时顶层 Makefile 默认编入本文件,
 *       AIC_MBEDTLS_FWD_EXAMPLE=0 可关闭, 改为完全自主实现)
 *   3. EXT_INC_DIRS += <平台 mbedtls 头文件路径> (能 #include <mbedtls/bignum.h>)
 *   4. SDK 链接平台 mbedtls 库 (不再链接 libaic_mbedtls.a)
 *
 * 注意: 本文件面向平台自带的"标准 mbedtls" (函数名为 mbedtls_* 原名)。
 *       驱动内置的 vendored mbedtls (host/thirdparty/mbedtls_v3_6_2) 的
 *       函数名本身已带 aic_ 前缀, 不能与本文件混用。
 * ================================================================
 */

#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"

/* ================================================================
 * 第一部分: 大数运算接口 (MPI) — 22 个
 * 每个函数 → 去掉 aic_ 前缀的同名标准 mbedtls 接口
 * ================================================================ */

/* --- 生命周期 --- */

/* aic_mbedtls_mpi_init → mbedtls_mpi_init */
void aic_mbedtls_mpi_init(mbedtls_mpi *X)
{
    mbedtls_mpi_init(X);
}

/* aic_mbedtls_mpi_free → mbedtls_mpi_free */
void aic_mbedtls_mpi_free(mbedtls_mpi *X)
{
    mbedtls_mpi_free(X);
}

/* --- 复制与赋值 --- */

/* aic_mbedtls_mpi_copy → mbedtls_mpi_copy */
int aic_mbedtls_mpi_copy(mbedtls_mpi *X, const mbedtls_mpi *Y)
{
    return mbedtls_mpi_copy(X, Y);
}

/* aic_mbedtls_mpi_lset → mbedtls_mpi_lset */
int aic_mbedtls_mpi_lset(mbedtls_mpi *X, mbedtls_mpi_sint z)
{
    return mbedtls_mpi_lset(X, z);
}

/* aic_mbedtls_mpi_safe_cond_assign → mbedtls_mpi_safe_cond_assign */
int aic_mbedtls_mpi_safe_cond_assign(mbedtls_mpi *X, const mbedtls_mpi *Y,
                                     unsigned char assign)
{
    return mbedtls_mpi_safe_cond_assign(X, Y, assign);
}

/* --- 序列化 --- */

/* aic_mbedtls_mpi_read_binary → mbedtls_mpi_read_binary */
int aic_mbedtls_mpi_read_binary(mbedtls_mpi *X, const unsigned char *buf,
                                size_t buflen)
{
    return mbedtls_mpi_read_binary(X, buf, buflen);
}

/* aic_mbedtls_mpi_write_binary → mbedtls_mpi_write_binary */
int aic_mbedtls_mpi_write_binary(const mbedtls_mpi *X, unsigned char *buf,
                                 size_t buflen)
{
    return mbedtls_mpi_write_binary(X, buf, buflen);
}

/* --- 查询 --- */

/* aic_mbedtls_mpi_size → mbedtls_mpi_size */
size_t aic_mbedtls_mpi_size(const mbedtls_mpi *X)
{
    return mbedtls_mpi_size(X);
}

/* aic_mbedtls_mpi_bitlen → mbedtls_mpi_bitlen */
size_t aic_mbedtls_mpi_bitlen(const mbedtls_mpi *X)
{
    return mbedtls_mpi_bitlen(X);
}

/* --- 比较 --- */

/* aic_mbedtls_mpi_cmp_mpi → mbedtls_mpi_cmp_mpi */
int aic_mbedtls_mpi_cmp_mpi(const mbedtls_mpi *X, const mbedtls_mpi *Y)
{
    return mbedtls_mpi_cmp_mpi(X, Y);
}

/* aic_mbedtls_mpi_cmp_int → mbedtls_mpi_cmp_int */
int aic_mbedtls_mpi_cmp_int(const mbedtls_mpi *X, mbedtls_mpi_sint z)
{
    return mbedtls_mpi_cmp_int(X, z);
}

/* --- 移位 --- */

/* aic_mbedtls_mpi_shift_r → mbedtls_mpi_shift_r */
int aic_mbedtls_mpi_shift_r(mbedtls_mpi *X, size_t count)
{
    return mbedtls_mpi_shift_r(X, count);
}

/* --- 算术 --- */

/* aic_mbedtls_mpi_add_mpi → mbedtls_mpi_add_mpi */
int aic_mbedtls_mpi_add_mpi(mbedtls_mpi *X, const mbedtls_mpi *A,
                            const mbedtls_mpi *B)
{
    return mbedtls_mpi_add_mpi(X, A, B);
}

/* aic_mbedtls_mpi_sub_mpi → mbedtls_mpi_sub_mpi */
int aic_mbedtls_mpi_sub_mpi(mbedtls_mpi *X, const mbedtls_mpi *A,
                            const mbedtls_mpi *B)
{
    return mbedtls_mpi_sub_mpi(X, A, B);
}

/* aic_mbedtls_mpi_mul_mpi → mbedtls_mpi_mul_mpi */
int aic_mbedtls_mpi_mul_mpi(mbedtls_mpi *X, const mbedtls_mpi *A,
                            const mbedtls_mpi *B)
{
    return mbedtls_mpi_mul_mpi(X, A, B);
}

/* aic_mbedtls_mpi_div_mpi → mbedtls_mpi_div_mpi */
int aic_mbedtls_mpi_div_mpi(mbedtls_mpi *Q, mbedtls_mpi *R,
                            const mbedtls_mpi *A, const mbedtls_mpi *B)
{
    return mbedtls_mpi_div_mpi(Q, R, A, B);
}

/* aic_mbedtls_mpi_add_int → mbedtls_mpi_add_int */
int aic_mbedtls_mpi_add_int(mbedtls_mpi *X, const mbedtls_mpi *A,
                            mbedtls_mpi_sint b)
{
    return mbedtls_mpi_add_int(X, A, b);
}

/* aic_mbedtls_mpi_sub_int → mbedtls_mpi_sub_int */
int aic_mbedtls_mpi_sub_int(mbedtls_mpi *X, const mbedtls_mpi *A,
                            mbedtls_mpi_sint b)
{
    return mbedtls_mpi_sub_int(X, A, b);
}

/* aic_mbedtls_mpi_sub_abs → mbedtls_mpi_sub_abs */
int aic_mbedtls_mpi_sub_abs(mbedtls_mpi *X, const mbedtls_mpi *A,
                            const mbedtls_mpi *B)
{
    return mbedtls_mpi_sub_abs(X, A, B);
}

/* --- 模运算 --- */

/* aic_mbedtls_mpi_mod_mpi → mbedtls_mpi_mod_mpi */
int aic_mbedtls_mpi_mod_mpi(mbedtls_mpi *R, const mbedtls_mpi *A,
                            const mbedtls_mpi *B)
{
    return mbedtls_mpi_mod_mpi(R, A, B);
}

/* aic_mbedtls_mpi_exp_mod → mbedtls_mpi_exp_mod */
int aic_mbedtls_mpi_exp_mod(mbedtls_mpi *X, const mbedtls_mpi *A,
                            const mbedtls_mpi *E, const mbedtls_mpi *N,
                            mbedtls_mpi *prec_RR)
{
    return mbedtls_mpi_exp_mod(X, A, E, N, prec_RR);
}

/* aic_mbedtls_mpi_inv_mod → mbedtls_mpi_inv_mod */
int aic_mbedtls_mpi_inv_mod(mbedtls_mpi *X, const mbedtls_mpi *A,
                            const mbedtls_mpi *N)
{
    return mbedtls_mpi_inv_mod(X, A, N);
}

/* ================================================================
 * 第二部分: 椭圆曲线接口 (ECP) — 13 个
 * 每个函数 → 去掉 aic_ 前缀的同名标准 mbedtls 接口
 * ================================================================ */

/* --- 生命周期 --- */

/* aic_mbedtls_ecp_group_init → mbedtls_ecp_group_init */
void aic_mbedtls_ecp_group_init(mbedtls_ecp_group *grp)
{
    mbedtls_ecp_group_init(grp);
}

/* aic_mbedtls_ecp_group_free → mbedtls_ecp_group_free */
void aic_mbedtls_ecp_group_free(mbedtls_ecp_group *grp)
{
    mbedtls_ecp_group_free(grp);
}

/* aic_mbedtls_ecp_group_load → mbedtls_ecp_group_load */
int aic_mbedtls_ecp_group_load(mbedtls_ecp_group *grp, mbedtls_ecp_group_id id)
{
    return mbedtls_ecp_group_load(grp, id);
}

/* aic_mbedtls_ecp_point_init → mbedtls_ecp_point_init */
void aic_mbedtls_ecp_point_init(mbedtls_ecp_point *pt)
{
    mbedtls_ecp_point_init(pt);
}

/* aic_mbedtls_ecp_point_free → mbedtls_ecp_point_free */
void aic_mbedtls_ecp_point_free(mbedtls_ecp_point *pt)
{
    mbedtls_ecp_point_free(pt);
}

/* --- 点运算 --- */

/* aic_mbedtls_ecp_mul → mbedtls_ecp_mul */
int aic_mbedtls_ecp_mul(mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
                        const mbedtls_mpi *m, const mbedtls_ecp_point *P,
                        int (*f_rng)(void *, unsigned char *, size_t),
                        void *p_rng)
{
    return mbedtls_ecp_mul(grp, R, m, P, f_rng, p_rng);
}

/* aic_mbedtls_ecp_muladd → mbedtls_ecp_muladd */
int aic_mbedtls_ecp_muladd(mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
                           const mbedtls_mpi *m, const mbedtls_ecp_point *P,
                           const mbedtls_mpi *n, const mbedtls_ecp_point *Q)
{
    return mbedtls_ecp_muladd(grp, R, m, P, n, Q);
}

/* --- 序列化 --- */

/* aic_mbedtls_ecp_point_write_binary → mbedtls_ecp_point_write_binary */
int aic_mbedtls_ecp_point_write_binary(const mbedtls_ecp_group *grp,
                                       const mbedtls_ecp_point *P,
                                       int format, size_t *olen,
                                       unsigned char *buf, size_t buflen)
{
    return mbedtls_ecp_point_write_binary(grp, P, format, olen, buf, buflen);
}

/* aic_mbedtls_ecp_point_read_binary → mbedtls_ecp_point_read_binary */
int aic_mbedtls_ecp_point_read_binary(const mbedtls_ecp_group *grp,
                                      mbedtls_ecp_point *P,
                                      const unsigned char *buf, size_t ilen)
{
    return mbedtls_ecp_point_read_binary(grp, P, buf, ilen);
}

/* --- 判断 --- */

/* aic_mbedtls_ecp_check_pubkey → mbedtls_ecp_check_pubkey */
int aic_mbedtls_ecp_check_pubkey(const mbedtls_ecp_group *grp,
                                 const mbedtls_ecp_point *pt)
{
    return mbedtls_ecp_check_pubkey(grp, pt);
}

/* aic_mbedtls_ecp_is_zero → mbedtls_ecp_is_zero */
int aic_mbedtls_ecp_is_zero(mbedtls_ecp_point *pt)
{
    return mbedtls_ecp_is_zero(pt);
}

/* aic_mbedtls_ecp_point_cmp → mbedtls_ecp_point_cmp */
int aic_mbedtls_ecp_point_cmp(const mbedtls_ecp_point *P,
                              const mbedtls_ecp_point *Q)
{
    return mbedtls_ecp_point_cmp(P, Q);
}

/* aic_mbedtls_ecp_set_zero → mbedtls_ecp_set_zero */
int aic_mbedtls_ecp_set_zero(mbedtls_ecp_point *pt)
{
    return mbedtls_ecp_set_zero(pt);
}
