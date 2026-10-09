#include "app/application.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <system_error>
#include <utility>

#include "common/logger.hpp"
#include "common/metrics_report.hpp"


Application::Application(
    const CaptureConfig& capture_config,
    const StreamingConfig& streaming_config,
    bool measurement_enabled,
    QueueConfig queue_config,
    std::string metrics_output_dir)
    : capture_(
          capture_config,
          streaming_config.width,
          streaming_config.height),

      streaming_(
          streaming_config,
          measurement_enabled),

      capture_queue_(queue_config),

      streaming_queue_(queue_config),

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

      measurement_enabled_(measurement_enabled),
      metrics_output_dir_(std::move(metrics_output_dir))
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
        std::filesystem::path output_dir =
            metrics_output_dir_.empty()
                ? std::filesystem::current_path()
                : std::filesystem::path(metrics_output_dir_);
        std::error_code directory_error;
        std::filesystem::create_directories(
            output_dir,
            directory_error
        );
        if (directory_error)
        {
            Logger::error(
                "[Metrics] 출력 디렉터리 생성 실패: " +
                output_dir.string() + " (" +
                directory_error.message() + ")"
            );
            return false;
        }

        const auto metric_path =
            [&output_dir](const char* filename)
            {
                return (output_dir / filename).string();
            };

        const auto& capture_metrics =
            capture_thread_.metrics();

        const auto& preprocess_metrics =
            preprocess_thread_.metrics();

        const auto& streaming_metrics =
            streaming_thread_.metrics();

        const auto encoding_metrics =
            streaming_.encoding_metrics();

        const auto network_metrics =
            streaming_.network_metrics();

        FrameMetricSummary frame_metrics;
        frame_metrics.capture_success_count =
            capture_thread_.success_count();
        frame_metrics.preprocess_success_count =
            preprocess_thread_.success_count();
        frame_metrics.streaming_submit_success_count =
            streaming_thread_.success_count();

        // 현재 두 Queue 모두 일반 FIFO push()만 사용하므로
        // Queue 정책에 의해 명시적으로 제거된 Frame은 없다.
        frame_metrics.queue_policy_drop_count =
            capture_queue_.dropped_count() +
            streaming_queue_.dropped_count();
        frame_metrics.preprocess_failure_count =
            preprocess_thread_.failure_count();
        frame_metrics.streaming_submit_failure_count =
            streaming_thread_.failure_count();
        frame_metrics.capture_queue_push_failure_count =
            capture_thread_.queue_push_failure_count();
        frame_metrics.streaming_queue_push_failure_count =
            preprocess_thread_.queue_push_failure_count();
        frame_metrics.dataset_deadline_miss_count =
            capture_thread_.dataset_deadline_miss_count();


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

        print_stage_summary(
            "H.264 Encoding",
            encoding_metrics
        );

        print_fps_summary(
            "Capture FPS",
            capture_thread_.success_count(),
            capture_thread_.fps_duration_seconds()
        );

        print_fps_summary(
            "Streaming Submit FPS",
            streaming_thread_.success_count(),
            streaming_thread_.fps_duration_seconds()
        );

        print_frame_summary(frame_metrics);


        // Raw measurement CSV
        save_stage_metrics_csv(
            metric_path("capture_metrics.csv"),
            capture_metrics
        );

        save_stage_metrics_csv(
            metric_path("preprocess_metrics.csv"),
            preprocess_metrics
        );

        save_stage_metrics_csv(
            metric_path("streaming_metrics.csv"),
            streaming_metrics
        );

        save_encoding_metrics_csv(
            metric_path("encoding_metrics.csv"),
            encoding_metrics
        );

        save_fps_metrics_csv(
            metric_path("fps_metrics.csv"),
            capture_thread_.fps_metrics(),
            streaming_thread_.fps_metrics()
        );

        save_frame_metrics_csv(
            metric_path("frame_metrics.csv"),
            frame_metrics
        );

        std::ofstream network_file(
            metric_path("network_metrics.csv")
        );
        if (network_file.is_open())
        {
            network_file
                << "rtp_packet_count,rtp_byte_count,"
                << "duration_seconds,average_bitrate_kbps,"
                << "metadata_injected_count,"
                << "metadata_lookup_miss_count,"
                << "metadata_extension_failure_count\n"
                << std::fixed << std::setprecision(3)
                << network_metrics.rtp_packet_count << ","
                << network_metrics.rtp_byte_count << ","
                << network_metrics.duration_seconds << ","
                << network_metrics.average_bitrate_kbps << ","
                << network_metrics.metadata_injected_count << ","
                << network_metrics.metadata_lookup_miss_count << ","
                << network_metrics.metadata_extension_failure_count
                << "\n";
        }
    }


    Logger::info("[Application] 종료");

    return true;
}
