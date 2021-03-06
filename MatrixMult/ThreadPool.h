#pragma once
#include <functional>
#include <thread>
#include <queue>
#include <mutex>
#include <tuple>
#include <vector>
#include <iostream>
#include <cmath>
#include <future>
#include <array>
#include <cassert>
#include "CPUUtil.h"

/*
 * Thread pool that respects cache locality on HyperThreaded CPUs (WIN32 API dependent)
 *
 * Each job is described as an array of N functions. (ideal N=2 for HT)
 * For each job, N threads are created and assigned respective functions.
 * For a given job, all threads are guaranteed to be on the same physical core.
 * No two threads from different jobs are allowed on the same physical core.
 *
 *
 * Why?
 *   When doing multithreading on cache sensitive tasks, 
 *   we want to keep threads that operate on same or contiguous memory region 
 *     on the same physical core s.t they share the same L2 cache.
 *
 * Reference: This code is influenced by writeup that explains thread pools at
 * https://github.com/mtrebi/thread-pool/blob/master/README.md
 *
 * Structure:
 *   CPUUtil:
 *     Uses Windows API to detect the number of physical cores, cache sizes 
 *       and mapping between physical and logical processors.
 *
 *   HWLocalThreadPool:
 *     Submission:
 *       initializer list or vector of (void function (void)) of length N
 *         where N is the num of threads that will spawn on the same core,
 *         and, the length of the std::function array. 
 *         ith thread handles repective ith function
 *     
 *     Core Handlers:
 *       We create NumHWCores many CoreHandler objects.
 *       These objects are responsible for managing their cores.
 *       They check the main pool for jobs, when a job is found,
 *           if N==1   ,   they call the only function in the job description.
 *           if N>1    ,   they assign N-1 threads on the same physical core to,
 *                         respective functions in the array. The CoreHandler is 
 *                         assigned to the first function.
 *       Once CoreHandler finishes its own task, it waits for other threads,
 *       Then its available for new jobs, waiting to be notified by the pool manager.
 *     
 *     Thread Handlers:
 *       Responsible for handling tasks handed away by the CoreHandler.
 *       When they finish execution, they signal to notify CoreHandler 
 *       Then, they wait for a new task to run until they are terminated.
 * 
 * Notes:
 * 
 *   DON'T KEEP THESE TASKS TOO SMALL. 
 *   We don't want our CoreHandler to check its childrens states constantly,
 *   So, when a thread finishes a task, we signal the CoreHandler.
 *   This might become a overhead if the task itself is trivial.
 *   In that case you probably shouldn't be using this structure anyways,
 *   But if you want to, you can change it so that,
 *   CoreHandler periodically checks m_childThreadOnline array and sleeps in between.
 *
 */

class HWLocalThreadPool {
public:
    HWLocalThreadPool(int _numOfCoresToUse, int _numThreadsPerCore) : m_terminate(false)
    {
        m_numHWCores = CPUUtil::GetNumHWCores();

        if (_numOfCoresToUse <= 0) {
            m_numCoreHandlers = m_numHWCores;
        } else {
            m_numCoreHandlers = _numOfCoresToUse;
        }

        if (_numThreadsPerCore <= 0) {
            m_numThreadsPerCore =
              CPUUtil::GetNumLogicalProcessors() / m_numCoreHandlers;
        } else {
            m_numThreadsPerCore = _numThreadsPerCore;
        }

        /* malloc m_coreHandlers s.t no default initialization takes place, 
        we construct every object with placement new */
        m_coreHandlers = (CoreHandler*)malloc(m_numCoreHandlers * sizeof(CoreHandler));
        m_coreHandlerThreads = new std::thread[m_numCoreHandlers];

        for (int i = 0; i < m_numCoreHandlers; ++i) {
            ULONG_PTR processAffinityMask;
            int maskQueryRetCode = CPUUtil::GetProcessorMask(i, processAffinityMask);
            if (maskQueryRetCode) {
                assert(0, "Can't query processor relations.");
                return;
            }
            CoreHandler* coreHandler =
              new (&m_coreHandlers[i]) CoreHandler(this, i, processAffinityMask);
            m_coreHandlerThreads[i] = std::thread(std::ref(m_coreHandlers[i]));
        }
    }

