#include "external/envoy/source/common/protobuf/protobuf.h"
#include "external/envoy/test/mocks/common.h"

#include "api/adaptive_load/benchmark_result.pb.h"
#include "api/adaptive_load/metric_spec.pb.h"
#include "api/adaptive_load/scoring_function_impl.pb.h"
#include "api/client/service.pb.h"

#include "source/adaptive_load/metrics_evaluator_impl.h"

#include "test/adaptive_load/fake_plugins/fake_metrics_plugin/fake_metrics_plugin.h"
#include "test/adaptive_load/minimal_output.h"
#include "test/test_common/proto_matchers.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Nighthawk {
namespace {

using ::Envoy::Protobuf::util::MessageDifferencer;
using ::nighthawk::adaptive_load::BenchmarkResult;
using ::nighthawk::adaptive_load::FakeMetricsPluginConfig;
using ::nighthawk::adaptive_load::MetricEvaluation;
using ::nighthawk::adaptive_load::MetricSpec;
using ::nighthawk::adaptive_load::ThresholdSpec;

using ::testing::_;
using ::testing::AllOf;
using ::testing::Eq;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::Return;

// For testing MetricPlugin with GetMetricByNameWithReportingPeriod defined.
class MockMetricsPluginWithReportingPeriod : public FakeMetricsPlugin {
public:
  explicit MockMetricsPluginWithReportingPeriod(
      const nighthawk::adaptive_load::FakeMetricsPluginConfig& config)
      : FakeMetricsPlugin(config) {}

  MOCK_METHOD(absl::StatusOr<double>, GetMetricByNameWithReportingPeriod,
              (absl::string_view metric_name, const ReportingPeriod& reporting_period), (override));
};

/**
 * Creates a valid TypedExtensionConfig proto selecting the real BinaryScoringFunction plugin
 * and configuring it with a threshold.
 *
 * @param lower_threshold Threshold value to set within the config proto.
 *
 * @return TypedExtensionConfig Full scoring function plugin spec that selects
 * nighthawk.binary_scoring and provides a config.
 */
envoy::config::core::v3::TypedExtensionConfig
MakeLowerThresholdBinaryScoringFunctionConfig(double lower_threshold) {
  envoy::config::core::v3::TypedExtensionConfig config;
  config.set_name("nighthawk.binary_scoring");
  nighthawk::adaptive_load::BinaryScoringFunctionConfig inner_config;
  inner_config.mutable_lower_threshold()->set_value(lower_threshold);
  config.mutable_typed_config()->PackFrom(inner_config);
  return config;
}

/**
 * Creates a simulated Nighthawk Service response that reflects the specified send rate.
 *
 * @param send_rate The send rate that the proto values should represent.
 *
 * @return ExecutionResponse A simulated Nighthawk Service response with counters representing the
 * specified send rate, along with other dummy counters and stats.
 */
nighthawk::client::ExecutionResponse MakeNighthawkResponseWithSendRate(double send_rate) {
  nighthawk::client::ExecutionResponse response;
  nighthawk::client::Output output = MakeSimpleNighthawkOutput({
      /*concurrency=*/"auto",
      /*requests_per_second=*/1024,
      /*actual_duration_seconds=*/10,
      /*upstream_rq_total=*/static_cast<int>(10 * 1024 * send_rate),
      /*response_count_2xx=*/320,
      /*min_ns=*/400,
      /*mean_ns=*/500,
      /*max_ns=*/600,
      /*pstdev_ns=*/11,
  });
  *response.mutable_output() = output;
  return response;
}

TEST(EvaluateMetric, SetsMetricIdForValidMetric) {
  const std::string kMetricName = "good-metric";
  FakeMetricsPluginConfig config;
  FakeMetricsPluginConfig::FakeMetric* fake_metric = config.mutable_fake_metrics()->Add();
  fake_metric->set_name(kMetricName);
  FakeMetricsPlugin fake_plugin(config);

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);

  ReportingPeriod reporting_period;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<MetricEvaluation> evaluation_or = evaluator.EvaluateMetric(
      metric_spec, fake_plugin, /*threshold_spec=*/nullptr, reporting_period);
  ASSERT_TRUE(evaluation_or.ok());
  nighthawk::adaptive_load::MetricEvaluation evaluation = evaluation_or.value();
  EXPECT_EQ(evaluation.metric_id(), "nighthawk.fake_metrics_plugin/good-metric");
}

