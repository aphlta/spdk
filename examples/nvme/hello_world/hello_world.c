/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk_internal/nvme_util.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

#define DATA_BUFFER_STRING "Hello world!"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr; // nvme控制器指针
	TAILQ_ENTRY(ctrlr_entry)	link; // 链表指针
	char				name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr; // nvme控制器指针
	struct spdk_nvme_ns	*ns; // nvme命名空间指针
	TAILQ_ENTRY(ns_entry)	link; // 链表指针
	struct spdk_nvme_qpair	*qpair; // nvme io队列对指针
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers); // 存储所有发现的nvme控制器
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces); // 存储所有活跃的命令空间
static struct spdk_nvme_transport_id g_trid = {}; // 传输层标识符,用于指定连接方式

static bool g_vmd = false; // 是否使用vmd

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

struct hello_world_sequence {
	struct ns_entry	*ns_entry;
	char		*buf;
	unsigned        using_cmb_io;
	int		is_completed;
};

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence *sequence = arg;

	/* Assume the I/O was successful */
	sequence->is_completed = 1;
	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}

	if (strcmp(sequence->buf, DATA_BUFFER_STRING)) {
		fprintf(stderr, "Read data doesn't match write data\n");
		exit(1);
	}

	/*
	 * The read I/O has completed.  Print the contents of the
	 *  buffer, free the buffer, then mark the sequence as
	 *  completed.  This will trigger the hello_world() function
	 *  to exit its polling loop.
	 */
	printf("%s\n", sequence->buf);
	spdk_free(sequence->buf);
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence	*sequence = arg;
	struct ns_entry			*ns_entry = sequence->ns_entry;
	int				rc;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 *
	 * 检查是否发生错误。 如果发生错误, 则显示有关错误的信息, 并将完成值设置为1, 以便I/O调用者知道发生了错误。
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
	/*
	 * The write I/O has completed.  Free the buffer associated with
	 *  the write I/O and allocate a new zeroed buffer for reading
	 *  the data back from the NVMe namespace.
	 *
	 * 写入I/O已完成。 释放与写入I/O关联的缓冲区, 并为从NVMe命名空间读取数据分配一个新的零缓冲区。
	 */
	if (sequence->using_cmb_io) {
		spdk_nvme_ctrlr_unmap_cmb(ns_entry->ctrlr);
	} else {
		spdk_free(sequence->buf);
	}
	sequence->buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);

	/*
	 * Submit a read I/O to read the data back from the NVMe namespace.
	 *
	 * 提交一个读取I/O, 从NVMe命名空间读取数据。
	 */
	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, sequence->buf,
				   0, /* LBA start */
				   1, /* number of LBAs */
				   read_complete, (void *)sequence, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}
}

static void
reset_zone_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence *sequence = arg;

	/* Assume the I/O was successful */
	sequence->is_completed = 1;
	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Reset zone I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
}

static void
reset_zone_and_wait_for_completion(struct hello_world_sequence *sequence)
{
	if (spdk_nvme_zns_reset_zone(sequence->ns_entry->ns, sequence->ns_entry->qpair,
				     0, /* starting LBA of the zone to reset */
				     false, /* don't reset all zones */
				     reset_zone_complete,
				     sequence)) {
		fprintf(stderr, "starting reset zone I/O failed\n");
		exit(1);
	}
	while (!sequence->is_completed) {
		spdk_nvme_qpair_process_completions(sequence->ns_entry->qpair, 0);
	}
	sequence->is_completed = 0;
}

