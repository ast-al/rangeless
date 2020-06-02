/*
================================================================================
 
                             PUBLIC DOMAIN NOTICE
                 National Center for Biotechnology Information
 
  This software is a "United States Government Work" under the terms of the
  United States Copyright Act.  It was written as part of the author's official
  duties as a United States Government employees and thus cannot be copyrighted. 
  This software is freely available to the public for use. The National Library
  of Medicine and the U.S. Government have not placed any restriction on its use
  or reproduction.
 
  Although all reasonable efforts have been taken to ensure the accuracy and
  reliability of this software, the NLM and the U.S. Government do not and
  cannot warrant the performance or results that may be obtained by using this
  software. The NLM and the U.S. Government disclaim all warranties, expressed
  or implied, including warranties of performance, merchantability or fitness
  for any particular purpose.
 
  Please cite NCBI in any work or product based on this material.
 
================================================================================

  Author: Alex Astashyn

*/
#ifndef RANGELESS_MT_HPP_
#define RANGELESS_MT_HPP_

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <chrono>
#include <cassert>

#include "fn.hpp"


/////////////////////////////////////////////////////////////////////////////

namespace rangeless
{ 
   
namespace mt
{ 

/////////////////////////////////////////////////////////////////////////////
/// \brief A simple timer.
struct timer
{
    /// returns the time elapsed since timer's instantiation, in seconds.
    /// To reset: `my_timer = mt::timer{}`.
    operator double() const
    {
        return double(std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock_t::now() - start_timepoint).count()) * 1e-9;
    }
private:
    using clock_t = std::chrono::steady_clock;
    clock_t::time_point start_timepoint = clock_t::now();
};

namespace lockables
{


class partial_spin_mutex : public std::mutex
{
public:
    partial_spin_mutex() = default;

    void lock() noexcept
    {   
        for(size_t i = 0; i < 16; i++) {
            if(try_lock()) {
                return;
            }

            std::this_thread::sleep_for(
                std::chrono::microseconds(1));
        }

        std::mutex::lock();
    }
};

/////////////////////////////////////////////////////////////////////////////
/// \brief Can be used as alternative to std::mutex.
///
/// It is faster than std::mutex, yet does not aggressively max-out the CPUs.
/// NB: may cause thread starvation
class atomic_lock
{    
    alignas(128) std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
    // alignas to avoid false-sharing

    // todo: could we implement exponential-backoff algorithm that prevents
    // thread-starvation in mpsc spmc scenarios?

public:

    atomic_lock() = default;

    // non-copyable and non-movable, as with std::mutex
    
               atomic_lock(const atomic_lock&) = delete;
    atomic_lock& operator=(const atomic_lock&) = delete;

               atomic_lock(atomic_lock&&) = delete;
    atomic_lock& operator=(atomic_lock&&) = delete;

    bool try_lock() noexcept
    {    
        return !m_flag.test_and_set(std::memory_order_acquire);
    }    

    void lock() noexcept
    {   
        while(m_flag.test_and_set(std::memory_order_acquire)) { 
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(1)); // the actual time is greater, depends on scheduler
        }

        // Related:
        // https://stackoverflow.com/questions/7868235/why-is-sleeping-not-allowed-while-holding-a-spinlock
        //
        // Note that here we are sleepining while NOT holding 
        // the lock, but while trying to acquire it.
        // 
        // If we did NOT sleep here, the polling
        // thread would deadlock on a single-core system
        // if the thread holding the lock goes to sleep.
        //
        // Caveat emptor.
    }    

    void unlock() noexcept
    {    
        m_flag.clear(std::memory_order_release);
    }    
};   

}

class synchronized_queue_base
{
public:
    enum class status { success, closed, timeout };

    // deriving from end_seq::exception to enable
    // adapting queue as input-seq: fn::seq(my_queue.pop) % fn::for_each(...)
    // (push will throw queue_closed, which will terminate the seq).

    class queue_closed : public rangeless::fn::end_seq::exception
    {};

    // NB: Core guideline T.62: Place non-dependent class template members in a non-templated base class
};


