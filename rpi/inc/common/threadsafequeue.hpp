#pragma once

#include <pthread.h>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <utility>

enum class QueuePolicy
{
    Fifo,
    Latest
};

struct QueueConfig
{
    // 0 means unbounded. Latest requires a positive maximum size.
    std::size_t max_size = 0;
    QueuePolicy policy = QueuePolicy::Fifo;
};

// 여러 pthread 사이에서 데이터를 안전하게 전달하기 위한 공통 Queue.
template <typename T>
class ThreadSafeQueue
{
private:
    std::queue<T> queue_;

    // 여러 Thread의 Queue 동시 접근을 방지
    pthread_mutex_t mutex_;

    // Queue가 비어 있을 때 Consumer를 대기시키고,
    // 데이터가 들어오거나 종료될 때 깨우기 위해 사용
    pthread_cond_t cond_;

    // Bounded FIFO producers wait here until a consumer frees a slot.
    pthread_cond_t not_full_cond_;

    // Queue 종료 상태
    bool closed_;
    QueueConfig config_;
    std::uint64_t dropped_count_ = 0;
    
public:
    // Mutex, Condition Variable 초기화
    explicit ThreadSafeQueue(QueueConfig config = {})
        : closed_(false),
          config_(config)
    {
        pthread_mutex_init(&mutex_, nullptr);
        pthread_cond_init(&cond_, nullptr);
        pthread_cond_init(&not_full_cond_, nullptr);
    }

    // Mutex, Condition Variable 자원 해제
    ~ThreadSafeQueue()
    {
        pthread_cond_destroy(&not_full_cond_);
        pthread_cond_destroy(&cond_);
        pthread_mutex_destroy(&mutex_);
    }

    // 동기화 객체를 포함하므로 복사 금지
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    // 데이터를 Queue에 삽입하고 대기 중인 Consumer 하나를 깨운다.
    // Queue가 이미 닫힌 경우 false를 반환한다.
    bool push(T data)
    {
        pthread_mutex_lock(&mutex_);

        if (config_.policy == QueuePolicy::Latest)
        {
            if (closed_ || config_.max_size == 0)
            {
                pthread_mutex_unlock(&mutex_);
                return false;
            }

            while (queue_.size() >= config_.max_size)
            {
                queue_.pop();
                ++dropped_count_;
            }

            queue_.push(std::move(data));
            pthread_mutex_unlock(&mutex_);
            pthread_cond_signal(&cond_);
            return true;
        }

        while (!closed_ && config_.max_size > 0 &&
               queue_.size() >= config_.max_size)
        {
            pthread_cond_wait(&not_full_cond_, &mutex_);
        }

        if (closed_)
        {
            pthread_mutex_unlock(&mutex_);
            return false;
        }

        queue_.push(std::move(data));

        pthread_mutex_unlock(&mutex_);

        pthread_cond_signal(&cond_);
        return true;
    }

    // 최신 상태가 중요한 GUI 경로에서 사용한다.
    // max_size를 넘으면 가장 오래된 항목을 제거하고 새 항목을 넣는다.
    bool push_latest(T data, std::size_t max_size)
    {
        pthread_mutex_lock(&mutex_);

        if (closed_ || max_size == 0)
        {
            pthread_mutex_unlock(&mutex_);
            return false;
        }

        while (queue_.size() >= max_size)
            queue_.pop();

        queue_.push(std::move(data));

        pthread_mutex_unlock(&mutex_);

        pthread_cond_signal(&cond_);
        return true;
    }

    // Queue에서 데이터를 하나 꺼낸다.
    // Queue가 비어 있으면 데이터가 들어오거나 close()될 때까지 대기한다.
    // true  : 정상적으로 데이터를 꺼냄
    // false : Queue가 닫혔고 남은 데이터도 없음
    bool pop(T& data)
    {
        pthread_mutex_lock(&mutex_);

        while (queue_.empty() && !closed_)
        {
            pthread_cond_wait(&cond_, &mutex_);
        }

        if (queue_.empty() && closed_)
        {
            pthread_mutex_unlock(&mutex_);
            return false;
        }

        data = std::move(queue_.front());
        queue_.pop();

        pthread_mutex_unlock(&mutex_);
        pthread_cond_signal(&not_full_cond_);
        return true;
    }

    // Queue를 종료 상태로 만들고,
    // pop()에서 대기 중인 모든 Thread를 깨운다.
    void close()
    {
        pthread_mutex_lock(&mutex_);

        closed_ = true;

        pthread_mutex_unlock(&mutex_);

        pthread_cond_broadcast(&cond_);
        pthread_cond_broadcast(&not_full_cond_);
    }

    // 현재 Queue가 비어 있는지 확인한다.
    bool empty()
    {
        pthread_mutex_lock(&mutex_);

        bool result = queue_.empty();

        pthread_mutex_unlock(&mutex_);
        return result;
    }

    // 현재 Queue에 저장된 데이터 개수를 반환한다.
    std::size_t size()
    {
        pthread_mutex_lock(&mutex_);

        std::size_t result = queue_.size();

        pthread_mutex_unlock(&mutex_);
        return result;
    }

    std::uint64_t dropped_count()
    {
        pthread_mutex_lock(&mutex_);
        const std::uint64_t result = dropped_count_;
        pthread_mutex_unlock(&mutex_);
        return result;
    }

};
