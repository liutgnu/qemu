#include "qemu/osdep.h"
#include <glib.h>
#include "libqtest.h"
#include "qemu/option.h"
#include "qemu/range.h"
#include "sysemu/char.h"
#include "sysemu/sysemu.h"
#include <sys/mman.h>
#include <sys/vfs.h>
#include <qemu/sockets.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__linux__) && defined(__NR_userfaultfd) && defined(CONFIG_EVENTFD)
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>

bool got_stop;

static bool ufd_version_check(void) {
	struct uffdio_api api_struct;
	uint64_t ioctl_mask;

	int ufd = ufd = syscall(__NR_userfaultfd, O_CLOEXEC);

	if (ufd == -1) {
		g_test_message("Skipping test: userfaultfd not available");
		return false;
	}

	api_struct.api = UFFD_API;
	api_struct.features = 0;
	if (ioctl(ufd, UFFDIO_API, &api_struct)) {
		g_test_message("Skipping test: UFFDIO_API failed");
		return false;
	}

	ioctl_mask = (__u64)1 << _UFFDIO_REGISTER |
	             (__u64)1 << _UFFDIO_UNREGISTER;
	if ((api_struct.ioctls & ioctl_mask) != ioctl_mask) {
		g_test_message("Skipping test: Missing userfault feature");
		return false;
	}
	return true;
}
# else
static bool ufd_version_check(void) {
	g_test_message("Skipping test: Userfault not available (builtdtime)");
	return false;
}
#endif

static const char *tmpfs;

/*
* Wait for some output in the serial output file,
* we get an 'A' followed by an endless string of 'B's
* but on the destination we won't have the A.
*/

static void wait_for_serial(const char *side) {
	char *serialpath = g_strdup_printf("%s/%s", tmpfs, side);
	FILE *serialfile = fopen(serialpath, "r");

	do {
		int readvalue = fgetc(serialfile);
		switch (readvalue) {
		case 'A':
			return;

		case EOF:
			fseek(serialfile, 0, SEEK_SET);
			usleep(1000);
			break;

		default:
			fprintf(stderr, "Unexpected %d on %s serial\n", readvalue, side);
			assert(0);
		}
	}
	while (true);
}

/*
* Events can get in the way of responses we are actually waiting for.
*/

static QDict *return_or_event(QDict *response) {
	const char *event_string;
	if (!qdict_haskey(response, "event")) {
		return response;
	}

	/* OK, it was an event */
	event_string = qdict_get_str(response, "event");
	if (!strcmp(event_string, "STOP")) {
		got_stop = true;
	}
	QDECREF(response);
	return return_or_event(qtest_qmp_receive(global_qtest));
}

static void wait_for_migration_complete(void) {
	QDict *rsp, *rsp_return;
	bool completed;

	do {
		const char *status;

		rsp = return_or_event(qmp("{ 'execute': 'query-migrate' }"));
		rsp_return = qdict_get_qdict(rsp, "return");
		status = qdict_get_str(rsp_return, "status");
		completed = strcmp(status, "completed") == 0;
		assert(strcmp(status, "failed"));
		QDECREF(rsp);
		usleep(1000 * 0.1);
	}
	while (!completed);
}

static void cleanup(const char *filename) {
	char *path = g_strdup_printf("%s/%s", tmpfs, filename);

	unlink(path);
}

/*
 * We need to input ports first. 
 * portsrc represent port which data come from.
 * portdest represent port which data go to.
 *
 *          Postcopy migration gragh
 *          
 *              ----------------
 * source ----> |   malware    | ----> destination
 *              ----------------
 *           portsrc        portdest
 */            
