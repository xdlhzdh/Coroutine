#include <iostream>
#include <queue>
#include <vector>
#include <map>
#include <condition_variable>
#include <coroutine>
#include <thread>
#include <chrono>
#include <memory>
#include <cstdint>
#include <functional>

using namespace std::chrono_literals;

struct Coroutine
{
    struct promise_type
    {
        using handle_type = std::coroutine_handle<promise_type>;
        Coroutine get_return_object()
        {
            return Coroutine(handle_type::from_promise(*this));
        }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept
        {
            completed_ = true;
            return {};
        }
        void return_void() {}
        void unhandled_exception() {} // std::current_exception()
        bool completed() const
        {
            return completed_;
        }
    private:
        volatile bool completed_ = false;
    };

    explicit Coroutine(promise_type::handle_type h) : h_(h) {}
    void operator()() const { h_.resume(); }
    bool completed() const { return h_.promise().completed(); }

private:
    promise_type::handle_type h_;
};

struct CoroutineHolder
{
public:
    static CoroutineHolder& getInstance()
    {
        static CoroutineHolder instance;
        return instance;
    }
    void push(uintptr_t addr, const Coroutine& coroutine)
    {
        std::lock_guard<std::mutex> lock(holder_mtx_);
        holder_[addr].push_back(coroutine);
    }
    void erase(uintptr_t addr)
    {
        std::lock_guard<std::mutex> lock(holder_mtx_);
        holder_.erase(addr);
    }
    void loop(std::stop_token st)
    {
        while (!st.stop_requested())
        {
            {
                std::lock_guard<std::mutex> lock(holder_mtx_);
                for (auto iter = holder_.begin(); iter != holder_.end();)
                {
                    std::erase_if(iter->second, [](const auto& coroutine) {
                       return coroutine.completed();
                    });
                    if (!iter->second.size())
                    {
                        iter = holder_.erase(iter);
                    }
                    else
                    {
                        ++iter;
                    }
                }
            }
            {
                std::unique_lock<std::mutex> lock(cv_mtx_);
                cv_.wait_for(lock, 1s, [&st] {
                    return st.stop_requested();
                });
            }
        }
    }
private:
    CoroutineHolder()
    {
        jt_ = std::jthread([this](std::stop_token st) {
            this->loop(st);
        });
    }
    CoroutineHolder(const CoroutineHolder&) = delete;
    CoroutineHolder& operator=(const CoroutineHolder&) = delete;

    std::jthread jt_;
    std::map<uintptr_t, std::vector<Coroutine>> holder_;
    std::mutex holder_mtx_;
    std::condition_variable cv_;
    std::mutex cv_mtx_;
};

struct CoroutineGuard
{
    CoroutineGuard(uintptr_t addr, const Coroutine& coroutine)
        : addr_(addr)
        , coroutine_(coroutine)
    {
    }
    ~CoroutineGuard()
    {
        if (!coroutine_.completed())
        {
            CoroutineHolder::getInstance().push(addr_, coroutine_);
        }
    }
private:
    uintptr_t addr_;
    Coroutine coroutine_;
};

template<class MessageType>
class MessageQueue {
public:
  void push(const MessageType& msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push(msg);
    cv_.notify_one();
  }

  MessageType pop() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return !queue_.empty(); });
    auto msg = queue_.front();
    queue_.pop();
    return msg;
  }

private:
  std::queue<MessageType> queue_;
  std::mutex mtx_;
  std::condition_variable cv_;
};

template<class MessageType>
auto await_message(MessageQueue<MessageType>& mq, std::jthread& jt) {
    struct Awaitable {
        Awaitable(MessageQueue<MessageType>& mq, std::jthread& jt) : mq_(mq), jt_(jt) {}
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept
        {
            jt_ = std::jthread([this, h](std::stop_token st)
            {
            msg_ = mq_.pop();
            if (!st.stop_requested() && !h.done())
            {
                h.resume();
            }
            });
        }
        MessageType await_resume() const noexcept
        {
            return msg_;
        }

        MessageQueue<MessageType>& mq_;
        std::jthread& jt_;
        MessageType msg_;
    };
    return Awaitable(mq, jt);
}

template<class MessageType>
class AwaitReciver
{
public:
    using MessageHandler = std::function<void(const MessageType& msg)>;

    AwaitReciver(MessageQueue<MessageType>& mq) : mq_(mq) {}
    ~AwaitReciver()
    {
        CoroutineHolder::getInstance().erase(reinterpret_cast<uintptr_t>(this));
    }
    void await_recevie(MessageHandler handler)
    {
        CoroutineGuard(reinterpret_cast<uintptr_t>(this), gen_coroutine(handler));
    }
    Coroutine gen_coroutine(MessageHandler handler)
    {
        auto msg = co_await await_message(mq_, jt_);
        handler(msg);
    }
private:
    MessageQueue<MessageType>& mq_;
    std::jthread jt_;
};

int main() {
  MessageQueue<std::string> mq;
  AwaitReciver<std::string> receiver(mq);
  receiver.await_recevie([](const std::string& str) {
      std::cout << "Recevied " << str << std::endl;
  });
  mq.push(std::string("Hello"));
  mq.push(std::string("World"));
  
  std::this_thread::sleep_for(2s);
  return 0;
}
