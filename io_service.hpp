#pragma once
#include <functional>
#include <system_error>
#include <chrono>
#include <sys/poll.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <liburing.h>   // http://git.kernel.dk/liburing
#ifndef NDEBUG
#   include <execinfo.h>
#endif

#include "promise.hpp"
#include "task.hpp"

#ifndef LINUX_KERNEL_VERSION
#   define LINUX_KERNEL_VERSION 56
#endif

/** Fill an iovec struct using buf & size */
constexpr inline iovec to_iov(void *buf, size_t size) noexcept {
    return { buf, size };
}
/** Fill an iovec struct using string view */
constexpr inline iovec to_iov(std::string_view sv) noexcept {
    return to_iov(const_cast<char *>(sv.data()), sv.size());
}
/** Fill an iovec struct using std::array */
template <size_t N>
constexpr inline iovec to_iov(std::array<char, N>& array) noexcept {
    return to_iov(array.data(), array.size());
}

template <typename Fn>
struct on_scope_exit {
    on_scope_exit(Fn &&fn): _fn(std::move(fn)) {}
    ~on_scope_exit() { this->_fn(); }

private:
    Fn _fn;
};

[[nodiscard]]
constexpr inline __kernel_timespec dur2ts(std::chrono::nanoseconds dur) noexcept {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(dur);
    dur -= secs;
    return { secs.count(), dur.count() };
}

/** Convert errno to exception
 * @throw std::runtime_error / std::system_error
 * @return never
 */
[[noreturn]]
void panic(std::string_view sv, int err) {
#ifndef NDEBUG
    // https://stackoverflow.com/questions/77005/how-to-automatically-generate-a-stacktrace-when-my-program-crashes
    void *array[32];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 32);

    // print out all the frames to stderr
    fprintf(stderr, "Error: errno %d:\n", err);
    backtrace_symbols_fd(array, size, STDERR_FILENO);

    // __asm__("int $3");
#endif

    throw std::system_error(err, std::generic_category(), sv.data());
}

struct panic_on_err {
    panic_on_err(std::string_view _command, bool _use_errno)
        : command(_command)
        , use_errno(_use_errno) {}
    std::string_view command;
    bool use_errno;
};

inline int operator |(int ret, panic_on_err&& poe) {
    if (ret < 0) {
        if (poe.use_errno) {
            panic(poe.command, errno);
        } else {
            if (ret != -ETIME) panic(poe.command, -ret);
        }
    }
    return ret;
}
template <bool nothrow>
inline task<int> operator |(task<int, nothrow> tret, panic_on_err&& poe) {
    co_return (co_await tret) | std::move(poe);
}

class io_service {
public:
    /** Init io_service / io_uring object
     * @see io_uring_setup(2)
     * @param entries Maximum sqe can be gotten without submitting
     * @param flags flags used to init io_uring
     */
    io_service(int entries = 64, unsigned flags = 0) {
        io_uring_queue_init(entries, &ring, flags) | panic_on_err("queue_init", false);
    }

    /** Destroy io_service / io_uring object */
    ~io_service() noexcept {
        io_uring_queue_exit(&ring);
    }

    // io_service is not copyable. It can be moveable but humm...
    io_service(const io_service&) = delete;
    io_service& operator =(const io_service&) = delete;

public:

#define DEFINE_AWAIT_OP(operation)                                                   \
    task<int, true> operation(                                                       \
        int fd,                                                                      \
        iovec* iovecs,                                                               \
        unsigned nr_vecs,                                                            \
        off_t offset,                                                                \
        uint8_t iflags = 0                                                           \
    ) noexcept {                                                                     \
        auto* sqe = io_uring_get_sqe_safe();                                         \
        io_uring_prep_##operation(sqe, fd, iovecs, nr_vecs, offset);                 \
        return await_work(sqe, iflags);                                              \
    }                                                                                \

