#ifndef HECTOR_TESTING_UTILS_TEST_FIXTURES_HPP
#define HECTOR_TESTING_UTILS_TEST_FIXTURES_HPP

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>

#include <hector_testing_utils/test_context.hpp>
#include <hector_testing_utils/test_executor.hpp>
#include <hector_testing_utils/test_node.hpp>

namespace hector_testing_utils
{

/// @brief Base test fixture that provides a TestExecutor and TestNode.
///
/// This fixture initializes rclcpp if needed and provides executor_ and tester_node_
/// members for use in tests.
class HectorTestFixture : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if ( !rclcpp::ok() ) {
      rclcpp::init( 0, nullptr );
    }
    // Allow derived classes to provide a custom executor (e.g. MultiThreaded)
    // by overriding create_test_executor()
    std::shared_ptr<rclcpp::Executor> internal_exec = create_test_executor();
    if ( !internal_exec ) {
      throw std::runtime_error( "create_test_executor() returned nullptr" );
    }

    executor_ = std::make_shared<TestExecutor>( internal_exec );

    // Instantiate our new smart TestNode
    tester_node_ = std::make_shared<TestNode>( "hector_tester_node" );
    executor_->add_node( tester_node_ );
  }

  void TearDown() override
  {
    // Clean up nodes and executor before shutdown to avoid
    executor_.reset();
    tester_node_.reset();

    if ( rclcpp::ok() ) {
      rclcpp::shutdown();
    }
  }

  /// @brief Virtual method to allow overriding the executor type.
  /// @return A shared pointer to the executor to use.
  virtual std::shared_ptr<rclcpp::Executor> create_test_executor()
  {
    return std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
  }

  std::shared_ptr<TestExecutor> executor_;
  std::shared_ptr<TestNode> tester_node_;
};

/// @brief Test fixture that uses an isolated context instead of the global context.
///
/// This fixture creates its own rclcpp::Context, allowing tests to run in complete
/// isolation from other tests.
class HectorTestFixtureWithContext : public ::testing::Test
{
protected:
  void SetUp() override
  {
    context_ = std::make_shared<TestContext>();
    executor_ = std::make_shared<TestExecutor>( context_->context() );
    tester_node_ = std::make_shared<TestNode>( "hector_tester_node", context_->node_options() );
    executor_->add_node( tester_node_ );
  }

  void TearDown() override
  {
    // Executor first to stop any background spinner before the node is destroyed.
    executor_.reset();
    tester_node_.reset();
    context_.reset();
  }

  std::shared_ptr<TestContext> context_;
  std::shared_ptr<TestExecutor> executor_;
  std::shared_ptr<TestNode> tester_node_;
};

// =============================================================================
// Helper Functions
// =============================================================================

/// @brief Create NodeOptions that load parameters from a YAML file.
/// @param params_file Path to the YAML parameters file.
/// @param extra_arguments Additional ROS arguments.
/// @return Configured NodeOptions.
inline rclcpp::NodeOptions
node_options_from_yaml( const std::string &params_file,
                        const std::vector<std::string> &extra_arguments = {} )
{
  std::vector<std::string> args = { "--ros-args", "--params-file", params_file };
  args.insert( args.end(), extra_arguments.begin(), extra_arguments.end() );
  rclcpp::NodeOptions options;
  options.arguments( args );
  options.automatically_declare_parameters_from_overrides( true );
  return options;
}

} // namespace hector_testing_utils

#endif // HECTOR_TESTING_UTILS_TEST_FIXTURES_HPP