TEST(EvaluateMetric, PropagatesMetricsPluginError) {
  const int kExpectedStatusCode = static_cast<int>(absl::StatusCode::kFailedPrecondition);
  const std::string kMetricName = "bad-metric";
  const std::string kExpectedStatusMessage = "artificial metric error";
  FakeMetricsPluginConfig config;
  FakeMetricsPluginConfig::FakeMetric* fake_metric = config.mutable_fake_metrics()->Add();
  fake_metric->set_name(kMetricName);
  fake_metric->mutable_error_status()->set_code(kExpectedStatusCode);
  fake_metric->mutable_error_status()->set_message(kExpectedStatusMessage);
  FakeMetricsPlugin fake_plugin(config);

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);

  ReportingPeriod reporting_period;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<MetricEvaluation> evaluation_or = evaluator.EvaluateMetric(
      metric_spec, fake_plugin, /*threshold_spec=*/nullptr, reporting_period);
  ASSERT_FALSE(evaluation_or.ok());
  EXPECT_EQ(static_cast<int>(evaluation_or.status().code()), kExpectedStatusCode);
  EXPECT_THAT(evaluation_or.status().message(), HasSubstr(kExpectedStatusMessage));
}

TEST(EvaluateMetric, StoresMetricValueForValidMetric) {
  const std::string kMetricName = "good-metric";
  const double kExpectedValue = 123.0;
  FakeMetricsPluginConfig config;
  FakeMetricsPluginConfig::FakeMetric* fake_metric = config.mutable_fake_metrics()->Add();
  fake_metric->set_name(kMetricName);
  fake_metric->set_value(kExpectedValue);
  FakeMetricsPlugin fake_plugin(config);

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);

  ReportingPeriod reporting_period;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<MetricEvaluation> evaluation_or = evaluator.EvaluateMetric(
      metric_spec, fake_plugin, /*threshold_spec=*/nullptr, reporting_period);
  ASSERT_TRUE(evaluation_or.ok());
  EXPECT_EQ(evaluation_or.value().metric_value(), kExpectedValue);
}

TEST(EvaluateMetric, SetsWeightToZeroForValidInformationalMetric) {
  const std::string kMetricName = "good-metric";
  const double kExpectedValue = 123.0;

  FakeMetricsPluginConfig config;
  FakeMetricsPluginConfig::FakeMetric* fake_metric = config.mutable_fake_metrics()->Add();
  fake_metric->set_name(kMetricName);
  fake_metric->set_value(kExpectedValue);
  FakeMetricsPlugin fake_plugin(config);

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);

  ReportingPeriod reporting_period;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<MetricEvaluation> evaluation_or = evaluator.EvaluateMetric(
      metric_spec, fake_plugin, /*threshold_spec=*/nullptr, reporting_period);
  ASSERT_TRUE(evaluation_or.ok());
  EXPECT_EQ(evaluation_or.value().weight(), 0.0);
}

TEST(EvaluateMetric, SetsWeightForValidScoredMetric) {
  const std::string kMetricName = "good-metric";
  const double kExpectedValue = 123.0;
  const double kExpectedWeight = 1.5;
  const double kLowerThreshold = 200.0;

  FakeMetricsPluginConfig config;
  FakeMetricsPluginConfig::FakeMetric* fake_metric = config.mutable_fake_metrics()->Add();
  fake_metric->set_name(kMetricName);
  fake_metric->set_value(kExpectedValue);
  FakeMetricsPlugin fake_plugin(config);

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);

  ThresholdSpec threshold_spec;
  threshold_spec.mutable_weight()->set_value(kExpectedWeight);
  *threshold_spec.mutable_scoring_function() =
      MakeLowerThresholdBinaryScoringFunctionConfig(kLowerThreshold);

  ReportingPeriod reporting_period;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<MetricEvaluation> evaluation_or =
      evaluator.EvaluateMetric(metric_spec, fake_plugin, &threshold_spec, reporting_period);
  ASSERT_TRUE(evaluation_or.ok());
  EXPECT_EQ(evaluation_or.value().weight(), kExpectedWeight);
}

