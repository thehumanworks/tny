/* Real descriptor/pipe/lock boundaries; private implementation is source-bound
 * so fault and mutation runs exercise the actual lifecycle, not a model. */
#include "cpp/owners.hpp"
#include <unistd.h>
static int fail_pipe_at = 0, pipe_calls = 0;
static int checked_pipe(int ends[2]) {
    if (fail_pipe_at && ++pipe_calls == fail_pipe_at) {
        errno = EMFILE;
        return -1;
    }
    return pipe(ends);
}
#define pipe checked_pipe
#include "cpp/resources.hpp"
#undef pipe
extern "C" {
#include "core/runner.h"
#include "util/jobs_host.h"
#include "util/tny_poll.h"
#include "net/net.h"
}
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>

extern "C" void tny_resource_fault_set(int fault, int dup_at);

static bool probe_writer = false;
static bool fail_fork = false, fail_listener = false, fail_save = false;
static pid_t checked_fork() {
    if (fail_fork) {
        errno = EAGAIN;
        return -1;
    }
    return fork();
}
static int checked_listener(const char *path) {
    if (fail_listener) {
        errno = EMFILE;
        return -1;
    }
    return unix_listen(path);
}
static char writer_path[1024];
static int save_checked(tny_session_state *session) {
    if (probe_writer) {
        assert(session->lock_fd >= 0);
        assert(tny_jobs_host_owner_state(writer_path) == TNY_JOBS_OWNER_HELD);
    }
    if (fail_save) return -1;
    return session_save(session);
}
static int unlink_checked(const char *path) {
    if (probe_writer && std::strstr(path, "sock"))
        assert(tny_jobs_host_owner_state(writer_path) == TNY_JOBS_OWNER_HELD);
    return unlink(path);
}
#define fork         checked_fork
#define unix_listen  checked_listener
#define session_save save_checked
#define unlink       unlink_checked
#ifndef TNY_RUNNER_SOURCE
#define TNY_RUNNER_SOURCE "../../src/core/runner.cpp"
#endif
#include TNY_RUNNER_SOURCE
#undef unlink
#undef session_save
#undef unix_listen
#undef fork
#ifndef TNY_JOBS_SOURCE
#define TNY_JOBS_SOURCE "../../src/core/jobs.cpp"
#endif
#include TNY_JOBS_SOURCE

static int descriptor_count() {
    DIR *d = opendir("/dev/fd");
    if (!d) d = opendir("/proc/self/fd");
    assert(d);
    int count = 0;
    while (auto *entry = readdir(d))
        if (entry->d_name[0] != '.') ++count;
    closedir(d);
    return count;
}

static void descriptor_transfers() {
    int before = descriptor_count();
    for (int i = 0; i < 200; ++i) {
        tny::pipe_pair pipe;
        assert(pipe.open() == 0);
        int raw = pipe.ends[1].borrow();
        {
            tny::descriptor moved(std::move(pipe.ends[1]));
            assert(pipe.ends[1].borrow() == -1);
            pipe.ends[1].adopt(moved.release());
        }
        assert(fcntl(raw, F_GETFD) >= 0);
        assert(write(pipe.ends[1].borrow(), "x", 1) == 1);
        char c = 0;
        assert(read(pipe.ends[0].borrow(), &c, 1) == 1 && c == 'x');
        pipe.ends[1].reset();
        int replacement = open("/dev/null", O_RDWR);
        assert(replacement >= 0);
        assert(dup2(replacement, raw) == raw); /* force the closed number's reuse */
        pipe.ends[1].reset();
        assert(fcntl(raw, F_GETFD) >= 0);
        if (replacement != raw) close(replacement);
        close(raw);
    }
    assert(descriptor_count() == before);
    std::printf("descriptor transfer/reuse cycles=200 before=%d after=%d\n", before,
                descriptor_count());
}