/////////////////////////////////////////////////////////////////////////////
/*! \brief Optionally-bounded blocking concurrent MPMC queue.
 *
 *   - Supports not-copy-constructible/not-default-constructible value-types (just requires move-assigneable).
 *   - Can be used with lockables other than `std::mutex`, e.g. `mt::atomic_lock`.
 *   - Contention-resistant: when used with `mt::atomic_lock` the throughput is comparable to state-of-the-art lock-free implementations.
 *   - Short and simple implementation using only c++11 standard library primitives.
 *   - Provides RAII-based closing semantics to communicate end-of-inputs 
 *     from the pushing end or failure/going-out-of-scope from the popping end.
 *
 * Related:
 * <br> <a href="http://www.boost.org/doc/libs/1_66_0/libs/fiber/doc/html/fiber/synchronization/channels/buffered_channel.html">`boost::fibers::buffered_channel`</a>
 * <br> <a href="http://www.boost.org/doc/libs/1_66_0/doc/html/thread/sds.html">`boost::sync_bounded_queue`</a>
 * <br> <a href="http://www.boost.org/doc/libs/1_66_0/doc/html/boost/lockfree/queue.html">`boost::lockfree::queue`</a>
 * <br> <a href="https://software.intel.com/en-us/node/506200">`tbb::concurrent_queue`</a>
 * <br> <a href="https://github.com/cameron314/concurrentqueue">`moodycamel::BlockingConcurrentQueue`</a>
 *
@code
    // A toy example to compute sum of lengths of strings in parallel.
    //
    // Spin-off a separate async-task that enqueues jobs 
    // to process a single input, and enqueues the 
    // futures into a synchronized queue, while accumulating 
    // the ready results from the queue in this thread.

    using queue_t = mt::synchronized_queue<std::future<size_t> >;
    queue_t queue{ 10 };

    auto fut = std::async(std::launch::async,[ &queue ]
    {
        auto close_on_exit = queue.close();

        for(std::string line; std::getline(std::cin, line); ) {
            queue <<= 
                std::async(
                    std::launch::async, 
                    [](const std::string& s) { 
                        return s.size(); 
                    },
                    std::move(line));
        }
    });

    size_t total = 0;
    queue >>= [&](queue_t::value_type x) { total += x.get(); };
    fut.get(); // rethrow exception, if any.
@endcode
*/
template <typename T, class BasicLockable = std::mutex>
class synchronized_queue : public synchronized_queue_base
{
public:
    using value_type = T;

    ///@{ 

    synchronized_queue(size_t cap = 1024)
      : m_capacity{ cap }
    {}

    ~synchronized_queue() = default;
    // What if there are elements in the queue? 
    // If there are active poppers, we ought to 
    // let them finish, but if there aren't any,
    // this will block indefinitely, so we'll 
    // leave it to the user code to manage the
    // lifetime across multiple threads.
    //
    // Should we call x_close() ?
    // No point - we're already destructing.
    // (it also grabs the m_queue_mutex, which 
    // theoretically may throw).


    // After careful consideration, decided not to provide 
    // move-semantics; copy and move constructors are implicitly
    // deleted.
    //
    // A synchronized_queue can be thought of buffered mutex
    // (i.e. a synchronization primitive rather than just a 
    // data structure), and mutexes are not movable.
    
    ///@}
    ///@{

    // push and pop are implemented as callable function-objects fields
    // rather than plain methods to enable usage like:
    //
    // Adapt queue as input-range:
    // fn::seq(synchronized_queue.pop) % fn::for_each(...)
    //
    // Adapt queue as sink-function:
    // std::move(inputs) % fn::for_each(my_queue.push);
    //
    // Adapt queue as output-iterator:
    // std::copy(inputs.begin(), inputs.end(), my_queue.push);

    /////////////////////////////////////////////////////////////////////////
    /// Implements insert_iterator and unary-invokable.
    struct push_t
    {
        using iterator_category = std::output_iterator_tag;
        using   difference_type = void;
        using        value_type = synchronized_queue::value_type;
        using           pointer = void;
        using         reference = void;

        synchronized_queue& m_queue;