TEST(EvaluateMetric, SetsScoreForValidMetric) {
  const std::string kMetricName = "good-metric";
  const double kExpectedValue = 123.0;
  const double kLowerThreshold = 200.0;

  FakeMetricsPluginConfig config;
  FakeMetricsPluginConfig::FakeMetric* fake_metric = config.mutable_fake_metrics()->Add();
  fake_metric->set_name(kMetricName);
  fake_metric->set_value(kExpectedValue);
  FakeMetricsPlugin fake_plugin(config);

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);

  ThresholdSpec threshold_spec;
  *threshold_spec.mutable_scoring_function() =
      MakeLowerThresholdBinaryScoringFunctionConfig(kLowerThreshold);

  ReportingPeriod reporting_period;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<MetricEvaluation> evaluation_or =
      evaluator.EvaluateMetric(metric_spec, fake_plugin, &threshold_spec, reporting_period);
  ASSERT_TRUE(evaluation_or.ok());
  EXPECT_EQ(evaluation_or.value().threshold_score(), -1.0);
}

TEST(ExtractMetricSpecs, ExtractsScoredMetricAndThresholdForValidMetric) {
  const std::string kExpectedMetricName = "a";
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;
  nighthawk::adaptive_load::MetricSpecWithThreshold* metric_threshold =
      spec.mutable_metric_thresholds()->Add();
  metric_threshold->mutable_metric_spec()->set_metric_name(kExpectedMetricName);
  nighthawk::adaptive_load::ThresholdSpec threshold_spec;
  threshold_spec.mutable_weight()->set_value(123.0);
  *metric_threshold->mutable_threshold_spec() = threshold_spec;

  MetricsEvaluatorImpl evaluator;
  const std::vector<std::pair<const nighthawk::adaptive_load::MetricSpec*,
                              const nighthawk::adaptive_load::ThresholdSpec*>>
      spec_threshold_pairs = evaluator.ExtractMetricSpecs(spec);

  ASSERT_GT(spec_threshold_pairs.size(), 0);
  EXPECT_EQ(spec_threshold_pairs[0].first->metric_name(), kExpectedMetricName);
  ASSERT_NE(spec_threshold_pairs[0].second, nullptr);
  EXPECT_THAT(*spec_threshold_pairs[0].second, EqualsProto(threshold_spec));
}

TEST(ExtractMetricSpecs, ExtractsValueForValidInformationalMetric) {
  const std::string kExpectedMetricName = "a";
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;
  nighthawk::adaptive_load::MetricSpec* metric_spec =
      spec.mutable_informational_metric_specs()->Add();
  metric_spec->set_metric_name(kExpectedMetricName);

  MetricsEvaluatorImpl evaluator;
  const std::vector<std::pair<const nighthawk::adaptive_load::MetricSpec*,
                              const nighthawk::adaptive_load::ThresholdSpec*>>
      spec_threshold_pairs = evaluator.ExtractMetricSpecs(spec);

  ASSERT_GT(spec_threshold_pairs.size(), 0);
  EXPECT_EQ(spec_threshold_pairs[0].first->metric_name(), kExpectedMetricName);
  EXPECT_EQ(spec_threshold_pairs[0].second, nullptr);
}

TEST(AnalyzeNighthawkBenchmark, PropagatesNighthawkServiceError) {
  const std::string kExpectedErrorMessage = "artificial nighthawk service error";
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;
  nighthawk::client::ExecutionResponse bad_nighthawk_response;
  bad_nighthawk_response.mutable_error_detail()->set_code(
      static_cast<int>(absl::StatusCode::kUnavailable));
  bad_nighthawk_response.mutable_error_detail()->set_message(kExpectedErrorMessage);
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      bad_nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kUnavailable);
  EXPECT_EQ(result_or.status().message(), kExpectedErrorMessage);
}

TEST(AnalyzeNighthawkBenchmark, StoresNighthawkResultForSuccessfulMetricEvaluation) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;
  nighthawk::client::ExecutionResponse nighthawk_response = MakeNighthawkResponseWithSendRate(1.0);
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_TRUE(result_or.ok());

  EXPECT_TRUE(MessageDifferencer::Equivalent(result_or.value().nighthawk_service_output(),
                                             nighthawk_response.output()));
  EXPECT_THAT(result_or.value().nighthawk_service_output(),
            EqualsProto(nighthawk_response.output()));
}

