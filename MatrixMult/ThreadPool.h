#pragma once
#include <functional>
#include <thread>
#include <queue>
#include <mutex>
#include <tuple>
#include <vector>
#include <Windows.h>
#include <iostream>
#include <cmath>
#include <future>
#include <array>
#include <cassert>

/*
* Thread pool that respects cache locality on HyperThreaded CPUs (WIN32 API dependent)
*
* Each job is described as an array of N functions.
* For each job, N threads are created and assigned respective functions.
* For a given job, all threads are guaranteed to be on the same physical core.
* No two threads from different jobs are allowed on the same physical core.
*
* Currently this piece of code is not or claim to be:
*     * As optimized as it can be
*     * As conformative to modern C++ as it can be
*
* Reference: This code is influenced by writeup that explains thread pools at
* https://github.com/mtrebi/thread-pool/blob/master/README.md
* However, this is NOT a copy and has drastically different aims and implementation.
*
* Structure:
*
* Submission:
*     array of (void function (void)) of length N
*         where N is the num of threads that will spawn on the same core,
*         and, the length of the function ptr array. ith thread handles the ith function
*
* Core Handlers:
*     We create NumHWCores many CoreHandler objects.
*     These objects are responsible for managing their cores.
*     They check the main pool for jobs, when a job is found,
*         if N==1   ,   they call the only function in the job description.
*         if N>1    ,   they spawn N-1 more threads on the same physical core,
*                       each one is assigned i+1 th function in the arr.
*                       The CoreHandler is assigned to the first function.
*     Once CoreHandler finishes its own task, it waits for other threads,
*     Then its available for new jobs, waiting to be notified by the pool manager.
*
*/

namespace QueryHWCores {
    static char cache = 0;
    static unsigned numHWCores;
    static ULONG_PTR* map = NULL;

    const char* BitmaskToStr(WORD bitmask) {
        const unsigned N = sizeof(WORD) * 8;
        char* const str = new char[N + 1];
        str[N] = 0;
        for (int i = 0; i < N; ++i) {
            str[N - i - 1] = '0' + ((bitmask) & 1);
            bitmask >>= 1;
        }
        return str;
    }