        push_t& operator=(value_type val)
        {
            this->operator()(std::move(val));
            return *this;
        }

        push_t& operator*()     { return *this; }
        push_t& operator++()    { return *this; }
        push_t  operator++(int) { return *this; }

        /// Blocking push. May throw `queue_closed`
        void operator()(value_type val) noexcept(false)
        {
            // NB: if this throws, val is gone. If the user needs 
            // to preserve it in case of failure (strong exception
            // guarantee), it should be using try_push which takes 
            // value by rvalue-reference and moves it only in case of success.
            //
            // We could do the same here, but that would mean
            // that the user must always pass as rvalue, and 
            // to pass by copy it would have to do so explicitly, e.g.
            //
            // void operator()(value_type&& val);
            // queue.push(std::move(my_value));
            // queue.push(my_const_ref); // won't compile.
            // queue.push(queueue_t::value_type( my_const_ref )); // explicit copy
            //
            // I think this would actually be a good thing, as it 
            // makes the copying visible, but all other synchronied-queue
            // APIs allow pushing by const-or-forwarding-references, 
            // so we have allow the same for the sake of consistency.

            const status st = 
                m_queue.try_push(std::move(val), no_timeout_sentinel_t{});

            assert(st != status::timeout);

            if(st == status::closed) {
                throw queue_closed{};
            }

            assert(st == status::success);
        }
    };
    friend struct push_t;


    /////////////////////////////////////////////////////////////////////////
    /// Blocking push. May throw `queue_closed`.
    push_t push = { *this };


    /////////////////////////////////////////////////////////////////////////
    struct pop_t
    {
        synchronized_queue& m_queue;

        value_type operator()()
        {
            return m_queue.x_blocking_pop();
        }
    };

    friend struct pop_t;

    /////////////////////////////////////////////////////////////////////////
    /// Blocking pop. May throw `queue_closed`.
    pop_t pop = { *this };


    /////////////////////////////////////////////////////////////////////////

    ///@}
    ///@{

    /// \brief pop() the values into the provided sink-function until closed and empty.
    ///
    /// e.g. `queue >>= [&out_it](T x){ *out_it++ = std::move(x); };`
    /// <br>Queue is automatically closed if exiting via exception, unblocking the pushers.
    template<typename F>
    auto operator>>=(F&& sink) -> decltype((void)sink(this->pop()))
    {
        auto guard = this->close();

        while(true) {
            bool threw_in_pop = true;

            try {
                value_type val = this->pop();
                threw_in_pop = false;
                sink(std::move(val));

            } catch(queue_closed&) {

                if(threw_in_pop) {
                    break; // threw in pop()
                } else {
                    throw; // threw in sink() - not our business - rethrow;
                    //
                    // This could be an unhandled exception from
                    // sink that is from some different queue that we
                    // shouldn't be intercepting.
                    //
                    // If sink intends to close the queue
                    // (e.g. break-out), it can do it explicitly
                    // via the close-guard;
                }
            }
        }
        
        assert(closed() && m_queue.empty());
    }

    ///@}
    ///@{ 


    /////////////////////////////////////////////////////////////////////////
    /// In case of success, the value will be moved-from.
    template <typename Duration = std::chrono::milliseconds>
    status try_push(value_type&& value, Duration timeout = {})
    {
        // NB: we could have taken value as value_type&, but
        // the user-code may forget or not expect that the value 
        // will be moved-from and may try to use it.
        //
        // With rvalue-reference the caller-code has to, e.g.
        //     auto state = queue.try_push(std::move(x));
        //
        // making the move explicitly visible.

        guard_t push_guard{ m_push_mutex };
        lock_t queue_lock{ m_queue_mutex };
        
        const bool ok = x_wait_until([this]
    	    {
    		    return m_queue.size() < m_capacity || !m_capacity;
    	    }, 
            m_can_push, queue_lock, timeout);

        if(!ok) {
            return status::timeout;
        }

        if(!m_capacity) {
             return status::closed;
        }

        assert(m_queue.size() < m_capacity); 

        // if push throws, is the value moved-from?
        // No. std::move is just an rvalue_cast - no-op
        // if the move-assignment never happens.
        m_queue.push(std::move(value));

        queue_lock.unlock();

        m_can_pop.notify_one();
        return status::success;
    }


