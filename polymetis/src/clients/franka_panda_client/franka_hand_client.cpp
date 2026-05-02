#include "polymetis/clients/franka_hand_client.hpp"

#include "spdlog/spdlog.h"
#include <string>
#include <time.h>

#include <grpc/grpc.h>

#include "polymetis.grpc.pb.h"
#include "polymetis/utils.h"

using grpc::ClientContext;

FrankaHandClient::FrankaHandClient(std::shared_ptr<grpc::Channel> channel,
                                   YAML::Node config)
    : stub_(GripperServer::NewStub(channel)), is_moving_(false) {
  // Connect to gripper
  std::string robot_ip = config["robot_ip"].as<std::string>();
  if (config["low_latency_gripper"]) {
    low_latency_gripper_ = config["low_latency_gripper"].as<bool>();
  }
  spdlog::info("Connecting to robot_ip {}", robot_ip);
  gripper_.reset(new franka::Gripper(robot_ip));

  // Initialize gripper
  gripper_->homing();

  // Initialize server connection
  franka::GripperState franka_gripper_state = gripper_->readOnce();

  GripperMetadata metadata;
  metadata.set_max_width(franka_gripper_state.max_width);
  metadata.set_hz(GRIPPER_HZ);

  ClientContext context;
  Empty empty;
  stub_->InitRobotClient(&context, metadata, &empty);

  if (low_latency_gripper_) {
    command_thread_ = std::thread(&FrankaHandClient::commandWorker, this);
  }

  spdlog::info("Connected. low_latency_gripper={}", low_latency_gripper_);
}

void FrankaHandClient::getGripperState(void) {
  franka::GripperState franka_gripper_state = gripper_->readOnce();

  gripper_state_.set_width(franka_gripper_state.width);
  gripper_state_.set_is_grasped(franka_gripper_state.is_grasped);
  gripper_state_.set_is_moving(is_moving_.load());
  gripper_state_.set_prev_command_successful(prev_cmd_successful_);

  // gripper_state.time();  // Use current timestamp instead!
  setTimestampToNow(gripper_state_.mutable_timestamp());
}

void FrankaHandClient::applyGripperCommand(const GripperCommand &gripper_cmd) {
  is_moving_ = true;

  if (gripper_cmd.grasp()) {
    spdlog::info("Grasping at width {} at speed={}", gripper_cmd.width(),
                 gripper_cmd.speed());
    double eps_inner = (gripper_cmd.epsilon_inner() < 0)
                           ? EPSILON_INNER
                           : gripper_cmd.epsilon_inner();
    double eps_outer = (gripper_cmd.epsilon_outer() < 0)
                           ? EPSILON_OUTER
                           : gripper_cmd.epsilon_outer();
    prev_cmd_successful_ =
        gripper_->grasp(gripper_cmd.width(), gripper_cmd.speed(),
                        gripper_cmd.force(), eps_inner, eps_outer);

  } else {
    spdlog::info("Moving to width {} at speed={}", gripper_cmd.width(),
                 gripper_cmd.speed());
    prev_cmd_successful_ =
        gripper_->move(gripper_cmd.width(), gripper_cmd.speed());
  }

  is_moving_ = false;
}

void FrankaHandClient::applyGripperCommandAsync(void) {
  applyGripperCommand(gripper_cmd_);
}

void FrankaHandClient::commandWorker(void) {
  while (true) {
    GripperCommand cmd;
    {
      std::unique_lock<std::mutex> lock(command_mutex_);
      command_cv_.wait(lock, [&] { return has_pending_command_; });
      cmd = latest_gripper_cmd_;
      has_pending_command_ = false;
    }

    try {
      applyGripperCommand(cmd);
    } catch (const std::exception &e) {
      prev_cmd_successful_ = false;
      is_moving_ = false;
      spdlog::error("Failed to command Franka Hand: {}", e.what());
    }
  }
}

void FrankaHandClient::handleNewCommand(const GripperCommand &gripper_cmd) {
  {
    std::lock_guard<std::mutex> lock(command_mutex_);
    latest_gripper_cmd_ = gripper_cmd;
    has_pending_command_ = true;
  }

  if (is_moving_.load()) {
    try {
      spdlog::info("Interrupting active gripper command for newer target width {}",
                   gripper_cmd.width());
      gripper_->stop();
    } catch (const std::exception &e) {
      spdlog::warn("Failed to stop active Franka Hand command: {}", e.what());
    }
  }

  command_cv_.notify_one();
}

void FrankaHandClient::run(void) {
  long period_ns = 1000000000L / GRIPPER_HZ;

  int timestamp_ns;

  struct timespec abs_target_time;
  clock_gettime(CLOCK_REALTIME, &abs_target_time);
  while (true) {
    // Run control step
    getGripperState();

    grpc::ClientContext context;
    status_ = stub_->ControlUpdate(&context, gripper_state_, &gripper_cmd_);

    timestamp_ns = gripper_cmd_.timestamp().nanos();
    if (timestamp_ns != prev_cmd_timestamp_ns_ && timestamp_ns) {
      if (low_latency_gripper_) {
        // Accept newer commands even while the hand is moving so slider teleop
        // can preempt stale full-width moves.
        handleNewCommand(gripper_cmd_);
        prev_cmd_timestamp_ns_ = timestamp_ns;
      } else if (!is_moving_.load()) {
        // Original behavior: only start a command when the current move is done.
        std::thread th(&FrankaHandClient::applyGripperCommandAsync, this);
        th.detach();
        prev_cmd_timestamp_ns_ = timestamp_ns;
      }
    }

    // Spin once
    abs_target_time.tv_nsec += period_ns;
    while (abs_target_time.tv_nsec >= 1000000000L) {
      abs_target_time.tv_nsec -= 1000000000L;
      abs_target_time.tv_sec += 1;
    }
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &abs_target_time, nullptr);
  }
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    spdlog::error("Usage: franka_hand_client /path/to/cfg.yaml");
    return 1;
  }
  YAML::Node config = YAML::LoadFile(argv[1]);

  // Launch client
  std::string control_address = config["control_ip"].as<std::string>() + ":" +
                                config["control_port"].as<std::string>();
  FrankaHandClient franka_hand_client(
      grpc::CreateChannel(control_address, grpc::InsecureChannelCredentials()),
      config);
  franka_hand_client.run();

  // Termination
  spdlog::info("Wait for shutdown; press CTRL+C to close.");

  return 0;
}