static void job_acquisition_failures(const char *directory) {
    int before = descriptor_count();
    char error[256];
    for (int i = 0; i < 100; ++i) {
        /* Real acquired state lock, then missing record; destructor fallback
         * releases it even without the ordinary explicit end call. */
        jobs_txn transaction;
        assert(jobs_txn_begin(directory, "0123456789abcdef0123456789abcdef", &transaction, error,
                              sizeof error) != 0);
        tny::pipe_pair payload, ack;
        assert(payload.open() == 0 && ack.open() == 0);
        /* Invalid admission consumes every inherited owned fd on early return. */
        assert(tny_jobs_worker_main(nullptr, "invalid", payload.ends[0].release(),
                                    ack.ends[1].release(), -1) == 1);
        jobs_launch launch{};
        assert(jobs_launch_worker("/nonexistent/tny-phase3", directory,
                                  "0123456789abcdef0123456789abcdef", -1, "{}", &launch, error,
                                  sizeof error, nullptr, nullptr) != 0);
        assert(!launch.live);
    }
    assert(descriptor_count() == before);
    std::printf("job failed acquisition cycles=100 before=%d after=%d\n", before,
                descriptor_count());
}

static void acquisition_faults(const char *directory) {
    int before = descriptor_count();
    for (int at = 1; at <= 2; ++at) {
        fail_pipe_at = at;
        pipe_calls = 0;
        jobs_launch launch{};
        char error[256];
        assert(jobs_launch_worker("/nonexistent/tny-phase3", directory,
                                  "0123456789abcdef0123456789abcdef", -1, "{}", &launch, error,
                                  sizeof error, nullptr, nullptr) != 0);
        assert(pipe_calls == at && !launch.live);
        assert(descriptor_count() == before);
    }
    fail_pipe_at = 0;
    for (int fault = 0; fault < 3; ++fault) {
        auto *ctx = tny_ctx_new_explicit(directory, directory);
        auto *session = session_new(ctx);
        assert(session);
        fail_save = fault == 0;
        fail_listener = fault == 1;
        fail_fork = fault == 2;
        tny_runner_opts options{};
        char error[256];
        assert(tny_runner_spawn(ctx, session, &options, error, sizeof error) < 0);
        assert(session->lock_fd < 0);
        assert(!session_is_running(ctx, session->id));
        char *socket = tny_runner_sock_path(session->dir);
        assert(socket && access(socket, F_OK) != 0);
        std::free(socket);
        session_close(session);
        tny_ctx_free(ctx);
        assert(descriptor_count() == before);
    }
    fail_save = fail_listener = fail_fork = false;
    puts("acquisition faults: pipe1/pipe2/save/listener/fork closed all resources");
}

static void client_allocation_fault(const char *directory) {
    char path[1024];
    std::snprintf(path, sizeof path, "%s/client.sock", directory);
    int before = descriptor_count();
    for (int failure = 0; failure <= 1; ++failure) {
        int listener = unix_listen(path);
        assert(listener >= 0);
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            pollfd fd{listener, POLLIN, 0};
            if (tny_poll(&fd, 1, 3000) <= 0) _exit(2);
            int peer = accept(listener, nullptr, nullptr);
            if (peer < 0) _exit(3);
            char bytes[256];
            pollfd input{peer, POLLIN, 0};
            if (tny_poll(&input, 1, 3000) > 0) (void)read(peer, bytes, sizeof bytes);
            close(peer);
            _exit(0);
        }
        setenv("TNY_TEST_ALLOC_SCOPE", "runner-owner", 1);
        setenv("TNY_TEST_ALLOC_FAIL_AT", failure ? "1" : "0", 1);
        tny_alloc_scope_begin("runner-owner");
        auto *client = tny_runner_client_connect(path, 2000, TNY_RUNNER_OWNER, false);
        bool result = failure ? client == nullptr : client != nullptr;
        size_t allocations = tny_alloc_test_scope_count();
        tny_runner_client_close(client);
        assert(tny_alloc_test_scope_count() == allocations);
        unsetenv("TNY_TEST_ALLOC_SCOPE");
        unsetenv("TNY_TEST_ALLOC_FAIL_AT");
        tny_alloc_scope_begin("fixture");
        int status;
        assert(waitpid(child, &status, 0) == child);
        close(listener);
        unlink(path);
        assert(result && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert(descriptor_count() == before);
        assert(tny_parser_test_live_allocations() == 0);
    }
    puts("client allocation failure and allocation-free teardown passed");
}