    /////////////////////////////////////////////////////////////////////////
    /// In case of success, the value will be move-assigned.
    template <typename Duration = std::chrono::milliseconds>
    status try_pop(value_type& value, Duration timeout = {})
    {
        guard_t pop_guard{ m_pop_mutex };
        lock_t queue_lock{ m_queue_mutex };

        bool ok = x_wait_until([this]
            {
                return !m_queue.empty() || !m_capacity;
            },
            m_can_pop, queue_lock, timeout);

        if(!ok) {
            return status::timeout;
        }

        if(!m_queue.empty()) {
            ;
        } else if(!m_capacity) {
            return status::closed;
        } else {
            assert(false);
        }

        value = std::move(m_queue.front());
        m_queue.pop();

        queue_lock.unlock();
        m_can_push.notify_one();

        return status::success;
    }

    ///@}
    ///@{ 

    /////////////////////////////////////////////////////////////////////////
    size_t approx_size() const noexcept
    {
        return m_queue.size();
    }

    size_t capacity() const noexcept
    {
        return m_capacity;
    }

    bool closed() const noexcept
    {
        return !m_capacity;
    }

    /////////////////////////////////////////////////////////////////////////
    struct close_guard
    {
    private:
        synchronized_queue* ptr;

    public:
        close_guard(synchronized_queue& queue) : ptr{ &queue }
        {}

        close_guard(const close_guard&) = default; // -Weffc++ warning
        close_guard& operator=(const close_guard&) = default;

        void reset()
        {
            ptr = nullptr;
        }

        ~close_guard()
        {
            if(ptr) {
                ptr->x_close();
            }
        }
    };

    /// \brief Return an RAII object that will close the queue in its destructor.
    /// 
    /// @code
    /// auto guard = queue.close(); // close the queue when leaving scope
    /// queue.close(); // close the queue now (guard's is destroyed immediately)
    /// @endcode
    ///
    /// <br> NB: closing is non-blocking.
    /// <br>Blocked calls to try_push()/try_pop() shall return with status::closed.
    /// <br>Blocked calls to push()/pop() shall throw `queue_closed`.
    /// <br>Subsequent calls to push()/try_push() shall do as above.
    /// <br>Subsequent calls to pop()/try_pop() will succeed
    ///   until the queue becomes empty, and throw/return-closed thereafter.
    close_guard close() noexcept
    {
       return close_guard{ *this };
    }

    ///@}

private:
    using guard_t = std::lock_guard<BasicLockable>;
    using  lock_t = std::unique_lock<BasicLockable>;
    using queue_t = std::queue<value_type>;

    using condvar_t = typename std::conditional<
        std::is_same<BasicLockable, std::mutex>::value,
            std::condition_variable,
            std::condition_variable_any        >::type;

    struct no_timeout_sentinel_t
    {};

    template<typename F>
    bool x_wait_until(F condition, condvar_t& cv, lock_t& lock, no_timeout_sentinel_t)
    {    
        cv.wait(lock, std::move(condition));
        return true;
    }    

    template<typename Duration, typename F>
    bool x_wait_until(F condition, condvar_t& cv, lock_t& lock, Duration duration)
    {    
        return cv.wait_for(lock, duration, std::move(condition));
    } 

    value_type x_blocking_pop()
    {
        guard_t pop_guard{ m_pop_mutex };
        lock_t queue_lock{ m_queue_mutex };

        m_can_pop.wait(
            queue_lock,
            [this]
            {
                return !m_queue.empty() || !m_capacity;
            });

        if(m_queue.empty()) {
            throw queue_closed{};
        }

        value_type ret = std::move(m_queue.front());
        m_queue.pop();

        queue_lock.unlock();
        m_can_push.notify_one();

        return ret;
    }

    /////////////////////////////////////////////////////////////////////////
    /// \brief Closes the queue for more pushing.
    ///
    void x_close()
    {
        {
            guard_t g{ m_queue_mutex }; 
            m_capacity = 0;
        }
        m_can_pop.notify_all();
        m_can_push.notify_all();
    }