TEST(AnalyzeNighthawkBenchmark, StoresScoreForSuccessfulMetricEvaluation) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  const std::string kMetricName = "good-metric";
  const double kExpectedValue = 123.0;

  FakeMetricsPluginConfig metrics_plugin_config;
  FakeMetricsPluginConfig::FakeMetric* fake_metric =
      metrics_plugin_config.mutable_fake_metrics()->Add();
  fake_metric->set_name(kMetricName);
  fake_metric->set_value(kExpectedValue);

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);

  *spec.mutable_informational_metric_specs()->Add() = metric_spec;

  nighthawk::client::ExecutionResponse nighthawk_response = MakeNighthawkResponseWithSendRate(1.0);
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;
  name_to_custom_metrics_plugin_map["nighthawk.fake_metrics_plugin"] =
      std::make_unique<FakeMetricsPlugin>(metrics_plugin_config);

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_TRUE(result_or.ok());
  ASSERT_GT(result_or.value().metric_evaluations().size(), 0);
  EXPECT_EQ(result_or.value().metric_evaluations()[0].metric_value(), kExpectedValue);
}

TEST(AnalyzeNighthawkBenchmark, PropagatesErrorFromFailedMetricEvaluation) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  const std::string kMetricName = "bad-metric";
  const std::string kExpectedStatusMessage = "artificial metric error";

  FakeMetricsPluginConfig metrics_plugin_config;
  FakeMetricsPluginConfig::FakeMetric* fake_metric =
      metrics_plugin_config.mutable_fake_metrics()->Add();
  fake_metric->set_name(kMetricName);
  fake_metric->mutable_error_status()->set_code(
      static_cast<int>(absl::StatusCode::kPermissionDenied));
  fake_metric->mutable_error_status()->set_message(kExpectedStatusMessage);

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);
  *spec.mutable_informational_metric_specs()->Add() = metric_spec;

  nighthawk::client::ExecutionResponse nighthawk_response = MakeNighthawkResponseWithSendRate(1.0);
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;
  name_to_custom_metrics_plugin_map["nighthawk.fake_metrics_plugin"] =
      std::make_unique<FakeMetricsPlugin>(metrics_plugin_config);

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_FALSE(result_or.ok());
  // All errors during evaluation are rolled up into a single InternalError.
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kInternal);
  EXPECT_THAT(result_or.status().message(), HasSubstr(kExpectedStatusMessage));
}

TEST(AnalyzeNighthawkBenchmark, UsesBuiltinMetricsPluginForUnspecifiedPluginName) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  const std::string kMetricName = "send-rate";
  const double kExpectedSendRate = 0.5;

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.builtin");
  metric_spec.set_metric_name(kMetricName);

  *spec.mutable_informational_metric_specs()->Add() = metric_spec;

  nighthawk::client::ExecutionResponse nighthawk_response =
      MakeNighthawkResponseWithSendRate(kExpectedSendRate);
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_TRUE(result_or.ok());
  ASSERT_GT(result_or.value().metric_evaluations().size(), 0);
  EXPECT_EQ(result_or.value().metric_evaluations()[0].metric_value(), kExpectedSendRate);
}

TEST(AnalyzeNighthawkBenchmark, ReturnsErrorWithReportingPeriodImplementation) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  const std::string kMetricName = "metric";
  const std::string kWithReportingPeriodStatusMessage = "GetMetricByNameWithReportingPeriod Error";

  FakeMetricsPluginConfig metrics_plugin_config;

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);
  *spec.mutable_informational_metric_specs()->Add() = metric_spec;

  nighthawk::client::ExecutionResponse nighthawk_response = MakeNighthawkResponseWithSendRate(1.0);
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;
  std::unique_ptr<MockMetricsPluginWithReportingPeriod> plugin =
      std::make_unique<MockMetricsPluginWithReportingPeriod>(metrics_plugin_config);
  EXPECT_CALL(*plugin, GetMetricByNameWithReportingPeriod(_, _))
      .WillRepeatedly(Return(absl::InternalError(kWithReportingPeriodStatusMessage)));
  name_to_custom_metrics_plugin_map["nighthawk.fake_metrics_plugin"] = std::move(plugin);

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kInternal);
  EXPECT_THAT(result_or.status().message(), HasSubstr(kWithReportingPeriodStatusMessage));
}