    /** Read data into multiple buffers asynchronously
     * @see preadv2(2)
     * @see io_uring_enter(2) IORING_OP_READV
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(readv)

    /** Write data into multiple buffers asynchronously
     * @see pwritev2(2)
     * @see io_uring_enter(2) IORING_OP_WRITEV
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(writev)
#undef DEFINE_AWAIT_OP

#if LINUX_KERNEL_VERSION >= 56
#define DEFINE_AWAIT_OP(operation)                                                   \
    task<int, true> operation(                                                       \
        int fd,                                                                      \
        const void* buf,                                                             \
        unsigned nbytes,                                                             \
        off_t offset,                                                                \
        uint8_t iflags = 0                                                           \
    ) {                                                                              \
        auto* sqe = io_uring_get_sqe_safe();                                         \
        io_uring_prep_##operation(sqe, fd, const_cast<void *>(buf), nbytes, offset); \
        return await_work(sqe, iflags);                                              \
    }
#else
#define DEFINE_AWAIT_OP(operation)                                                   \
    task<int, true> operation(                                                       \
        int fd,                                                                      \
        const void* buf,                                                             \
        unsigned nbytes,                                                             \
        off_t offset,                                                                \
        uint8_t iflags = 0                                                           \
    ) noexcept {                                                                     \
        iovec iov = { .iov_base = const_cast<void *>(buf), .iov_len = nbytes };      \
        co_return co_await operation##v(fd, &iov, 1, offset, iflags);                \
    }
#endif

    /** Read from a file descriptor at a given offset asynchronously
     * @see pread(2)
     * @see io_uring_enter(2) IORING_OP_READ
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(read)

    /** Write to a file descriptor at a given offset asynchronously
     * @see pwrite(2)
     * @see io_uring_enter(2) IORING_OP_WRITE
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(write)
#undef DEFINE_AWAIT_OP

#define DEFINE_AWAIT_OP(operation)                                                   \
    task<int, true> operation(                                                       \
        int fd,                                                                      \
        void* buf,                                                                   \
        unsigned nbytes,                                                             \
        off_t offset,                                                                \
        int buf_index,                                                               \
        uint8_t iflags = 0                                                           \
    ) noexcept {                                                                     \
        auto* sqe = io_uring_get_sqe_safe();                                         \
        io_uring_prep_##operation(sqe, fd, buf, nbytes, offset, buf_index);          \
        return await_work(sqe, iflags);                                              \
    }

    /** Read data into a fixed buffer asynchronously
     * @see preadv2(2)
     * @see io_uring_enter(2) IORING_OP_READ_FIXED
     * @param buf_index the index of buffer registered with register_buffers
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(read_fixed)

    /** Write data into a fixed buffer asynchronously
     * @see pwritev2(2)
     * @see io_uring_enter(2) IORING_OP_WRITE_FIXED
     * @param buf_index the index of buffer registered with register_buffers
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(write_fixed)
#undef DEFINE_AWAIT_OP

    /** Synchronize a file's in-core state with storage device asynchronously
     * @see fsync(2)
     * @see io_uring_enter(2) IORING_OP_FSYNC
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    task<int, true> fsync(
        int fd,
        unsigned fsync_flags,
        uint8_t iflags = 0
    ) noexcept {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_fsync(sqe, fd, fsync_flags);
        return await_work(sqe, iflags);
    }

    /** Sync a file segment with disk asynchronously
     * @see sync_file_range(2)
     * @see io_uring_enter(2) IORING_OP_SYNC_FILE_RANGE
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    task<int, true> sync_file_range(
        int fd,
        off64_t offset,
        off64_t nbytes,
        unsigned sync_range_flags,
        uint8_t iflags = 0
    ) noexcept {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_rw(IORING_OP_SYNC_FILE_RANGE, sqe, fd, nullptr, nbytes, offset);
        sqe->sync_range_flags = sync_range_flags;
        return await_work(sqe, iflags);
    }

#define DEFINE_AWAIT_OP(operation)                                                   \
    task<int, true> operation(                                                       \
        int sockfd,                                                                  \
        msghdr* msg,                                                                 \
        uint32_t flags,                                                              \
        uint8_t iflags = 0                                                           \
    ) noexcept {                                                                     \
        auto* sqe = io_uring_get_sqe_safe();                                         \
        io_uring_prep_##operation(sqe, sockfd, msg, flags);                          \
        return await_work(sqe, iflags);                                              \
    }                                                                                \

    /** Receive a message from a socket asynchronously
     * @see recvmsg(2)
     * @see io_uring_enter(2) IORING_OP_RECVMSG
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(recvmsg)

    /** Send a message on a socket asynchronously
     * @see sendmsg(2)
     * @see io_uring_enter(2) IORING_OP_SENDMSG
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(sendmsg)
#undef DEFINE_AWAIT_OP

#if LINUX_KERNEL_VERSION >= 56
#define DEFINE_AWAIT_OP(operation)                                                     \
    task<int, true> operation(                                                         \
        int sockfd,                                                                    \
        const void* buf,                                                               \
        unsigned nbytes,                                                               \
        uint32_t flags,                                                                \
        uint8_t iflags = 0                                                             \
    ) noexcept {                                                                       \
        auto* sqe = io_uring_get_sqe_safe();                                           \
        io_uring_prep_##operation(sqe, sockfd, const_cast<void *>(buf), nbytes, flags);\
        return await_work(sqe, iflags);                                                \
    }
#else
#define DEFINE_AWAIT_OP(operation)                                                     \
    task<int, true> operation(                                                         \
        int sockfd,                                                                    \
        const void* buf,                                                               \
        unsigned nbytes,                                                               \
        uint32_t flags,                                                                \
        uint8_t iflags = 0                                                             \
    ) noexcept {                                                                       \
        iovec iov = { .iov_base = const_cast<void *>(buf), .iov_len = nbytes };        \
        msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };                             \
        co_return co_await operation##msg (sockfd, &msg, flags, iflags);               \
    }
#endif

    /** Receive a message from a socket asynchronously
     * @see recv(2)
     * @see io_uring_enter(2) IORING_OP_RECV
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(recv)

    /** Send a message on a socket asynchronously
     * @see send(2)
     * @see io_uring_enter(2) IORING_OP_SEND
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    DEFINE_AWAIT_OP(send)
#undef DEFINE_AWAIT_OP

    /** Wait for an event on a file descriptor asynchronously
     * @see poll(2)
     * @see io_uring_enter(2)
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    task<int, true> poll(
        int fd,
        short poll_mask,
        uint8_t iflags = 0
    ) noexcept {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_poll_add(sqe, fd, poll_mask);
        return await_work(sqe, iflags);
    }

    /** Enqueue a NOOP command, which eventually acts like pthread_yield when awaiting
     * @see io_uring_enter(2) IORING_OP_NOP
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    task<int, true> yield(
        uint8_t iflags = 0
    ) noexcept {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_nop(sqe);
        return await_work(sqe, iflags);
    }

    /** Accept a connection on a socket asynchronously
     * @see accept4(2)
     * @see io_uring_enter(2) IORING_OP_ACCEPT
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    task<int, true> accept(
        int fd,
        sockaddr *addr,
        socklen_t *addrlen,
        int flags = 0,
        uint8_t iflags = 0
    ) noexcept {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
        return await_work(sqe, iflags);
    }

    /** Initiate a connection on a socket asynchronously
     * @see connect(2)
     * @see io_uring_enter(2) IORING_OP_CONNECT
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    task<int, true> connect(
        int fd,
        sockaddr *addr,
        socklen_t addrlen,
        int flags = 0,
        uint8_t iflags = 0
    ) noexcept {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_connect(sqe, fd, addr, addrlen);
        return await_work(sqe, iflags);
    }

    /** Wait for specified duration asynchronously
     * @see io_uring_enter(2) IORING_OP_TIMEOUT
     * @param ts initial expiration, timespec
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    task<int, true> timeout(
        __kernel_timespec *ts,
        uint8_t iflags = 0
    ) noexcept {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_timeout(sqe, ts, 0, 0);
        co_return co_await await_work(sqe, iflags);
    }

    task<int, true> timeout(
        std::chrono::nanoseconds dur,
        uint8_t iflags = 0
    ) noexcept {
        __kernel_timespec ts = dur2ts(dur);
        // Everytime we pass pointers into other function,
        // we MUST use co_return co_await to insure that variable
        // isn't destructed before await_work truly returns
        co_return co_await timeout(&ts, iflags);
    }

    /** Open and possibly create a file asynchronously
     * @see io_uring_enter(2) IORING_OP_OPENAT
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    task<int, true> openat(
        int dfd,
        const char *path,
        int flags,
        mode_t mode,
        uint8_t iflags = 0
    ) noexcept {
#if LINUX_KERNEL_VERSION >= 56
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_openat(sqe, dfd, path, flags, mode);
        return await_work(sqe, iflags);
#else
        co_await yield(iflags);
        co_return ::openat(dfd, path, flags, mode);
#endif
    }

    /** Close a file descriptor asynchronously
     * @see io_uring_enter(2) IORING_OP_CLOSE
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    task<int, true> close(
        int fd,
        uint8_t iflags = 0
    ) noexcept {
#if LINUX_KERNEL_VERSION >= 56
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_close(sqe, fd);
        return await_work(sqe, iflags);
#else
        co_await yield(iflags);
        co_return ::close(fd);
#endif
    }

private:
    task<int, true> await_work(
        io_uring_sqe* sqe,
        uint8_t iflags
    ) noexcept {
        promise<int, true> p([] (promise<int, true>* self, void* user_data) noexcept {
            auto* service = static_cast<io_service *>(user_data);
            io_uring_sqe *sqe = service->io_uring_get_sqe_safe();
            io_uring_prep_cancel(sqe, self, 0);
        }, this);
        io_uring_sqe_set_flags(sqe, iflags);
        io_uring_sqe_set_data(sqe, &p);
        co_return co_await p;
    }

public:
    /** Get a sqe pointer that can never be NULL
     * @param ring pointer to inited io_uring struct
     * @return pointer to `io_uring_sqe` struct (not NULL)
     */
    [[nodiscard]]
    io_uring_sqe* io_uring_get_sqe_safe() noexcept {
        auto* sqe = io_uring_get_sqe(&ring);
        if (__builtin_expect(!!sqe, true)) {
            return sqe;
        } else {
#ifndef NDEBUG
            printf(__FILE__ ": SQ is full, flushing %u cqe(s)\n", cqe_count);
#endif
            io_uring_cq_advance(&ring, cqe_count);
            cqe_count = 0;
            io_uring_submit(&ring);
            sqe = io_uring_get_sqe(&ring);
            assert(sqe && "sqe should not be NULL");
            return sqe;
        }
    }

