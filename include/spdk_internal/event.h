/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_EVENT_H
#define SPDK_INTERNAL_EVENT_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/event.h"
#include "spdk/json.h"
#include "spdk/thread.h"
#include "spdk/util.h"

struct spdk_event {
	uint32_t		lcore;
	spdk_event_fn		fn;
	void			*arg1;
	void			*arg2;
};

enum spdk_reactor_state {
	SPDK_REACTOR_STATE_UNINITIALIZED = 0,
	SPDK_REACTOR_STATE_INITIALIZED = 1,
	SPDK_REACTOR_STATE_RUNNING = 2,
	SPDK_REACTOR_STATE_EXITING = 3,
	SPDK_REACTOR_STATE_SHUTDOWN = 4,
};

struct spdk_lw_thread;

/**
 * Completion callback to set reactor into interrupt mode or poll mode.
 *
 * \param cb_arg Argument to pass to the callback function.
 */
typedef void (*spdk_reactor_set_interrupt_mode_cb)(void *cb_arg1, void *cb_arg2);

struct spdk_reactor {
	/* Lightweight threads running on this reactor */
	TAILQ_HEAD(, spdk_lw_thread)			threads; // 轻量级线程队列,是linux意义的线程吗?
	uint32_t					thread_count; // 线程数量

	/* Logical core number for this reactor. */
	uint32_t					lcore; // 绑定的逻辑核心编号

	struct {
		uint32_t				is_valid : 1;
		uint32_t				reserved : 31;
	} flags;

	uint64_t					tsc_last; // 上一次处理事件的时间戳
	struct spdk_ring				*events; // 无锁环形事件队列,这里对于写入和读取都有head和tail指针,其中head指向当前可以读取或者写入的位置,tail指向写完/读完以后更新的位置.head通过cas保证同步,而tail通过顺序写入保证同步,
	int						events_fd; // 事件通知文件描述符

	/* The last known rusage values */
	struct rusage					rusage; // 上一次 rusage 信息
	uint64_t					last_rusage; // 上一次 rusage 更新时间

	uint64_t					busy_tsc; // 忙碌状态的时间戳
	uint64_t					idle_tsc; // 空闲状态的时间戳

	/* Each bit of cpuset indicates whether a reactor probably requires event notification */
	struct spdk_cpuset				notify_cpuset; // 通知 cpuset,用于指示哪些核心可能需要事件通知
	/* Indicate whether this reactor currently runs in interrupt mode */
	bool						in_interrupt; // 当前是否在中断模式
	bool						set_interrupt_mode_in_progress; // 是否正在设置中断模式
	bool						new_in_interrupt; // 新的中断模式
	spdk_reactor_set_interrupt_mode_cb		set_interrupt_mode_cb_fn; // 设置中断模式回调函数
	void						*set_interrupt_mode_cb_arg; // 设置中断模式回调函数参数

	struct spdk_fd_group				*fgrp; // 文件描述符组,用于中断模式下的事件监听
	int						resched_fd; // 重新调度文件描述符,用于通知 reactor 重新调度事件
	uint16_t					trace_id; // 跟踪 ID,用于调试
} __attribute__((aligned(SPDK_CACHE_LINE_SIZE)));

int spdk_reactors_init(size_t msg_mempool_size);
void spdk_reactors_fini(void);

void spdk_reactors_start(void);
void spdk_reactors_stop(void *arg1);

struct spdk_reactor *spdk_reactor_get(uint32_t lcore);

extern bool g_scheduling_in_progress;

/**
 * Allocate and pass an event to each reactor, serially.
 *
 * The allocated event is processed asynchronously - i.e. spdk_for_each_reactor
 * will return prior to `fn` being called on each reactor.
 *
 * \param fn This is the function that will be called on each reactor.
 * \param arg1 Argument will be passed to fn when called.
 * \param arg2 Argument will be passed to fn when called.
 * \param cpl This will be called on the originating reactor after `fn` has been
 * called on each reactor.
 */
void spdk_for_each_reactor(spdk_event_fn fn, void *arg1, void *arg2, spdk_event_fn cpl);

/**
 * Set reactor into interrupt mode or back to poll mode.
 *
 * Currently, this function is only permitted within spdk application thread.
 * Also it requires the corresponding reactor does not have any spdk_thread.
 *
 * \param lcore CPU core index of specified reactor.
 * \param new_in_interrupt Set interrupt mode for true, or poll mode for false.
 * \param cb_fn This will be called on spdk application thread after setting interrupt mode.
 * \param cb_arg Argument will be passed to cb_fn when called.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_reactor_set_interrupt_mode(uint32_t lcore, bool new_in_interrupt,
				    spdk_reactor_set_interrupt_mode_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_INTERNAL_EVENT_H */
