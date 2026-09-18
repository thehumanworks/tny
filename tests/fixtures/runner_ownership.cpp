/* Real descriptor/pipe/lock boundaries; private implementation is source-bound
 * so fault and mutation runs exercise the actual lifecycle, not a model. */
#include "util/ownership.hpp"
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
#include "util/resources.hpp"
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
static yyjson_mut_val *checkpoint_bool;
static int checkpoint_save_calls;
static int save_checked(tny_session_state *session) {
    ++checkpoint_save_calls;
    if (checkpoint_bool) {
        auto *continuation =
            yyjson_mut_obj_get(yyjson_mut_doc_get_root(session->doc), "continuation");
        auto *resume = yyjson_mut_obj_get(continuation, "_resume");
        assert(yyjson_mut_obj_get(resume, "resumable") == checkpoint_bool);
        assert(yyjson_mut_is_bool(checkpoint_bool) && !yyjson_mut_get_bool(checkpoint_bool));
        assert(tny_alloc_test_scope_count() == 0 && !tny_alloc_test_scope_injected());
    }
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

/* Job persistence faults run the real supervisor. Mode 1 loses the first
 * child's wait result while a second real child waits for a fixture file;
 * mode 2 rejects the final write, mode 3 rejects the write-ahead hold. */
static int job_fault_mode, job_fault_hits, job_notify_fd = -1;
static pid_t job_children[2] = {-1, -1};
static char job_release_path[1024];
static const char *transaction_lock_probe = nullptr;
static int checked_job_reap(pid_t pid, int *status) {
    if (!job_fault_mode) return tny_jobs_host_reap(pid, status);
    if (job_children[0] < 0) job_children[0] = pid;
    if (pid != job_children[0]) job_children[1] = pid;
    if (job_fault_mode == 1 && pid == job_children[1]) return 0;
    int result = tny_jobs_host_reap(pid, status);
    if (job_fault_mode == 1 && result == 1) {
        ++job_fault_hits;
        return -1; /* real child reaped, but its exit proof is unavailable */
    }
    return result;
}
static int checked_job_write(const char *path, const void *data, size_t size) {
    if (transaction_lock_probe && std::strstr(path, "job.json")) {
        // Open the observer directly: the write fault must not also disable
        // the independent lock probe inside the instrumented C host seam.
        int probe = open(transaction_lock_probe, O_RDWR | O_CLOEXEC);
        assert(probe >= 0);
        assert(tny_jobs_host_lock_try(probe) == TNY_JOBS_LOCK_BUSY);
        close(probe);
    }
    if (!job_fault_mode || !std::strstr(path, "job.json"))
        return tny_jobs_host_write_private(path, data, size);
    auto *doc = jparse(static_cast<const char *>(data), size);
    assert(doc);
    auto *root = yyjson_doc_get_root(doc);
    const char *state = jget_str(root, "state");
    auto *items = jget(root, "items");
    bool terminal = state && std::strcmp(state, "failed") == 0;
    bool protected_start = jget_bool(root, "cleanup_hold", false);
    bool partial = yyjson_arr_size(items) == 2 &&
                   std::strcmp(jget_str(yyjson_arr_get(items, 0), "state"), "interrupted") == 0 &&
                   std::strcmp(jget_str(yyjson_arr_get(items, 1), "state"), "running") == 0;
    yyjson_doc_free(doc);
    if ((job_fault_mode == 2 && terminal) || (job_fault_mode == 3 && protected_start)) {
        ++job_fault_hits;
        return EIO;
    }
    if (job_fault_mode == 1 && partial) {
        assert(job_fault_hits == 1 && job_children[1] > 1);
        int status;
        assert(waitpid(job_children[1], &status, WNOHANG) == 0);
        int result = tny_jobs_host_write_private(path, data, size);
        assert(result == 0); /* unknown item persisted while the sibling is alive */
        int release = open(job_release_path, O_CREAT | O_WRONLY, 0600);
        assert(release >= 0);
        close(release);
        /* Reap the fixture-owned sibling before the parent kills this worker.
         * Its persisted state remains running at the tested crash boundary. */
        assert(waitpid(job_children[1], &status, 0) == job_children[1]);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert(write(job_notify_fd, "x", 1) == 1);
        for (;;) pause();
    }
    return tny_jobs_host_write_private(path, data, size);
}
#define tny_jobs_host_reap          checked_job_reap
#define tny_jobs_host_write_private checked_job_write
#ifndef TNY_JOBS_SOURCE
#define TNY_JOBS_SOURCE "../../src/core/jobs.cpp"
#endif
#include TNY_JOBS_SOURCE
#undef tny_jobs_host_write_private
#undef tny_jobs_host_reap

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

static_assert(!std::is_copy_constructible_v<jobs_txn>);
static_assert(!std::is_move_constructible_v<jobs_txn>);
static_assert(std::is_nothrow_destructible_v<jobs_txn>);
static_assert(std::is_same_v<decltype(jobs_txn::dir), tny::c_string>);
static_assert(std::is_same_v<decltype(jobs_txn::doc), tny::mutable_document>);

static void transaction_owners(const char *directory) {
    constexpr char id[] = "0123456789abcdef0123456789abcdef";
    constexpr char original[] =
        R"({"version":1,"kind":"job","id":"0123456789abcdef0123456789abcdef","state":"queued","items":[],"revision":0})";
    tny::c_string dir(path_join(directory, "transaction-owners"));
    assert(dir && tny_jobs_host_mkdir_private(dir.get()) == 0);
    tny::c_string record_path(jobs_file(dir.get(), "job.json"));
    tny::c_string lock_path(jobs_file(dir.get(), "state.lock"));
    assert(record_path && lock_path);
    assert(tny_jobs_host_write_private(record_path.get(), original, sizeof original - 1) == 0);
    int before = descriptor_count();
    char error[256];
    auto unchanged = [&] {
        size_t size = 0;
        tny::c_string bytes(file_slurp(record_path.get(), &size));
        assert(bytes && size == sizeof original - 1);
        assert(std::memcmp(bytes.get(), original, size) == 0);
        assert(tny_jobs_host_owner_state(lock_path.get()) == TNY_JOBS_OWNER_FREE);
        assert(descriptor_count() == before);
    };
    struct abandoned {};
    for (int cycle = 0; cycle < 32; ++cycle) {
        try {
            jobs_txn transaction;
            assert(jobs_txn_begin(dir.get(), id, &transaction, error, sizeof error) == 0);
            assert(transaction.dir && transaction.doc);
            assert(tny_jobs_host_owner_state(lock_path.get()) == TNY_JOBS_OWNER_HELD);
            jm_set_str(transaction.doc.get(), yyjson_mut_doc_get_root(transaction.doc.get()),
                       "state", "running");
            // No implicit persistence on ordinary destruction or C++ unwinding.
            if (cycle % 2) throw abandoned{};
        } catch (const abandoned &) {}
        unchanged();
    }
    {
        jobs_txn transaction;
        assert(jobs_txn_begin(dir.get(), id, &transaction, error, sizeof error) == 0);
        tny_alloc_scope_begin("transaction-close");
        transaction.reset();
        transaction.reset();
        assert(!transaction.dir && !transaction.doc && transaction.lock_fd.borrow() == -1);
        assert(tny_alloc_test_scope_count() == 0);
        unchanged();
        assert(jobs_txn_begin(dir.get(), id, &transaction, error, sizeof error) == 0);
        transaction.reset();
    }
    unchanged();
    // Every directory-copy/JSON allocation during admission releases its lock.
    size_t allocation_count = 0;
    for (size_t index = 0; index <= allocation_count; ++index) {
        jobs_txn transaction;
        char number[32];
        std::snprintf(number, sizeof number, "%zu", index);
        setenv("TNY_TEST_ALLOC_SCOPE", "transaction-json", 1);
        setenv("TNY_TEST_ALLOC_FAIL_AT", number, 1);
        tny_alloc_scope_begin("transaction-json");
        int rc = jobs_txn_begin(dir.get(), id, &transaction, error, sizeof error);
        if (index == 0) {
            assert(rc == 0);
            allocation_count = tny_alloc_test_scope_count();
            assert(allocation_count > 0);
        } else {
            assert(tny_alloc_test_scope_injected() && rc != 0);
            if (index == 1) {
                assert(rc == ENOMEM && std::strcmp(error, "out of memory") == 0);
                assert(tny_alloc_test_scope_count() == 1); // No record load after directory OOM.
            }
            assert(!transaction.dir && !transaction.doc && transaction.lock_fd.borrow() == -1);
        }
        unsetenv("TNY_TEST_ALLOC_SCOPE");
        unsetenv("TNY_TEST_ALLOC_FAIL_AT");
        tny_alloc_scope_begin("fixture");
        transaction.reset();
        unchanged();
    }
    // Real write/open/fsync/rename failures must not publish in-memory edits.
    for (int fault = 1; fault <= 4; ++fault) {
        jobs_txn transaction;
        assert(jobs_txn_begin(dir.get(), id, &transaction, error, sizeof error) == 0);
        transaction_lock_probe = lock_path.get();
        tny_resource_fault_set(fault, 0);
        int rc = jobs_txn_commit(&transaction);
        tny_resource_fault_set(0, 0);
        transaction_lock_probe = nullptr;
        assert(rc != 0);
        assert(!transaction.dir && !transaction.doc && transaction.lock_fd.borrow() == -1);
        unchanged();
    }
    {
        jobs_txn transaction;
        assert(jobs_txn_begin(dir.get(), id, &transaction, error, sizeof error) == 0);
        transaction_lock_probe = lock_path.get();
        assert(jobs_txn_commit(&transaction) == 0);
        transaction_lock_probe = nullptr;
        assert(!transaction.dir && !transaction.doc && transaction.lock_fd.borrow() == -1);
        tny::mutable_document saved(jobs_record_load(dir.get(), id, error, sizeof error));
        assert(saved && jm_int(yyjson_mut_doc_get_root(saved.get()), "revision", -1) == 1);
    }
    assert(tny_jobs_host_owner_state(lock_path.get()) == TNY_JOBS_OWNER_FREE);
    assert(descriptor_count() == before);
    assert(unlink(record_path.get()) == 0 && unlink(lock_path.get()) == 0);
    assert(rmdir(dir.get()) == 0);
    std::printf("transaction owners: 32 abandon/unwind cycles, %zu admission allocation faults, "
                "4 persistence faults, reset/reuse and explicit commit passed\n",
                allocation_count);
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
        assert(tny_alloc_test_owned_live() == 0);
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

static void durable_cleanup_faults(const char *directory) {
    /* These injected consuming-wait faults cover the POSIX seam. Native Job
     * handle cleanup is covered by the actual MSYS integration suite. */
    if (tny_process_scope_native_jobs()) return;
    auto *ctx = tny_ctx_new_explicit(directory, directory);
    assert(ctx);
    ctx->library_mode = false; /* this fixture owns real native children, not embedded jobs */
    std::snprintf(job_release_path, sizeof job_release_path, "%s/release-item", directory);
    for (int mode = 0; mode <= 3; ++mode) {
        char id[33], output_path[1024], error[256];
        std::snprintf(id, sizeof id, "%032d", mode + 100);
        std::snprintf(output_path, sizeof output_path, "%s/held-%d.png", directory, mode);
        char *dir = jobs_dir(ctx, id);
        assert(dir && mkdir_p(dir) == 0);
        char *owner_path = jobs_file(dir, "owner.lock");
        tny::lock_descriptor owner;
        owner.adopt(tny_jobs_host_lock_open(owner_path));
        assert(tny_jobs_host_lock_try(owner.borrow()) == TNY_JOBS_LOCK_ACQUIRED);
        const char *json =
            mode == 1 ? "{\"kind\":\"ask\",\"concurrency\":2,\"items\":["
                        "{\"index\":0,\"prompt\":\"quick\"},"
                        "{\"index\":1,\"prompt\":\"fixture wait\"}]}"
                      : "{\"kind\":\"ask\",\"items\":[{\"index\":0,\"prompt\":\"quick\"}]}";
        auto *args = jparse(json, std::strlen(json));
        jobs_request request{};
        assert(jobs_request_parse(ctx, yyjson_doc_get_root(args), &request, error, sizeof error) ==
               0);
        /* A real claim exercises the same shared reservation policy as image
         * jobs; these local ask children never contact a provider. */
        request.outputs[0] = xstrdup(output_path);
        auto *record = record_new(ctx, &request, id, dir, nullptr);
        assert(record && jobs_record_store(dir, record) == 0);
        yyjson_mut_doc_free(record);
        assert(reservation_claim_one(ctx, output_path, id, 0, 1, error, sizeof error) == 0);
        auto *payload_doc = yyjson_doc_mut_copy(args, jallocator());
        auto *root = yyjson_mut_doc_get_root(payload_doc);
        char *self = tny_process_self_path();
        jm_set_str(payload_doc, root, "self", self);
        jm_set_str(payload_doc, root, "cwd", directory);
        jm_set_int(payload_doc, root, "attempt", 1);
        std::free(self);
        char *serialized = jwrite(payload_doc);
        auto *payload = jparse(serialized, std::strlen(serialized));
        std::free(serialized);
        yyjson_mut_doc_free(payload_doc);
        job_fault_hits = 0;
        job_children[0] = job_children[1] = -1;
        if (mode == 1) {
            tny::pipe_pair notice;
            assert(notice.open() == 0);
            pid_t supervisor = fork();
            assert(supervisor >= 0);
            if (!supervisor) {
                notice.ends[0].reset();
                job_notify_fd = notice.ends[1].borrow();
                fcntl(job_notify_fd, F_SETFD, FD_CLOEXEC);
                job_fault_mode = mode;
                _exit(worker_supervise(ctx, dir, id, yyjson_doc_get_root(payload)) ? 2 : 0);
            }
            owner.reset(); /* only the supervisor now owns its inherited description */
            notice.ends[1].reset();
            pollfd event{notice.ends[0].borrow(), POLLIN, 0};
            int ready = tny_poll(&event, 1, 10000);
            char byte = 0;
            bool reached = ready > 0 && read(notice.ends[0].borrow(), &byte, 1) == 1;
            kill(supervisor, SIGKILL);
            int status;
            assert(waitpid(supervisor, &status, 0) == supervisor);
            assert(reached && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            unlink(job_release_path);
        } else {
            job_fault_mode = mode;
            int result = worker_supervise(ctx, dir, id, yyjson_doc_get_root(payload));
            job_fault_mode = 0;
            assert(mode == 0 ? result == 0 : result == EIO);
            assert(job_fault_hits == (mode == 0 ? 0 : 1));
            owner.reset();
        }
        assert(tny_jobs_host_owner_state(owner_path) == TNY_JOBS_OWNER_FREE);
        record = jobs_record_load(dir, id, error, sizeof error);
        assert(record);
        root = yyjson_mut_doc_get_root(record);
        if (mode == 1 || mode == 2) {
            assert(jm_bool(root, "cleanup_hold", false));
            assert(std::strcmp(jm_str(root, "state"), "running") == 0);
            if (mode == 1) {
                assert(std::strcmp(jm_str(jm_item(record, 0), "state"), "interrupted") == 0);
                assert(std::strcmp(jm_str(jm_item(record, 1), "state"), "running") == 0);
                assert(std::strcmp(jm_str(root, "cleanup"), "pending") == 0);
                /* Leave room for the key node, but force the replacement bool
                 * to grow the pool and hit a one-shot allocation failure. The
                 * old setter deleted the hold, then later writes succeeded. */
                assert(yyjson_mut_strcpy(record, "cleanup_hold"));
                record->val_pool.cur = record->val_pool.end - 1;
                auto *hold = yyjson_mut_obj_get(root, "cleanup_hold");
                setenv("TNY_TEST_ALLOC_SCOPE", "hold-relatch", 1);
                setenv("TNY_TEST_ALLOC_FAIL_AT", "1", 1);
                tny_alloc_scope_begin("hold-relatch");
                jm_set_bool(record, root, "cleanup_hold", true);
                size_t allocations = tny_alloc_test_scope_count();
                /* An allocation-free update leaves the fault armed: consume
                 * the last node and prove the very next growth really fails. */
                if (!tny_alloc_test_scope_injected()) {
                    assert(yyjson_mut_bool(record, false));
                    assert(!yyjson_mut_bool(record, false));
                }
                assert(tny_alloc_test_scope_injected());
                unsetenv("TNY_TEST_ALLOC_SCOPE");
                unsetenv("TNY_TEST_ALLOC_FAIL_AT");
                tny_alloc_scope_begin("fixture");
                assert(jobs_record_store(dir, record) == 0);
                assert(jm_bool(root, "cleanup_hold", false));
                assert(allocations == 0 && yyjson_mut_obj_get(root, "cleanup_hold") == hold);
                yyjson_mut_doc_free(record);
                record = jobs_record_load(dir, id, error, sizeof error);
                assert(record);
                root = yyjson_mut_doc_get_root(record);
                assert(jm_bool(root, "cleanup_hold", false));
            }
            assert(reservation_claim_one(ctx, output_path, "abcdef0123456789abcdef0123456789", 0, 1,
                                         error, sizeof error) != 0);
            assert(jobs_project(ctx, dir, id) == 0); /* loss projection preserves the latch */
            char *record_path = jobs_file(dir, "job.json");
            char *claim_path = reservation_path(ctx, output_path);
            size_t size;
            char *before_record = file_slurp(record_path, &size);
            char *before_claim = file_slurp(claim_path, &size);
            char selection[80];
            std::snprintf(selection, sizeof selection, "{\"id\":\"%s\"}", id);
            auto *retry = jparse(selection, std::strlen(selection));
            buf_t out{};
            assert(jobs_retry(ctx, yyjson_doc_get_root(retry), &out, error, sizeof error, nullptr,
                              nullptr) != 0);
            assert(std::strstr(error, "cleanup is unverified"));
            assert(jobs_rm(ctx, yyjson_doc_get_root(retry), &out, error, sizeof error) != 0);
            assert(std::strstr(error, "cleanup is unverified"));
            char *after_record = file_slurp(record_path, &size);
            char *after_claim = file_slurp(claim_path, &size);
            assert(before_record && after_record && std::strcmp(before_record, after_record) == 0);
            assert(before_claim && after_claim && std::strcmp(before_claim, after_claim) == 0);
            std::free(before_record);
            std::free(after_record);
            std::free(before_claim);
            std::free(after_claim);
            std::free(record_path);
            std::free(claim_path);
            buf_free(&out);
            yyjson_doc_free(retry);
        } else {
            assert(!jm_bool(root, "cleanup_hold", true));
            if (mode == 3) {
                assert(job_children[0] == -1);
                assert(std::strcmp(jm_str(root, "state"), "queued") == 0);
                char *log = jobs_item_log(dir, 0, 1);
                assert(access(log, F_OK) != 0);
                std::free(log);
            } else assert(std::strcmp(jm_str(root, "cleanup"), "complete") == 0);
            assert(reservation_claim_one(ctx, output_path, "abcdef0123456789abcdef0123456789", 0, 1,
                                         error, sizeof error) == 0);
        }
        yyjson_mut_doc_free(record);
        yyjson_doc_free(payload);
        jobs_request_free(&request);
        yyjson_doc_free(args);
        std::free(owner_path);
        std::free(dir);
    }
    tny_ctx_free(ctx);
    puts("job cleanup: allocation-fault re-latch, mixed-item loss and final-write failure retain "
         "claims; "
         "guard failure launches nothing; proven cleanup releases");
}

static void runner_shutdown(const char *directory) {
    auto *ctx = tny_ctx_new_explicit(directory, directory);
    assert(ctx);
    auto *session = session_new(ctx);
    assert(session && session_save(session) == 0);
    std::snprintf(writer_path, sizeof writer_path, "%s/lock", session->dir);
    probe_writer = true;
    tny_runner_opts opts{};
    opts.serve = true;
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
        ssize_t read_result;
        do {
            read_result = read(sentinel_gate.ends[0].borrow(), &byte, 1);
        } while (read_result < 0 && errno == EINTR);
        _exit(read_result == 0 ? 0 : 2);
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
    int cancel_result = jobs_cancel(ctx, yyjson_doc_get_root(args), &output, error, sizeof error);
    sentinel_gate.ends[1].reset(); /* EOF also works if a mutant killed the reader. */
    int status = 0;
    pid_t waited;
    do { waited = waitpid(sentinel, &status, 0); } while (waited < 0 && errno == EINTR);
    assert(waited == sentinel);
    assert(cancel_result == 0);
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

static void checkpoint_allocation_faults(const char *directory) {
    for (int spare = 0; spare <= 1; ++spare) {
        for (int save_fault = 0; save_fault <= 1; ++save_fault) {
            auto *ctx = tny_ctx_new_explicit(directory, directory);
            assert(ctx);
            auto *session = session_new(ctx);
            assert(session && session_lock_acquire(session) == 0);
            auto *doc = session->doc;
            auto *continuation = yyjson_mut_obj(doc);
            auto *resume = yyjson_mut_obj(doc);
            assert(yyjson_mut_obj_add_bool(doc, resume, "resumable", true));
            assert(yyjson_mut_obj_add_val(doc, continuation, "_resume", resume));
            assert(yyjson_mut_obj_add_val(doc, yyjson_mut_doc_get_root(doc), "continuation",
                                          continuation));
            assert(session_save(session) == 0);
            checkpoint_bool = yyjson_mut_obj_get(resume, "resumable");
            /* Force either the old key or value replacement to grow the pool.
             * Clearing must reach the real save with the fault still armed;
             * a rejected save must restore the same existing boolean. */
            doc->val_pool.cur = doc->val_pool.end - spare;
            auto *pool_cursor = doc->val_pool.cur;
            setenv("TNY_TEST_ALLOC_SCOPE", "checkpoint-consume", 1);
            setenv("TNY_TEST_ALLOC_FAIL_AT", "1", 1);
            tny_alloc_scope_begin("checkpoint-consume");
            fail_save = save_fault != 0;
            rn_state runner{};
            runner.session = session;
            assert(!rn_consume_checkpoint(&runner));
            assert(yyjson_mut_obj_get(resume, "resumable") == checkpoint_bool);
            assert(yyjson_mut_is_bool(checkpoint_bool) && yyjson_mut_get_bool(checkpoint_bool));
            if (fail_save) {
                assert(doc->val_pool.cur == pool_cursor);
                assert(tny_alloc_test_scope_count() == 0);
                /* Both updates left the fault armed; prove the next pool
                 * growth fails after any remaining spare node is consumed. */
                if (spare) assert(yyjson_mut_bool(doc, false));
                assert(!yyjson_mut_bool(doc, false));
            }
            assert(tny_alloc_test_scope_injected());
            fail_save = false;
            checkpoint_bool = nullptr;
            unsetenv("TNY_TEST_ALLOC_SCOPE");
            unsetenv("TNY_TEST_ALLOC_FAIL_AT");
            tny_alloc_scope_begin("fixture");
            assert(session_reload_locked(session, nullptr, 0) == 0);
            auto *packet = rn_disk_packet(session);
            assert(packet);
            yyjson_doc_free(packet);
            assert(rn_consume_checkpoint(&runner));
            assert(session_reload_locked(session, nullptr, 0) == 0);
            assert(rn_disk_packet(session) == nullptr);
            /* Consumed, malformed and missing flags cannot activate or save. */
            resume = yyjson_mut_obj_get(rn_continuation(session), "_resume");
            auto *flag = yyjson_mut_obj_get(resume, "resumable");
            int saves = checkpoint_save_calls;
            assert(!rn_consume_checkpoint(&runner));
            assert(yyjson_mut_set_int(flag, 1));
            assert(!rn_consume_checkpoint(&runner));
            assert(yyjson_mut_is_int(flag));
            assert(yyjson_mut_obj_remove_key(resume, "resumable"));
            assert(!rn_consume_checkpoint(&runner));
            assert(checkpoint_save_calls == saves);
            session_close(session);
            tny_ctx_free(ctx);
        }
    }
    puts("checkpoint allocation faults: in-place clear/restore, reload/retry and invalid flags "
         "passed");
}

int main(int argc, char **argv) {
    if (argc > 2 && std::strcmp(argv[1], "--cwd") == 0) {
        if (tny_process_scope_admit() != 0) return 2;
        char bytes[64];
        bool wait_for_release = false;
        ssize_t size;
        while ((size = read(STDIN_FILENO, bytes, sizeof bytes - 1)) > 0) {
            bytes[size] = '\0';
            if (std::strstr(bytes, "fixture wait")) wait_for_release = true;
        }
        if (wait_for_release) {
            char path[1024];
            std::snprintf(path, sizeof path, "%s/release-item", argv[2]);
            pid_t parent = getppid();
            int64_t deadline = monotonic_ms() + 10000;
            while (access(path, F_OK) != 0 && getppid() == parent && monotonic_ms() < deadline)
                tny_jobs_host_sleep_ms(10);
        }
        puts("fixture child drained");
        return 0;
    }
    assert(argc == 2);
    descriptor_transfers();
    job_acquisition_failures(argv[1]);
    transaction_owners(argv[1]);
    acquisition_faults(argv[1]);
    client_allocation_fault(argv[1]);
    item_lifecycle_loops(argv[1]);
    host_boundary_faults(argv[1]);
    unknown_cleanup();
    durable_cleanup_faults(argv[1]);
    runner_shutdown(argv[1]);
    metadata_pid_is_not_authority(argv[1]);
    consumed_checkpoint_stays_consumed(argv[1]);
    checkpoint_allocation_faults(argv[1]);
    puts("phase3 ownership oracles passed");
}
