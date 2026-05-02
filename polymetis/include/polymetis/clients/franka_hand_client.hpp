#include "spdlog/spdlog.h"
#include "yaml-cpp/yaml.h"

#include "polymetis/utils.h"
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <franka/gripper.h>
#include <franka/gripper_state.h>
#include <mutex>
#include <thread>

#define GRIPPER_HZ 30

// Define tolerances to be able to grasp any object without specifying width
#define EPSILON_INNER 0.2
#define EPSILON_OUTER 0.2

class FrankaHandClient {
private:
  void getGripperState(void);
  void applyGripperCommand(const GripperCommand &gripper_cmd);
  void applyGripperCommandAsync(void);
  void commandWorker(void);
  void handleNewCommand(const GripperCommand &gripper_cmd);

  // gRPC
  std::unique_ptr<GripperServer::Stub> stub_;
  grpc::Status status_;

  GripperState gripper_state_;
  GripperCommand gripper_cmd_;
  int prev_cmd_timestamp_ns_ = 0;
  bool prev_cmd_successful_ = true;
  bool low_latency_gripper_ = false;

  // Franka
  std::shared_ptr<franka::Gripper> gripper_;
  std::atomic<bool> is_moving_;

  // Latest-command tracking for low-latency slider teleop.
  std::thread command_thread_;
  std::mutex command_mutex_;
  std::condition_variable command_cv_;
  GripperCommand latest_gripper_cmd_;
  bool has_pending_command_ = false;

public:
  FrankaHandClient(std::shared_ptr<grpc::Channel> channel, YAML::Node config);
  void run(void);
};