static void
hello_world(void)
{
	struct ns_entry			*ns_entry;
	struct hello_world_sequence	sequence;
	int				rc;
	size_t				sz;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		/*
		 * Allocate an I/O qpair that we can use to submit read/write requests
		 *  to namespaces on the controller.  NVMe controllers typically support
		 *  many qpairs per controller.  Any I/O qpair allocated for a controller
		 *  can submit I/O to any namespace on that controller.
		 *
		 * 分配一个命名空间的I/O队列对用于提交读写请求, NVMe控制器支持多个I/O队列对, 每个队列对可以用于提交读写请求。
		 *
		 * The SPDK NVMe driver provides no synchronization for qpair accesses -
		 *  the application must ensure only a single thread submits I/O to a
		 *  qpair, and that same thread must also check for completions on that
		 *  qpair.  This enables extremely efficient I/O processing by making all
		 *  I/O operations completely lockless.
		 *
		 * SPDK的nvme驱动提供了无锁的I/O操作, 这使得应用必须确保只有一个线程提交I/O到一个队列对, 并检查该队列对的完成情况。
		 * 这提供了极有效率的io处理, 因为所有的I/O操作都是无锁的.
		 */
		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			return;
		}

		/*
		 * Use spdk_dma_zmalloc to allocate a 4KB zeroed buffer.  This memory
		 * will be pinned, which is required for data buffers used for SPDK NVMe
		 * I/O operations.
		 *
		 * 使用spdk_dma_zmalloc分配一个4KB的零初始化缓冲区。 这个内存会被固定, 这是SPDK NVMe I/O操作中使用的数据缓冲区所必需的。
		 */
		sequence.using_cmb_io = 1;
		sequence.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
		if (sequence.buf == NULL || sz < 0x1000) {
			sequence.using_cmb_io = 0;
			sequence.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		}
		if (sequence.buf == NULL) {
			printf("ERROR: write buffer allocation failed\n");
			return;
		}
		if (sequence.using_cmb_io) {
			printf("INFO: using controller memory buffer for IO\n");
		} else {
			printf("INFO: using host memory buffer for IO\n");
		}
		sequence.is_completed = 0;
		sequence.ns_entry = ns_entry;

		/*
		 * If the namespace is a Zoned Namespace, rather than a regular
		 * NVM namespace, we need to reset the first zone, before we
		 * write to it. This not needed for regular NVM namespaces.
		 *
		 * 如果命名空间是一个Zoned命名空间, 而不是一个普通的NVM命名空间, 我们需要在写入之前重置第一个区域。 这对普通的NVM命名空间来说是不需要的。
		 */
		if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
			reset_zone_and_wait_for_completion(&sequence);
		}

		/*
		 * Print DATA_BUFFER_STRING to sequence.buf. We will write this data to LBA
		 *  0 on the namespace, and then later read it back into a separate buffer
		 *  to demonstrate the full I/O path.
		 *
		 * 打印DATA_BUFFER_STRING到sequence.buf。 我们将把这个数据写入命名空间的LBA 0, 然后稍后读取到一个单独的缓冲区中, 以演示完整的I/O路径。
		 */
		snprintf(sequence.buf, 0x1000, "%s", DATA_BUFFER_STRING);

		/*
		 * Write the data buffer to LBA 0 of this namespace.  "write_complete" and
		 *  "&sequence" are specified as the completion callback function and
		 *  argument respectively.  write_complete() will be called with the
		 *  value of &sequence as a parameter when the write I/O is completed.
		 *  This allows users to potentially specify different completion
		 *  callback routines for each I/O, as well as pass a unique handle
		 *  as an argument so the application knows which I/O has completed.
		 *
		 * 向命名空间的LBA 0写入数据缓冲区。 "write_complete"和"&sequence"分别指定为完成回调函数和参数。
		 * 当写入I/O完成时, write_complete()将使用sequence作为参数调用, 这允许用户为每个I/O指定不同的完成回调例程, 并将一个唯一的句柄作为参数传递, 以便应用程序知道哪个I/O已完成。
		 *
		 * Note that the SPDK NVMe driver will only check for completions
		 *  when the application calls spdk_nvme_qpair_process_completions().
		 *  It is the responsibility of the application to trigger the polling
		 *  process.
		 *
		 * 注意, SPDK的nvme驱动只在应用程序调用spdk_nvme_qpair_process_completions()时检查完成情况。 这是应用程序的责任来触发轮询过程。
		 */
		rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, sequence.buf,
					    0, /* LBA start */
					    1, /* number of LBAs */
					    write_complete, &sequence, 0);
		if (rc != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			exit(1);
		}

		/*
		 * Poll for completions.  0 here means process all available completions.
		 *  In certain usage models, the caller may specify a positive integer
		 *  instead of 0 to signify the maximum number of completions it should
		 *  process.  This function will never block - if there are no
		 *  completions pending on the specified qpair, it will return immediately.
		 *
		 * 轮询完成。 0表示处理所有可用的完成情况。 在某些使用模型中, 调用者可以指定一个正整数, 而不是0, 来表示它应该处理的最大完成情况数量。
		 * 这个函数永远不会阻塞 - 如果指定的qpair上没有待处理的完成情况, 它会立即返回。
		 *
		 * When the write I/O completes, write_complete() will submit a new I/O
		 *  to read LBA 0 into a separate buffer, specifying read_complete() as its
		 *  completion routine.  When the read I/O completes, read_complete() will
		 *  print the buffer contents and set sequence.is_completed = 1.  That will
		 *  break this loop and then exit the program.
		 *
		 * 当写入I/O完成时,  write_complete 将会提交一个新的I/O来读取LBA 0到一个单独的缓冲区中, 指定read_complete()作为其完成例程。
		 * 当读取I/O完成时, read_complete()将打印缓冲区的内容并设置sequence.is_completed = 1。 这将导致循环退出并退出程序。
		 *
		 */

		while (!sequence.is_completed) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}

		/*
		 * Free the I/O qpair.  This typically is done when an application exits.
		 *  But SPDK does support freeing and then reallocating qpairs during
		 *  operation.  It is the responsibility of the caller to ensure all
		 *  pending I/O are completed before trying to free the qpair.
		 */
		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true; // 返回true表示要连接这个控制器
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	printf("Attached to %s\n", trid->traddr);

	/*
	 * spdk_nvme_ctrlr is the logical abstraction in SPDK for an NVMe
	 *  controller.  During initialization, the IDENTIFY data for the
	 *  controller is read using an NVMe admin command, and that data
	 *  can be retrieved using spdk_nvme_ctrlr_get_data() to get
	 *  detailed information on the controller.  Refer to the NVMe
	 *  specification for more details on IDENTIFY for NVMe controllers.
	 *
	 *  spdk_nvme_ctrlr 是一个NVMe控制器的逻辑抽象，在初始化时，会使用NVMe管理员命令读取控制器的IDENTIFY数据，
	 *  并可以使用 spdk_nvme_ctrlr_get_data() 获取详细的控制器信息。
	 *  参考NVMe规范了解更多关于控制器的IDENTIFY的详细信息。
	 */
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	/*
	 * Each controller has one or more namespaces.  An NVMe namespace is basically
	 *  equivalent to a SCSI LUN.  The controller's IDENTIFY data tells us how
	 *  many namespaces exist on the controller.  For Intel(R) P3X00 controllers,
	 *  it will just be one namespace.
	 *
	 * Note that in NVMe, namespace IDs start at 1, not 0.

	 * 每个控制器都有一个或多个命名空间。NVMe命名空间基本上相当于SCSI LUN。
	 * 控制器的IDENTIFY数据告诉我们控制器上存在多少个命名空间。
	 * 对于Intel(R) P3X00控制器，它只是一个命名空间。
	 *
	 * 注意，在NVMe中，命名空间ID从1开始，而不是0。
	 */
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns); // 注册命名空间
	}
}