TEST(AnalyzeNighthawkBenchmark, ReturnsSuccessWithReportingPeriodImplementation) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  const std::string kMetricName = "good-metric";
  const double kDeprecatedMetricValue = 123.0;

  FakeMetricsPluginConfig metrics_plugin_config;
  FakeMetricsPluginConfig::FakeMetric* fake_metric =
      metrics_plugin_config.mutable_fake_metrics()->Add();
  fake_metric->set_name(kMetricName);
  fake_metric->set_value(kDeprecatedMetricValue);

  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);

  *spec.mutable_informational_metric_specs()->Add() = metric_spec;

  const double kOverrideMetricValue = 1337;
  nighthawk::client::ExecutionResponse nighthawk_response = MakeNighthawkResponseWithSendRate(1.0);
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;
  std::unique_ptr<MockMetricsPluginWithReportingPeriod> plugin =
      std::make_unique<MockMetricsPluginWithReportingPeriod>(metrics_plugin_config);
  EXPECT_CALL(*plugin, GetMetricByNameWithReportingPeriod(_, _))
      .WillRepeatedly(Return(kOverrideMetricValue));
  name_to_custom_metrics_plugin_map["nighthawk.fake_metrics_plugin"] = std::move(plugin);

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_TRUE(result_or.ok());
  ASSERT_GT(result_or.value().metric_evaluations().size(), 0);
  EXPECT_EQ(result_or.value().metric_evaluations()[0].metric_value(), kOverrideMetricValue);
}

TEST(AnalyzeNighthawkBenchmark, FailsWithNoResults) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;
  nighthawk::client::ExecutionResponse nighthawk_response = MakeNighthawkResponseWithSendRate(1.0);
  nighthawk_response.mutable_output()->clear_results();
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(result_or.status().message(), HasSubstr("output.results cannot be empty."));
}

// Utility to set the execution time and execution duration of a results object.
void SetTimeAndDurationOnResult(nighthawk::client::Result* result, int64_t time_seconds,
                                int64_t duration_seconds) {
  result->mutable_execution_start()->set_seconds(time_seconds);
  result->mutable_execution_start()->set_nanos(0);
  result->mutable_execution_duration()->set_seconds(duration_seconds);
  result->mutable_execution_duration()->set_nanos(0);
}

TEST(AnalyzeNighthawkBenchmark, FailsWithDurationWhereAllWorkersAreNeverActive) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;
  nighthawk::client::ExecutionResponse nighthawk_response = MakeNighthawkResponseWithSendRate(1.0);
  SetTimeAndDurationOnResult(nighthawk_response.mutable_output()->mutable_results(0), 0, 100);
  SetTimeAndDurationOnResult(nighthawk_response.mutable_output()->add_results(), 100, 100);
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(result_or.status().message(),
              HasSubstr("Reported execution times in output.results indicate that there is no time "
                        "where all workers were active."));
}

TEST(AnalyzeNighthawkBenchmark, SelectsOverlappingReportingPeriodWithAllWorkers) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;
  const std::string kMetricName = "good-metric";
  FakeMetricsPluginConfig metrics_plugin_config;
  MetricSpec metric_spec;
  metric_spec.set_metrics_plugin_name("nighthawk.fake_metrics_plugin");
  metric_spec.set_metric_name(kMetricName);
  *spec.mutable_informational_metric_specs()->Add() = metric_spec;

  nighthawk::client::ExecutionResponse nighthawk_response = MakeNighthawkResponseWithSendRate(1.0);
  SetTimeAndDurationOnResult(nighthawk_response.mutable_output()->mutable_results(0), 0, 100);
  SetTimeAndDurationOnResult(nighthawk_response.mutable_output()->add_results(), 50, 100);
  SetTimeAndDurationOnResult(nighthawk_response.mutable_output()->add_results(), 25, 50);

  // Overlapping time for all is 50 -> 75.
  ReportingPeriod kExpectedReportingPeriod;
  kExpectedReportingPeriod.start_time.set_seconds(50);
  kExpectedReportingPeriod.duration.set_seconds(25);
  absl::flat_hash_map<std::string, MetricsPluginPtr> name_to_custom_metrics_plugin_map;
  std::unique_ptr<MockMetricsPluginWithReportingPeriod> plugin =
      std::make_unique<MockMetricsPluginWithReportingPeriod>(metrics_plugin_config);
  EXPECT_CALL(
      *plugin,
      GetMetricByNameWithReportingPeriod(
          _, AllOf(Field(&ReportingPeriod::start_time, Eq(kExpectedReportingPeriod.start_time)),
                   Field(&ReportingPeriod::duration, Eq(kExpectedReportingPeriod.duration)))))
      .WillOnce(Return(10));
  name_to_custom_metrics_plugin_map["nighthawk.fake_metrics_plugin"] = std::move(plugin);

  MetricsEvaluatorImpl evaluator;
  absl::StatusOr<BenchmarkResult> result_or = evaluator.AnalyzeNighthawkBenchmark(
      nighthawk_response, spec, name_to_custom_metrics_plugin_map);
  ASSERT_TRUE(result_or.ok());
}

} // namespace
} // namespace Nighthawk