static void item_lifecycle_loops(const char *directory) {
    char *self = tny_process_self_path();
    assert(self);
    auto *request = yyjson_mut_doc_new(jallocator());
    auto *root = yyjson_mut_obj(request);
    yyjson_mut_doc_set_root(request, root);
    jm_set_str(request, root, "self", self);
    jm_set_str(request, root, "cwd", directory);
    jm_set_str(request, root, "perm_mode", "ask");
    jm_set_str(request, root, "tools", "none");
    char *json = jwrite(request);
    auto *payload = jparse(json, std::strlen(json));
    std::free(json);
    std::free(self);
    yyjson_mut_doc_free(request);
    int before = descriptor_count();
    assert(tny_process_supervisor_init() == 0);
    for (int cycle = 0; cycle < 40; ++cycle) {
        job_slot slot{};
        char path[1024], error[256];
        std::snprintf(path, sizeof path, "%s/item-%d.log", directory, cycle);
        slot.log_path = xstrdup(path);
        slot.prompt = xstrdup("fixture prompt");
        assert(worker_spawn_item(yyjson_doc_get_root(payload), nullptr, false, &slot, error,
                                 sizeof error) == 0);
        int64_t deadline = monotonic_ms() + 5000;
        while ((!slot.reaped || !slot.eof || (slot.scope.borrow() && !slot.cleanup_done)) &&
               monotonic_ms() < deadline) {
            if (slot.scope.borrow()) {
                worker_scope_progress(&slot);
                if (slot.admission_ready && !slot.released)
                    slot.released =
                        tny_process_scope_go(slot.scope.borrow(), slot.in_fd.borrow()) == 1;
            }
            worker_pump(&slot, 1);
            if (!slot.scope.borrow() && !slot.reaped)
                slot.reaped = tny_jobs_host_reap(slot.pid, &slot.status) == 1;
        }
        bool complete = slot.reaped && slot.eof && !slot.cleanup_unknown;
        if (!complete && !slot.scope.borrow()) {
            tny_process_stop_owned_tree(slot.pid, &slot.status, &slot.reaped);
        }
        assert(slot_free(&slot));
        assert(complete && WIFEXITED(slot.status) && WEXITSTATUS(slot.status) == 0);
        size_t size;
        char *log = file_slurp(path, &size);
        assert(log && std::strcmp(log, "fixture child drained\n") == 0);
        std::free(log);
        unlink(path);
        assert(descriptor_count() == before);
    }
    yyjson_doc_free(payload);
    std::printf("item spawn/pump/reap/drain cycles=40 before=%d after=%d\n", before,
                descriptor_count());
}