    // NB: open() is not provided, such that if closed() returns true,
    // we know for sure that it's staying that way. 


    /////////////////////////////////////////////////////////////////////////

    // To reduce lock contention between producers and consumers
    // we're using separate queues for pushing and popping,
    // and lock them both and swap as necessary.

                 size_t m_capacity       = size_t(-1); // 0 means closed
                queue_t m_queue          = queue_t{};
          BasicLockable m_queue_mutex    = {};

          BasicLockable m_push_mutex     = {}; // these are to manage contention
          BasicLockable m_pop_mutex      = {}; // in MPSC/SPMC use-cases.

              condvar_t m_can_push       = {};   
              condvar_t m_can_pop        = {};


    // Notes: 
    //
    // Could use a single ring-buffer instead of pair of queues, and two
    // cursors, m_head and m_tail. Using the pair-of-queues allows
    // the size to grow dynamically, without preallocating the storage.
    //
    // Throughput for typical usage is limited by mt-performance of malloc,
    // rather than synchronized_queue, when the memory is allocated in
    // one thread and freed in another (e.g. future/promise shared-state, 
    // allocating memory for results in a worker-thread and passing the 
    // ownership to consumer thread, etc. In this case malloc has to 
    // synchronize memory ownership among threads under the hood. 
    //
    // MT-optimized allocators, such as libtcmalloc or libllalloc are 
    // much faster in this scenario.
    //
    // NB: Using fancy wait-free queue in parallelized-pipeline implementation,
    // or substituting mutexes with atomic_locks in this implementation
    // actually hurts performance somewhat in cpu-bound applications
    // even though it increases queue throughput for POD types.

}; // synchronized_queue

} // namespace mt


namespace fn
{

namespace impl
{
    struct async_wr
    {
        size_t queue_size;

        template<typename InGen>
        struct gen
        {
            using value_type = typename InGen::value_type;
            using queue_t = mt::synchronized_queue<maybe<value_type>>;

                        InGen gen;      // nullary generator yielding maybe<...>
                 const size_t queue_size;
     std::unique_ptr<queue_t> queue;    // as unique-ptr, because non-moveable
            std::future<void> fut;

            auto operator()() -> maybe<value_type>
            {
                if(!queue) {
                    queue.reset(new queue_t{ queue_size });
                    fut = std::async(std::launch::async, [this]
                    {
                        auto guard = queue->close();
                        for(auto x = gen(); x; x = gen()) {
                            queue->push(std::move(x));
                        }
                        queue->push({});
                    });
                }

                auto x = queue->pop();
                if(!x) {
                    fut.get(); // allow exceptions to rethrow
                    assert(queue->closed());
                }
                return x;
            }
        };

        RANGELESS_FN_OVERLOAD_FOR_SEQ(queue_size, {}, {})
    };
}

    /// \brief Wrap generating `seq` in an async-task.
    /*!
    @code
        long i = 0, res = 0;
        fn::seq([&]{ return i < 9 ? i++ : fn::end_seq(); })
      % fn::transform([](long x) { return x + 1; })
      % fn::to_async(42) // the generator+transform will be offloaded to an async-task
                         // and the elements will be yielded via 42-capacity blocking queue.
                         // (If we wanted the generator and transform to be offloaded to
                         // separate threads, we could insert another to_async() before transform()).
      % fn::for_each([&](long x) {
              res = res * 10 + x;
        });
        assert(res == 123456789);
    @endcode
    */
    inline impl::async_wr to_async(size_t queue_size = 16)
    {
        return { queue_size };
    }

} // namespace fn
} // namespace rangeless





#if RANGELESS_MT_ENABLE_RUN_TESTS
#include <string>
#include <iostream>
#include <cctype>

#ifndef VERIFY
#define VERIFY(expr) if(!(expr)) RANGELESS_FN_THROW("Assertion failed: ( "#expr" ).");
#endif