    ~HWLocalThreadPool()
    {
        if (!m_terminate)
            Close();
    }

    void Add(std::vector<std::function<void()>> const& F)
    {
        m_queue.Push(F);
        m_queueToCoreNotifier.notify_one();
    }

    /* if finishQueue is set, cores will termianate after handling every job at the queue
    if not, they will finish the current job they have and terminate. */
    void Close(const bool finishQueue = true)
    {
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_terminate = 1;
            m_waitToFinish = finishQueue;
            m_queueToCoreNotifier.notify_all();
        }

        for (int i = 0; i < m_numCoreHandlers; ++i) {
            if (m_coreHandlerThreads[i].joinable())
                m_coreHandlerThreads[i].join();
        }

        /* free doesn't call the destructor, so  */
        for (int i = 0; i < m_numCoreHandlers; ++i) {
            m_coreHandlers[i].~CoreHandler();
        }
        free(m_coreHandlers);
        delete[] m_coreHandlerThreads;
    }

    const unsigned NumCores()
    {
        return m_numHWCores;
    }

    const unsigned NumThreadsPerCore()
    {
        return m_numThreadsPerCore;
    }

    template <typename F, typename... Args>
    static std::function<void()> WrapFunc(F&& f, Args&&... args)
    {
        std::function<decltype(f(args...))()> func =
          std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        auto task_ptr =
          std::make_shared<std::packaged_task<decltype(f(args...))()>>(func);

        std::function<void()> wrapper_func = [task_ptr]() { (*task_ptr)(); };

        return wrapper_func;
    }