static void host_boundary_faults(const char *directory) {
    char path[1024];
    std::snprintf(path, sizeof path, "%s/atomic-state", directory);
    assert(tny_jobs_host_write_private(path, "before", 6) == 0);
    int before = descriptor_count();
    for (int fault = 1; fault <= 4; ++fault) {
        tny_resource_fault_set(fault, 0);
        assert(tny_jobs_host_write_private(path, "after", 5) != 0);
        tny_resource_fault_set(0, 0);
        size_t size;
        char *bytes = file_slurp(path, &size);
        assert(bytes && size == 6 && std::memcmp(bytes, "before", 6) == 0);
        std::free(bytes);
        assert(descriptor_count() == before);
    }
    char *self = tny_process_self_path();
    char *argv[] = {self, nullptr};
    char *env[] = {nullptr};
    {
        tny::pipe_pair pipe;
        assert(pipe.open() == 0);
        tny_fd_mapping maps[] = {{pipe.ends[0].borrow(), 0}, {pipe.ends[1].borrow(), 1}};
        int held = descriptor_count();
        for (int at = 1; at <= 3; ++at) {
            tny_resource_fault_set(at == 3 ? 6 : 5, at);
            pid_t pid = -1;
            assert(tny_process_spawn_mapped(argv, env, maps, 2, &pid) != 0);
            tny_resource_fault_set(0, 0);
            assert(pid == -1 && descriptor_count() == held);
            assert(fcntl(pipe.ends[0].borrow(), F_GETFD) >= 0);
            assert(fcntl(pipe.ends[1].borrow(), F_GETFD) >= 0);
        }
    }
    std::free(self);
    unlink(path);
    assert(descriptor_count() == before);
    puts("host faults: open/write/fsync/rename/dup1/dup2/spawn preserve bytes and borrowed fds");
}

static void unknown_cleanup() {
    auto *doc = yyjson_mut_doc_new(jallocator());
    auto *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    jm_set_str(doc, root, "state", "failed");
    jm_set_str(doc, root, "cleanup", "unknown");
    jm_set_str(doc, root, "error_code", TNY_JOBS_CODE_IO);
    jm_set_bool(doc, root, "cleanup_hold", true);
    assert(!cleanup_reclaimable(root));
    jm_set_bool(doc, root, "cleanup_hold", false);
    assert(!cleanup_reclaimable(root));
    jm_set_str(doc, root, "cleanup", "complete");
    assert(cleanup_reclaimable(root));
    yyjson_mut_doc_free(doc);
}

static void runner_shutdown(const char *directory) {
    auto *ctx = tny_ctx_new_explicit(directory, directory);
    assert(ctx);
    auto *session = session_new(ctx);
    assert(session && session_save(session) == 0);
    std::snprintf(writer_path, sizeof writer_path, "%s/lock", session->dir);
    probe_writer = true;
    tny_runner_opts opts{.serve = true};
    char error[256];
    pid_t child = tny_runner_spawn(ctx, session, &opts, error, sizeof error);
    assert(child > 0);
    char *socket = tny_runner_sock_path(session->dir);
    auto *client = tny_runner_client_connect(socket, 2000, TNY_RUNNER_OWNER, false);
    int send_result = client ? tny_runner_client_end(client, "done") : -1;
    if (send_result != 0) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        tny_runner_client_close(client);
        assert(send_result == 0);
    }
    bool bye = false;
    int64_t deadline = monotonic_ms() + 5000;
    while (!bye && monotonic_ms() < deadline) {
        pollfd fd{tny_runner_client_fd(client), POLLIN, 0};
        tny_poll(&fd, 1, 20);
        tny_runner_client_pump(client);
        while (auto *message = tny_runner_client_pop(client)) {
            if (message->kind == TNY_RMSG_BYE) bye = true;
            tny_runner_msg_free(message);
        }
    }
    int status = 0;
    if (!bye) kill(child, SIGKILL);
    assert(waitpid(child, &status, 0) == child);
    assert(bye && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(tny_jobs_host_owner_state(writer_path) == TNY_JOBS_OWNER_FREE);
    assert(access(socket, F_OK) != 0);
    tny_runner_client_close(client);
    std::free(socket);
    probe_writer = false;
    session_close(session);
    tny_ctx_free(ctx);
}