static void test_migrate(void)
{
	int portsrc,portdest;
	printf("Source host migrate to which port?");
	scanf("%d",&portsrc);
	printf("Destination host migrate from which port?");
	scanf("%d",&portdest);

	char *income_uri = g_strdup_printf("tcp:127.0.0.1:%d",portdest);
	char *outcome_uri = g_strdup_printf("tcp:127.0.0.1:%d",portsrc);

	QTestState *from, *to;
	gchar *cmd;
	QDict *rsp;

	char *bootpath = g_strdup_printf("/tmp/geniux.img");

	got_stop = false;

	cmd = g_strdup_printf("-machine accel=kvm:tcg -m 10M"
	                       " -name pcsource,debug-threads=on"
						   " -display gtk"
	                       " -serial file:%s/src_serial"
	                       " -fda %s",
	                       tmpfs, bootpath);
	from = qtest_start(cmd);
	g_free(cmd);

	cmd = g_strdup_printf("-machine accel=kvm:tcg -m 10M"
	                       " -name pcdest,debug-threads=on"
						   " -display gtk"
	                       " -serial file:%s/dest_serial"
	                       " -fda %s"
	                       " -incoming %s",
	                       tmpfs, bootpath, income_uri);
	to = qtest_init(cmd);
	g_free(cmd);

	global_qtest = from;
	rsp = qmp("{ 'execute': 'migrate-set-capabilities',"
	               "'arguments': { "
	                   "'capabilities': [ {"
	                       "'capability': 'postcopy-ram',"
	                       "'state': true } ] } }");
	g_assert(qdict_haskey(rsp, "return"));
	QDECREF(rsp);

	global_qtest = to;
	rsp = qmp("{ 'execute': 'migrate-set-capabilities',"
	               "'arguments': { "
	                   "'capabilities': [ {"
	                       "'capability': 'postcopy-ram',"
	                       "'state': true } ] } }");
	g_assert(qdict_haskey(rsp, "return"));
	QDECREF(rsp);

	/* We want to pick a speed slow enough that the test completes
 	 * quickly, but that it doesn't complete precopy even on a slow
 	 * machine, so also set the downtime.
 	 */
	global_qtest = from;
	rsp = qmp("{ 'execute': 'migrate_set_speed',"
	           "'arguments': { 'value': 10000000 } }");
	g_assert(qdict_haskey(rsp, "return"));
	QDECREF(rsp);

	/* 1ms downtime - it should never finish precopy */
	rsp = qmp("{ 'execute': 'migrate_set_downtime',"
	           "'arguments': { 'value': 0.0000001 } }");
	g_assert(qdict_haskey(rsp, "return"));
	QDECREF(rsp);

	/* Wait for the first serial output from the source */
	wait_for_serial("src_serial");

	cmd = g_strdup_printf("{ 'execute': 'migrate',"
	                       "'arguments': { 'uri': '%s' } }",
							outcome_uri);
	rsp = qmp(cmd);
	g_free(cmd);
	g_assert(qdict_haskey(rsp, "return"));
	QDECREF(rsp);

	rsp = return_or_event(qmp("{ 'execute': 'migrate-start-postcopy' }"));
	g_assert(qdict_haskey(rsp, "return"));
	QDECREF(rsp);

	if (!got_stop) {
		qmp_eventwait("STOP");
	}

	global_qtest = to;
	qmp_eventwait("RESUME");

	global_qtest = from;
	wait_for_migration_complete();

	g_free(income_uri);
	g_free(outcome_uri);

	cleanup("src_serial");
	cleanup("dest_serial");
}

int main(int argc, char **argv)
{
	char template[] = "/tmp/postcopy-mmsafty-test-XXXXXX";
	int ret;

	g_test_init(&argc, &argv, NULL);
	if (!ufd_version_check()) {
		return 0;
	}
	tmpfs = mkdtemp(template);
	if (!tmpfs) {
		g_test_message("mkdtemp on path (%s): %s\n", template, strerror(errno));
	}
	g_assert(tmpfs);

	module_call_init(MODULE_INIT_QOM);
	qtest_add_func("/postcopy-mmsafety", test_migrate);
	ret = g_test_run();
	g_assert_cmpint(ret, ==, 0);
	ret = rmdir(tmpfs);
	if (ret != 0) {
		g_test_message("unable to rmdir: path (%s): %s\n",
		                   tmpfs, strerror(errno));
	}
	return ret;
}