    /** Wait for an event forever, blocking
     * @see io_uring_wait_cqe
     * @see io_uring_enter(2)
     * @return a pair of promise pointer (used for resuming suspended coroutine) and retcode of finished command
     */
    template <typename T, bool nothrow>
    T run(const task<T, nothrow>& t) noexcept(nothrow) {
        while (!t.done()) {
            io_uring_submit_and_wait(&ring, 1);

            io_uring_cqe *cqe;
            unsigned head;

            io_uring_for_each_cqe(&ring, head, cqe) {
                ++cqe_count;
                auto coro = static_cast<promise<int, true> *>(io_uring_cqe_get_data(cqe));
                if (coro) coro->resolve(cqe->res);
            }

#ifndef NDEBUG
            printf(__FILE__ ": Found %u cqe(s), looping...\n", cqe_count);
#endif
            io_uring_cq_advance(&ring, cqe_count);
            cqe_count = 0;
        }

        return t.get_result();
    }

public:
    /** Register files for I/O
     * @param fds fds to register
     * @see io_uring_register(2) IORING_REGISTER_FILES
     */
    void register_files(std::initializer_list<int> fds) {
        register_files(fds.begin(), (unsigned int)fds.size());
    }
    void register_files(const int *files, unsigned int nr_files) {
        io_uring_register_files(&ring, files, nr_files) | panic_on_err("io_uring_register_files", false);
    }

