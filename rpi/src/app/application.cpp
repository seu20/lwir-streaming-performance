#include "app/application.hpp"

#include "common/logger.hpp"
#include "common/metrics_report.hpp"


Application::Application(
    const StreamingConfig& streaming_config,
    bool measurement_enabled)
    : streaming_(streaming_config),

      capture_thread_(
          capture_,
          capture_queue_,
          measurement_enabled),

      preprocess_thread_(
          preprocess_,
          capture_queue_,
          streaming_queue_,
          measurement_enabled),

      streaming_thread_(
          streaming_,
          streaming_queue_,
          measurement_enabled),

      measurement_enabled_(measurement_enabled)
{
}


bool Application::run()
{
    Logger::info("[Application] 시작");

    streaming_thread_.start();
    preprocess_thread_.start();
    capture_thread_.start();


    capture_thread_.join();
    preprocess_thread_.join();
    streaming_thread_.join();


    if (measurement_enabled_)
    {
        const auto& capture_metrics =
            capture_thread_.metrics();

        const auto& preprocess_metrics =
            preprocess_thread_.metrics();

        const auto& streaming_metrics =
            streaming_thread_.metrics();


        // Console summary
        print_stage_summary(
            "Capture",
            capture_metrics
        );

        print_stage_summary(
            "Preprocess",
            preprocess_metrics
        );

        print_stage_summary(
            "Streaming Submit",
            streaming_metrics
        );


        // Raw measurement CSV
        save_stage_metrics_csv(
            "capture_metrics.csv",
            capture_metrics
        );

        save_stage_metrics_csv(
            "preprocess_metrics.csv",
            preprocess_metrics
        );

        save_stage_metrics_csv(
            "streaming_metrics.csv",
            streaming_metrics
        );
    }


    Logger::info("[Application] 종료");

    return true;
}