static void metadata_pid_is_not_authority(const char *directory) {
    auto *ctx = tny_ctx_new_explicit(directory, directory);
    assert(ctx);
    const char *id = "123456789abcdef0123456789abcdef0";
    char *dir = jobs_dir(ctx, id);
    assert(dir && mkdir_p(dir) == 0);
    char *lock = jobs_file(dir, "owner.lock");
    tny::lock_descriptor owner;
    owner.adopt(tny_jobs_host_lock_open(lock));
    assert(owner.borrow() >= 0);
    assert(tny_jobs_host_lock_try(owner.borrow()) == TNY_JOBS_LOCK_ACQUIRED);
    tny::pipe_pair sentinel_gate;
    assert(sentinel_gate.open() == 0);
    pid_t sentinel = fork();
    assert(sentinel >= 0);
    if (!sentinel) {
        signal(SIGTERM, SIG_DFL);
        close(sentinel_gate.ends[1].borrow());
        char byte;
        _exit(read(sentinel_gate.ends[0].borrow(), &byte, 1) == 1 ? 0 : 2);
    }
    sentinel_gate.ends[0].reset();
    auto *record = yyjson_mut_doc_new(jallocator());
    auto *root = yyjson_mut_obj(record);
    yyjson_mut_doc_set_root(record, root);
    jm_set_int(record, root, "version", 1);
    jm_set_str(record, root, "kind", "job");
    jm_set_str(record, root, "id", id);
    jm_set_str(record, root, "state", "running");
    jm_set_str(record, root, "cleanup", "pending");
    jm_set_int(record, root, "pid", sentinel);
    yyjson_mut_obj_add_val(record, root, "items", yyjson_mut_arr(record));
    assert(jobs_record_store(dir, record) == 0);
    yyjson_mut_doc_free(record);
    char request[128];
    std::snprintf(request, sizeof request, "{\"id\":\"%s\"}", id);
    auto *args = jparse(request, std::strlen(request));
    buf_t output{};
    char error[256];
    assert(jobs_cancel(ctx, yyjson_doc_get_root(args), &output, error, sizeof error) == 0);
    assert(write(sentinel_gate.ends[1].borrow(), "x", 1) == 1);
    int status = 0;
    assert(waitpid(sentinel, &status, 0) == sentinel);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    buf_free(&output);
    yyjson_doc_free(args);
    std::free(lock);
    std::free(dir);
    tny_ctx_free(ctx);
}

static void consumed_checkpoint_stays_consumed(const char *directory) {
    auto *ctx = tny_ctx_new_explicit(directory, directory);
    assert(ctx);
    auto *session = session_new(ctx);
    assert(session && session_lock_acquire(session) == 0);
    auto *doc = session->doc;
    auto *continuation = yyjson_mut_obj(doc);
    auto *resume = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, resume, "resumable", true);
    yyjson_mut_obj_add_val(doc, continuation, "_resume", resume);
    yyjson_mut_obj_add_val(doc, yyjson_mut_doc_get_root(doc), "continuation", continuation);
    assert(session_save(session) == 0);
    auto *packet = rn_disk_packet(session);
    assert(packet);
    yyjson_doc_free(packet);
    rn_state runner{};
    runner.session = session;
    assert(rn_consume_checkpoint(&runner));
    assert(session_reload_locked(session, nullptr, 0) == 0);
    assert(rn_disk_packet(session) == nullptr); /* committed batch cannot re-enter */
    session_close(session);
    tny_ctx_free(ctx);
}

int main(int argc, char **argv) {
    if (argc > 2 && std::strcmp(argv[1], "--cwd") == 0) {
        if (tny_process_scope_admit() != 0) return 2;
        char bytes[64];
        while (read(STDIN_FILENO, bytes, sizeof bytes) > 0) {}
        puts("fixture child drained");
        return 0;
    }
    assert(argc == 2);
    descriptor_transfers();
    job_acquisition_failures(argv[1]);
    acquisition_faults(argv[1]);
    client_allocation_fault(argv[1]);
    item_lifecycle_loops(argv[1]);
    host_boundary_faults(argv[1]);
    unknown_cleanup();
    runner_shutdown(argv[1]);
    metadata_pid_is_not_authority(argv[1]);
    consumed_checkpoint_stays_consumed(argv[1]);
    puts("phase3 ownership oracles passed");
}