protected:
    template <typename T> class Queue {
    public:
        Queue()
        {
        }
        ~Queue()
        {
        }

        void Push(T const& element)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push(std::move(element));
        }

        bool Pop(T& function)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_queue.empty()) {
                function = std::move(m_queue.front());
                m_queue.pop();
                return true;
            }
            return false;
        }

        int Size()
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_queue.size();
        }

    private:
        std::queue<T> m_queue;
        std::mutex m_mutex;
    };

    class CoreHandler {
    public:
        CoreHandler(HWLocalThreadPool* const _parent, const unsigned _id,
                    const ULONG_PTR& _processorMask)
            : m_parent(_parent), m_id(_id), m_processorAffinityMask(_processorMask),
              m_terminate(false), m_numChildThreads(_parent->m_numThreadsPerCore - 1)
        {
            if (m_numChildThreads > 0) {
                m_childThreads = new std::thread[m_numChildThreads];
                m_childThreadOnline = new bool[m_numChildThreads];
                std::unique_lock<std::mutex> lock(m_threadMutex);
                for (int i = 0; i < m_numChildThreads; ++i) {
                    m_childThreadOnline[i] = 0;
                    m_childThreads[i] =
                      std::thread(ThreadHandler(this, i, m_processorAffinityMask));
                }
            }
        }

        void WaitForChildThreads()
        {
            if (!m_childThreads || m_numChildThreads < 1)
                return;

            std::unique_lock<std::mutex> lock(m_threadMutex);
            bool anyOnline = 1;
            while (anyOnline) {
                anyOnline = 0;
                for (int i = 0; i < m_numChildThreads; ++i) {
                    anyOnline |= m_childThreadOnline[i];
                }
                if (anyOnline) {
                    m_threadToCoreNotifier.wait(lock);
                }
            }
        }

        void CloseChildThreads()
        {
            if (m_terminate || m_numChildThreads < 1)
                return;

            {
                std::unique_lock<std::mutex> lock(m_threadMutex);
                m_terminate = 1;
                m_coreToThreadNotifier.notify_all();
            }

            /* Core closing threads */
            for (int i = 0; i < m_numChildThreads; ++i) {
                if (m_childThreads[i].joinable()) {
                    m_childThreads[i].join();
                }
            }

            delete[] m_childThreads;
            delete[] m_childThreadOnline;
        }

        void operator()()
        {
            SetThreadAffinityMask(GetCurrentThread(), m_processorAffinityMask);
            bool dequeued;
            while (1) {
                {
                    std::unique_lock<std::mutex> lock(m_parent->m_queueMutex);
                    if (m_parent->m_terminate &&
                        !(m_parent->m_waitToFinish && m_parent->m_queue.Size() > 0)) {
                        break;
                    }
                    if (m_parent->m_queue.Size() == 0) {
                        m_parent->m_queueToCoreNotifier.wait(lock);
                    }
                    dequeued = m_parent->m_queue.Pop(m_job);
                }
                if (dequeued) {
                    m_ownJob = std::move(m_job[0]);
                    if (m_numChildThreads < 1) {
                        m_ownJob();
                    } else {
                        {
                            std::unique_lock<std::mutex> lock(m_threadMutex);
                            for (int i = 0; i < m_numChildThreads; ++i) {
                                m_childThreadOnline[i] = 1;
                            }
                            m_coreToThreadNotifier.notify_all();
                        }

                        m_ownJob();

                        WaitForChildThreads();
                    }
                }
            }
            CloseChildThreads();
        }

        class ThreadHandler {
        public:
            ThreadHandler(CoreHandler* _parent, const unsigned _id,
                          const ULONG_PTR& _processorAffinityMask)
                : m_parent(_parent), m_processorAffinityMask(_processorAffinityMask),
                  m_id(_id), m_jobSlot(_id + 1)
            {
            }

            void operator()()
            {
                SetThreadAffinityMask(GetCurrentThread(), m_processorAffinityMask);
                while (1) {
                    {
                        std::unique_lock<std::mutex> lock(m_parent->m_threadMutex);
                        if (m_parent->m_terminate)
                            break;
                        if (!m_parent->m_childThreadOnline[m_id]) {
                            m_parent->m_coreToThreadNotifier.wait(lock);
                        }
                    }
                    bool online = 0;
                    {
                        std::unique_lock<std::mutex> lock(m_parent->m_threadMutex);
                        online = m_parent->m_childThreadOnline[m_id];
                    }
                    if (online) {
                        func = std::move(m_parent->m_job[m_jobSlot]);
                        func();
                        std::unique_lock<std::mutex> lock(m_parent->m_threadMutex);
                        m_parent->m_childThreadOnline[m_id] = 0;
                        m_parent->m_threadToCoreNotifier.notify_one();
                    }
                }
            }

            const unsigned m_id;
            const unsigned m_jobSlot;
            CoreHandler* m_parent;
            ULONG_PTR m_processorAffinityMask;
            std::function<void()> func;
        };

        const unsigned m_id;
        HWLocalThreadPool* const m_parent;
        const ULONG_PTR m_processorAffinityMask;
        const unsigned m_numChildThreads;

        std::thread* m_childThreads;
        bool* m_childThreadOnline;
        bool m_terminate;

        std::vector<std::function<void()>> m_job;
        std::function<void()> m_ownJob;

        std::mutex m_threadMutex;
        std::condition_variable m_coreToThreadNotifier;
        std::condition_variable m_threadToCoreNotifier;
    };

private:
    unsigned m_numHWCores, m_numCoreHandlers, m_numThreadsPerCore;
    CoreHandler* m_coreHandlers;
    std::thread* m_coreHandlerThreads;

    Queue<std::vector<std::function<void()>>> m_queue;

    bool m_terminate, m_waitToFinish;

    std::mutex m_queueMutex;
    std::condition_variable m_queueToCoreNotifier;
};