    /** Update registered files
     * @see io_uring_register(2) IORING_REGISTER_FILES_UPDATE
     */
    void register_files_update(unsigned off, int *files, unsigned nr_files) {
        io_uring_register_files_update(&ring, off, files, nr_files) | panic_on_err("io_uring_register_files", false);
    }

    /** Unregister all files
     * @see io_uring_register(2) IORING_UNREGISTER_FILES
     */
    int unregister_files() noexcept {
        return io_uring_unregister_files(&ring);
    }

public:
    /** Register buffers for I/O
     * @param ioves array of iovec to register
     * @see io_uring_register(2) IORING_REGISTER_BUFFERS
     */
    template <unsigned int N>
    void register_buffers(iovec (&&ioves) [N]) {
        register_buffers(&ioves[0], N);
    }
    void register_buffers(const struct iovec *iovecs, unsigned nr_iovecs) {
        io_uring_register_buffers(&ring, iovecs, nr_iovecs) | panic_on_err("io_uring_register_buffers", false);
    }

    /** Unregister all buffers
     * @see io_uring_register(2) IORING_UNREGISTER_BUFFERS
     */
    int unregister_buffers() noexcept {
        return io_uring_unregister_buffers(&ring);
    }

public:
    /** Return internal io_uring handle */
    [[nodiscard]]
    io_uring& get_handle() noexcept {
        return ring;
    }

private:
    io_uring ring;
    unsigned cqe_count = 0;
};
