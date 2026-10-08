#pragma once

#include <pthread.h>
#include <string>

// 출력할 로그의 중요도
enum class LogLevel
{
    ERROR = 0,
    WARN,
    INFO,
    DEBUG
};


// 프로젝트 전체에서 사용하는 공통 Logger
class Logger
{
private:
    // 현재 출력할 최대 로그 레벨
    static LogLevel current_level_;

    // 여러 Thread가 동시에 출력하는 것을 방지
    static pthread_mutex_t mutex_;

    // 실제 로그 출력 함수
    static void log(
        LogLevel level,
        const std::string& message
    );

public:
    // 현재 로그 레벨 설정
    static void set_level(LogLevel level);

    // 문자열로 로그 레벨 설정
    static void set_level(const std::string& level);

    // ERROR 로그 출력
    static void error(const std::string& message);

    // WARN 로그 출력
    static void warn(const std::string& message);

    // INFO 로그 출력
    static void info(const std::string& message);

    // DEBUG 로그 출력
    static void debug(const std::string& message);
};