    void PrintSysLPInfoArr(_SYSTEM_LOGICAL_PROCESSOR_INFORMATION* const sysLPInf, const DWORD& retLen) {
        unsigned numPhysicalCores = 0;
        for (int i = 0; i * sizeof(_SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= retLen; ++i) {
            if (sysLPInf[i].Relationship != RelationProcessorCore)
                continue;

            printf("PHYSICAL CPU[%d]\n\t_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX:\n", numPhysicalCores);
            printf("\t\tProcessorMask:%s\n", BitmaskToStr(sysLPInf[i].ProcessorMask));
            printf("\t\tRelationship:%u | RelationProcessorCore\n", (uint8_t)sysLPInf[i].Relationship);
            printf("\t\tProcessorCore:\n");
            printf("\t\t\tFlags(HT?):%d\n",
                (uint8_t)sysLPInf[i].ProcessorCore.Flags);
            ++numPhysicalCores;
        }
    }

    int TestCPUCores() {

        const unsigned N = 30;
        _SYSTEM_LOGICAL_PROCESSOR_INFORMATION sysLPInf[N];
        DWORD retLen = N * sizeof(_SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        LOGICAL_PROCESSOR_RELATIONSHIP lpRel = RelationProcessorCore;

        BOOL retCode = GetLogicalProcessorInformation(&sysLPInf[0], &retLen);

        if (!retCode) {
            DWORD errCode = GetLastError();
            printf("ERR: %d\n", errCode);
            if (errCode == ERROR_INSUFFICIENT_BUFFER) {
                printf("Buffer is not large enough! Buffer length required: %d\n", retLen);
            }
            else {
                printf("CHECK MSDN SYSTEM ERROR CODES LIST.\n");
            }
            return errCode;
        }

        PrintSysLPInfoArr(sysLPInf, retLen);

        return 0;
    }

    DWORD _GetSysLPMap(unsigned& numHWCores) {
        // These assumptions should never fail on desktop
        const unsigned N = 48, M = 48;

        _SYSTEM_LOGICAL_PROCESSOR_INFORMATION sysLPInf[N];
        DWORD retLen = N * sizeof(_SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        LOGICAL_PROCESSOR_RELATIONSHIP lpRel = RelationProcessorCore;

        static BOOL retCode = GetLogicalProcessorInformation(&sysLPInf[0], &retLen);

        if (!retCode) {
            return GetLastError();
        }

        ULONG_PTR* const lMap = (ULONG_PTR*)malloc(M * sizeof(ULONG_PTR));

        numHWCores = 0;
        for (int i = 0; i * sizeof(_SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= retLen; ++i) {
            if (sysLPInf[i].Relationship != RelationProcessorCore)
                continue;

            lMap[numHWCores++] = sysLPInf[i].ProcessorMask;
        }

        map = (ULONG_PTR*)malloc(numHWCores * sizeof(ULONG_PTR));
        memcpy(map, lMap, numHWCores * sizeof(ULONG_PTR));

        return 0;
    }

    // Get the logical processor mask corresponding to the Nth hardware core
    int GetProcessorMask(unsigned n, ULONG_PTR& mask) {
        if (!cache) {
            DWORD retCode = _GetSysLPMap(numHWCores);
            if (!retCode)
                cache = 1;
            else
                return retCode;
        }

        if (n >= numHWCores)
            return -1;

        mask = map[n];

        return 0;
    }

    int GetNumHWCores() {
        if (!cache) {
            DWORD retCode = _GetSysLPMap(numHWCores);
            if (!retCode)
                cache = 1;
            else
                return -1;
        }
        return numHWCores;
    }

    // unsafe impl. returns direct pointer to the map
    ULONG_PTR* GetProcessorMaskMap_UNSAFE() {
        if (!cache) {
            DWORD retCode = _GetSysLPMap(numHWCores);
            if (!retCode)
                cache = 1;
            else
                return NULL;
        }

        return map;
    }
};

template <int NumOfCoresToUse = -1, int NumThreadsPerCore = 2>
class HWLocalThreadPool
{
public:
    HWLocalThreadPool() :
        m_terminate(false)
    {
        m_numHWCores = QueryHWCores::GetNumHWCores();

        if (NumOfCoresToUse <= 0)
            m_numCoreHandlers = m_numHWCores;
        else
            m_numCoreHandlers = NumOfCoresToUse;

        m_coreHandlers = (CoreHandler*)malloc(m_numCoreHandlers * sizeof(CoreHandler));
        m_coreHandlerThreads = new std::thread[m_numCoreHandlers];

        for (int i = 0; i < m_numCoreHandlers; ++i) {
            ULONG_PTR processAffinityMask;
            int maskQueryRetCode = QueryHWCores::GetProcessorMask(i, processAffinityMask);
            if (maskQueryRetCode) {
                assert(0, "Can't query processor relations.");
                return;
            }
            CoreHandler* coreHandler = new(&m_coreHandlers[i]) CoreHandler(this, i, processAffinityMask);
            m_coreHandlerThreads[i] = std::thread(std::ref(m_coreHandlers[i]));
        }
    }

    ~HWLocalThreadPool() {
        if (!m_terminate)
            Close();
    }

    void Add(std::function<void()>* F) {
        m_queue.Push(F);
        m_queueToCoreNotifier.notify_one();
    }

    /* if finishQueue is set, cores will termianate after handling every job at the queue
    if not, they will finish the current job they have and terminate. */
    void Close(const bool finishQueue = true) {
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

        free(m_coreHandlers);
        delete[] m_coreHandlerThreads;
    }

    const unsigned NumCores() {
        return m_numHWCores;
    }

    template<typename F, typename...Args>
    static std::function<void()> WrapFunc(F&& f, Args&&... args) {
        std::function<decltype(f(args...))()> func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        auto task_ptr = std::make_shared<std::packaged_task<decltype(f(args...))()>>(func);

        std::function<void()> wrapper_func = [task_ptr]() {
            (*task_ptr)();
        };

        return wrapper_func;
    }


protected:
    template <typename T>
    class Queue {
    public:
        Queue() {}
        ~Queue() {}

        void Push(T const& element) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push(element);
        }

        bool Pop(T& function) {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_queue.empty()) {
                function = m_queue.front();
                m_queue.pop();
                return true;
            }
            return false;
        }

        int Size() {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_queue.size();
        }

    private:
        std::queue<T> m_queue;
        std::mutex m_mutex;
    };

    class CoreHandler {
    public:
        CoreHandler(HWLocalThreadPool* const _parent, const unsigned _id, const ULONG_PTR& _processorMask) :
            m_parent(_parent), m_id(_id), m_processorAffinityMask(_processorMask), m_terminate(false), m_numChildThreads(NumThreadsPerCore - 1)
        {
            m_job = new std::function<void()>[NumThreadsPerCore];

            if (m_numChildThreads > 0) {
                m_childThreads = new std::thread[m_numChildThreads];
                m_childThreadOnline = new bool[m_numChildThreads];
                std::unique_lock<std::mutex> lock(m_threadMutex);
                for (int i = 0; i < m_numChildThreads; ++i) {
                    m_childThreadOnline[i] = 0;
                    m_childThreads[i] = std::thread(ThreadHandler(this, i, m_processorAffinityMask));
                }
            }
        }

        void WaitForChildThreads() {
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

        void CloseChildThreads() {
            if (m_numChildThreads < 1)
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
        }

        void operator()() {
            SetThreadAffinityMask(GetCurrentThread(), m_processorAffinityMask);
            bool dequeued;
            while (1) {
                {
                    std::unique_lock<std::mutex> lock(m_parent->m_queueMutex);
                    if (m_parent->m_terminate 
                        && !(m_parent->m_waitToFinish && m_parent->m_queue.Size() > 0)) 
                    {
                        break;
                    }
                    if (m_parent->m_queue.Size() == 0) {
                        m_parent->m_queueToCoreNotifier.wait(lock);
                        //std::cout << "Core " << m_id << ", is fetching a job\n";
                    }
                    dequeued = m_parent->m_queue.Pop(m_job);
                }
                if (dequeued) {
                    if (m_numChildThreads < 1) {
                        //std::cout << "Core " << m_id << ", started executing\n";
                        m_job[0]();
                    }
                    else {
                        {
                            std::unique_lock<std::mutex> lock(m_threadMutex);
                            for (int i = 0; i < m_numChildThreads; ++i) {
                                m_childThreadOnline[i] = 1;
                            }
                            m_coreToThreadNotifier.notify_all();
                        }

                        //std::cout << "Run on the CoreHandler\n";
                        m_job[0]();

                        WaitForChildThreads();
                    }
                }
            }
            //std::cout << "Will close the core " << m_id << ", closing threads\n";
            CloseChildThreads();
            //std::cout << "Child threads are terminated\n";
        }

        class ThreadHandler {
        public:
            ThreadHandler(CoreHandler* _parent, const unsigned _id, const ULONG_PTR& _processorAffinityMask) :
                m_parent(_parent), m_processorAffinityMask(_processorAffinityMask), m_id(_id), m_jobSlot(_id + 1) {}

            void operator()() {
                SetThreadAffinityMask(GetCurrentThread(), m_processorAffinityMask);
                while (1) {
                    {
                        //std::cout << "Thread checking for jobs!!\n";
                        std::unique_lock<std::mutex> lock(m_parent->m_threadMutex);
                        if (m_parent->m_terminate)
                            break;
                        if (!m_parent->m_childThreadOnline[m_id]) {
                            m_parent->m_coreToThreadNotifier.wait(lock);
                        }
                    }
                    func = m_parent->m_job[m_jobSlot];
                    bool online = 0;
                    {
                        online = m_parent->m_childThreadOnline[m_id];
                    }
                    if (online) {
                        func();
                        std::unique_lock<std::mutex> lock(m_parent->m_threadMutex);
                        m_parent->m_childThreadOnline[m_id] = 0;
                        m_parent->m_threadToCoreNotifier.notify_one();
                    }
                }
                //std::cout << "Exiting the thread!\n";
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
        //std::array<std::atomic<int>, NumChildThreads> m_childThreadOnline;
        bool m_terminate;

        std::function<void()>* m_job;

        //std::mutex m_coreMutex;
        std::mutex m_threadMutex;
        std::condition_variable m_coreToThreadNotifier;
        std::condition_variable m_threadToCoreNotifier;
    };

    unsigned m_numHWCores, m_numCoreHandlers;
    CoreHandler* m_coreHandlers;
    std::thread* m_coreHandlerThreads;

    Queue<std::function<void()>*> m_queue;


    bool m_terminate, m_waitToFinish;

    std::mutex m_queueMutex;
    std::condition_variable m_queueToCoreNotifier;
};

