/* 仅供宿主机回归：保留 hilog 调用签名，不依赖 OHOS 服务。 */
#ifndef MEOW_TEST_HILOG_LOG_H
#define MEOW_TEST_HILOG_LOG_H
#define LOG_APP 0
#define LOG_DEBUG 3
#define LOG_INFO 4
#define LOG_WARN 5
#define LOG_ERROR 6
static inline int OH_LOG_Print(int type, int level, unsigned int domain,
                               const char* tag, const char* format, ...) {
    (void)type; (void)level; (void)domain; (void)tag; (void)format;
    return 0;
}
#endif
