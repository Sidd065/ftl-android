#pragma once
#include <stdio.h>

#ifdef __ANDROID__
#include <android/log.h>
#define PORT_TAG "FTLPort"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, PORT_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, PORT_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PORT_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, "[I] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGW(...) do { fprintf(stderr, "[W] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { fprintf(stderr, "[E] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

extern int port_verbose;
#define LOGV(...) do { if (port_verbose) LOGI(__VA_ARGS__); } while (0)

_Noreturn void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