namespace rangeless
{
namespace mt
{
namespace impl
{

static void run_tests()
{
    // test that queue works with non-default-constructible types
    {{ 
        int x = 10;
        mt::synchronized_queue<std::reference_wrapper<int> > queue;
        queue.push( std::ref(x));
        queue.push( std::ref(x));
        queue.close();

        auto y = queue.pop();
        queue >>= [&](int& x_) { x_ = 20; };
        assert(x == 20);
        assert(y == 20);
    }}

    {{
        synchronized_queue<std::string> q{1};
        std::string s1 = "1";
        std::string s2 = "2";

        auto st1 = q.try_push(std::move(s1), std::chrono::milliseconds(10));
        auto st2 = q.try_push(std::move(s2), std::chrono::milliseconds(10));

        assert(st1 == decltype(q)::status::success);
        assert(s1 == "");

        assert(st2 == decltype(q)::status::timeout);
        assert(s2 == "2");
    }}

    {{
        std::cerr << "Testing queue...\n";
        using queue_t = mt::synchronized_queue<long, mt::lockables::atomic_lock>;

        // test duration/timeout with try_pop
        {{
            queue_t q{ 10 };
            auto task = std::async(std::launch::async, [&] {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
                q.push(1);
                q.close();
            });
            long x = 0;
            
            auto res = q.try_pop(x, std::chrono::milliseconds(90));
            assert(res == queue_t::status::timeout);

            auto res2 = q.try_pop(x, std::chrono::milliseconds(20)); // 110 milliseconds passed
            assert(res2 == queue_t::status::success);
            assert(x == 1);

            assert(q.try_pop(x) == queue_t::status::closed);
            assert(q.try_push(42) == queue_t::status::closed);
        }}

        // test duration/timeout with try_push
        {{
            queue_t q{ 1 };
            q.push(1); // make full.

            auto task = std::async(std::launch::async, [&] {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
                q.pop();
            });

            auto res = q.try_push(42, std::chrono::milliseconds(90));
            assert(res == queue_t::status::timeout);

            auto res2 = q.try_push(42, std::chrono::milliseconds(20)); // 110 milliseconds passed
            assert(res2 == queue_t::status::success);
            assert(q.pop() == 42);
        }}


        {{ // test move, insert_iterator
            queue_t q1{ 10 };

            //*q1.inserter()++ = 123;
            q1.push(123);
            
            //auto q2 = std::move(q1);
            //assert(q1.empty());
            auto& q2 = q1;

            assert(q2.approx_size() == 1);
            assert(q2.pop() == 123);
        }}

        queue_t queue{ 1000 };

        mt::timer timer;

        std::vector<std::future<void>> pushers;
        std::vector<std::future<long>> poppers; 

        const size_t num_cpus = std::thread::hardware_concurrency();
        std::cerr << "hardware concurrency: " << num_cpus << "\n";
        const size_t num_jobs = num_cpus / 2;
        const int64_t num = 100000;

        // using 16 push-jobs and 16 pop-jobs:
        // In each pop-job accumulate partial sum.
        for(size_t i = 0; i < num_jobs; i++) {
            
            // Using blocking push/pop
            /////////////////////////////////////////////////////////////////

            pushers.emplace_back(
                std::async(std::launch::async, [&]
                {
                    for(long j = 0; j < num; j++) {
                        queue.push(1);
                    }
                }));

            poppers.emplace_back(
                std::async(std::launch::async, [&]
                {
                    //return std::accumulate(queue.begin(), queue.end(), 0L);
                    long acc = 0;
                    queue >>= [&acc](long x){ acc += x; };
                    return acc;
                }));
        }

        // wait for all push-jobs to finish, and 
        // close the queue, unblocking the poppers.
        for(auto& fut : pushers) {
            fut.wait();
        }

        std::cerr << "Closing queue...\n";
        queue.close(); // non-blocking; queue may still be non-empty
        std::cerr << "Size after close:" << queue.approx_size() << "\n";

        // pushing should now be prohibited,
        // even if the queue is not empty
        try {
            long x = 0;
            assert(queue.try_push(std::move(x)) == queue_t::status::closed);
            queue.push(std::move(x));
            assert(false);
        } catch(queue_t::queue_closed&) {}

        // collect subtotals accumulated from each pop-job.
        int64_t total = 0;
        std::cerr << "subtotals:";
        for(auto& fut : poppers) {
            const auto x = fut.get();
            std::cerr << " " << x;
            total += x;
        }
        std::cerr << "\n";


        assert(queue.approx_size() == 0);

#if 0
        auto queue2 = std::move(queue); // test move-semantics
        try {
            long x = 0;
            assert(queue2.try_pop(x) == queue_t::status::closed);
            queue2.pop();
            assert(false);
        } catch(queue_t::closed&) {}
#endif

        const auto n = int64_t(num_jobs) * num;

        assert(total == n);

        // of async-tasks (blocking and non-blocking versions)
        std::cerr << "Throughput: "  <<  double(total)/timer << "/s.\n";
    }}

#if 0
    // with 64 pushers and poppers on 32-CPU dev host:
    //                       CSyncQueue: 90k/s
    // synchronized_queue with mutex   : 1.5M/s
    // synchronized_queue with atomic_lock: 5.5M/s

    {{
        LOG("Testing CSyncQueue...");
        //CSyncQueue<long> queue{ 10000 };
        basic_synchronized_queue<long, mt::atomic_lock> queue{1000};

        const CTimeSpan timeout{ 2.9 }; // NB: must use explicit timeouts

        mt::timer timer;

        std::vector<std::future<void>> pushers;
        std::vector<std::future<long>> poppers; 

        const size_t num_jobs = std::thread::hardware_concurrency();
        const int64_t num = 100000;

        // using 16 push-jobs and 16 pop-jobs:
        // In each pop-job accumulate partial sum.
        for(size_t i = 0; i < num_jobs; i++) {
            
            // Using blocking push/pop
            /////////////////////////////////////////////////////////////////

            pushers.emplace_back(
                std::async(std::launch::async, [&]
                {
                    for(long j = 0; j < num; j++) {
                        //queue.Push(1, &timeout);
                        queue.push(1);
                    }
                }));

            poppers.emplace_back(
                std::async(std::launch::async, [&]
                {
                    long acc = 0;
                    for(long j = 0; j < num; j++) {
                        //acc += queue.Pop(&timeout);
                        acc += queue.pop();
                    }
                    return acc;
                }));
        }

        // wait for all push-jobs to finish, and 
        // close the queue, unblocking the poppers.
        for(auto& fut : pushers) {
            fut.wait();
        }

        int64_t total = 0;
        std::cerr << "subtotals:";
        for(auto& fut : poppers) {
            const auto x = fut.get();
            std::cerr << " " << x;
            total += x;
        }
        std::cerr << "\n";

        const auto n = (int64_t)num_jobs * num;
        assert(total == n);

        // of async-tasks (blocking and non-blocking versions)
        LOG("Throughput: "  <<  double(total)/timer << "/s.");
    }}
#endif


    {{
        // test timeout;
        using queue_t = synchronized_queue<int>;
        queue_t queue{ 1 };

        int x = 5;
        auto res = queue.try_pop(x, std::chrono::milliseconds(10));
        assert(res == queue_t::status::timeout);
        assert(x == 5);

        queue.push(10);
        res = queue.try_push(10, std::chrono::milliseconds(10));
        assert(res == queue_t::status::timeout);
    }}


    {{
        // test to_async
        long i = 0;
        long res = 0;
        namespace fn = rangeless::fn;
        using fn::operators::operator%;

        fn::seq([&]{ return i < 9 ? i++ : fn::end_seq(); })
      % fn::transform([](long x) { return x + 1; })
      % fn::to_async(16) // the generator+transform will be offloaded to an async-task
                         // and the elements will be yielded via 16-capacity blocking queue.
                         // (If we wanted the generator and transform to be offloaded to
                         // separate threads, we could insert another to_async() before transform()).
      % fn::for_each([&](long x) {
              res = res * 10 + x;
        });
        assert(res == 123456789);
    }}

} // run_tests()

} // namespace impl
} // namespace mt
} // namespace rangeless

#endif // RANGELESS_MT_ENABLE_RUN_TESTS

#endif // RANGELESS_MT_HPP_ 
