#include "ThreadPool.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

ThreadPool::ThreadPool(std::size_t threadCount)
{
    if (threadCount == 0) {
        throw std::invalid_argument("ThreadPool requires at least one worker thread");
    }

    workers_.reserve(threadCount);
    try {
        for (std::size_t index = 0; index < threadCount; ++index) {
            workers_.emplace_back([this]() {
                workerLoop();
            });
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }

        condition_.notify_all();

        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        throw;
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }

    condition_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::enqueue(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("ThreadPool is shutting down");
        }

        tasks_.push(std::move(task));
    }

    condition_.notify_one();
}

void ThreadPool::workerLoop()
{
    for (;;) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() {
                return stopping_ || !tasks_.empty();
            });

            if (stopping_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            task();
        } catch (const std::exception& exception) {
            std::cerr << "ThreadPool task failed: " << exception.what() << '\n';
        } catch (...) {
            std::cerr << "ThreadPool task failed with an unknown exception\n";
        }
    }
}
