#include "common/logger.hpp"

#include <iostream>
#include <stdexcept>

// 기본 로그 레벨은 INFO
LogLevel Logger::current_level_ = LogLevel::INFO;

// 여러 Thread의 로그 출력 동기화
pthread_mutex_t Logger::mutex_ = PTHREAD_MUTEX_INITIALIZER;


// 현재 로그 레벨을 설정한다.
void Logger::set_level(LogLevel level)
{
    current_level_ = level;
}


// 문자열을 LogLevel로 변환하여 설정한다.
void Logger::set_level(const std::string& level)
{
    if (level == "error")
    {
        current_level_ = LogLevel::ERROR;
    }
    else if (level == "warn")
    {
        current_level_ = LogLevel::WARN;
    }
    else if (level == "info")
    {
        current_level_ = LogLevel::INFO;
    }
    else if (level == "debug")
    {
        current_level_ = LogLevel::DEBUG;
    }
    else
    {
        throw std::runtime_error(
            "지원하지 않는 로그 레벨: " + level
        );
    }
}


// 현재 설정된 로그 레벨에 따라 로그를 출력한다.
void Logger::log(
    LogLevel level,
    const std::string& message)
{
    // 현재 설정보다 상세한 로그는 출력하지 않음
    if (static_cast<int>(level) >
        static_cast<int>(current_level_))
    {
        return;
    }

    std::string prefix;

    switch (level)
    {
        case LogLevel::ERROR:
            prefix = "[ERROR] ";
            break;

        case LogLevel::WARN:
            prefix = "[WARN] ";
            break;

        case LogLevel::INFO:
            prefix = "[INFO] ";
            break;

        case LogLevel::DEBUG:
            prefix = "[DEBUG] ";
            break;
    }

    pthread_mutex_lock(&mutex_);

    std::cout << prefix << message << '\n';

    pthread_mutex_unlock(&mutex_);
}


// ERROR 로그를 출력한다.
void Logger::error(const std::string& message)
{
    log(LogLevel::ERROR, message);
}


// WARN 로그를 출력한다.
void Logger::warn(const std::string& message)
{
    log(LogLevel::WARN, message);
}


// INFO 로그를 출력한다.
void Logger::info(const std::string& message)
{
    log(LogLevel::INFO, message);
}


// DEBUG 로그를 출력한다.
void Logger::debug(const std::string& message)
{
    log(LogLevel::DEBUG, message);
}