static void
cleanup(void)
{
	struct ns_entry *ns_entry, *tmp_ns_entry;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\t\n");
	printf("options:\n");
	printf("\t[-d DPDK huge memory size in MB]\n");
	printf("\t[-g use single file descriptor for DPDK memory segments]\n");
	printf("\t[-i shared memory group ID]\n");
	spdk_nvme_transport_id_usage(stdout, 0);
	printf("\t[-V enumerate VMD]\n");
#ifdef DEBUG
	printf("\t[-L enable debug logging]\n");
#else
	printf("\t[-L enable debug logging (flag disabled, must reconfigure with --enable-debug)]\n");
#endif
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, rc;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "d:ghi:r:L:V")) != -1) {
		switch (op) {
		case 'V':
			g_vmd = true;
			break;
		case 'i':
			env_opts->shm_id = spdk_strtol(optarg, 10);
			if (env_opts->shm_id < 0) {
				fprintf(stderr, "Invalid shared memory ID\n");
				return env_opts->shm_id;
			}
			break;
		case 'g':
			env_opts->hugepage_single_segments = true;
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 'd':
			env_opts->mem_size = spdk_strtol(optarg, 10);
			if (env_opts->mem_size < 0) {
				fprintf(stderr, "Invalid DPDK memory size\n");
				return env_opts->mem_size;
			}
			break;
		case 'L':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;

	/*
	 * SPDK relies on an abstraction around the local environment
	 * named env that handles memory allocation and PCI device operations.
	 * This library must be initialized first.
	 *
	 *  SPDK 依赖于一个抽象的本地环境，
	 *  名为 env，用于处理内存分配和 PCI 设备操作。
	 *  这个库必须先初始化。
	 */
	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	opts.name = "hello_world";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controllers\n");

	if (g_vmd && spdk_vmd_init()) {
		fprintf(stderr, "Failed to initialize VMD."
			" Some NVMe devices can be unavailable.\n");
	}

	/*
	 * Start the SPDK NVMe enumeration process.  probe_cb will be called
	 *  for each NVMe controller found, giving our application a choice on
	 *  whether to attach to each controller.  attach_cb will then be
	 *  called for each controller after the SPDK NVMe driver has completed
	 *  initializing the controller we chose to attach.
	 *
	 * 启动 SPDK NVMe 枚举过程。
	 *  对每个找到的 NVMe 控制器调用 probe_cb，
	 *  给我们的应用程序选择是否附加到每个控制器。
	 *  然后，在 SPDK NVMe 驱动程序完成初始化我们选择附加的控制器后，
	 *  会为每个控制器调用 attach_cb。
	 */
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}

	/*
	 * 等待所有控制器初始化完成。
	 *  等待所有控制器初始化完成。
	 *  等待所有控制器初始化完成。
	 */
	if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "no NVMe controllers found\n");
		rc = 1;
		goto exit;
	}

	printf("Initialization complete.\n");
	hello_world();

exit:
	fflush(stdout);
	cleanup();
	if (g_vmd) {
		spdk_vmd_fini();
	}

	spdk_env_fini();
	return rc;
}
