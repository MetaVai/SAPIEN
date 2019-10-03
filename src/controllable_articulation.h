//
// Created by sim on 10/2/19.
//
#pragma once
#include "articulation_interface.h"
#include <memory>
#include <mutex>
#include <queue>

class ThreadSafeQueue {
  std::mutex mLock;
  std::queue<std::vector<float>> mQueue;

public:
  ThreadSafeQueue();

  void push(const std::vector<float> &vec);
  void pushValue(const std::vector<float> vec);
  std::vector<float> pop();
  bool empty();
  void clear();
};

class ControllableArticulationWrapper {
private:
  std::shared_ptr<ThreadSafeQueue> jointStateQueue = std::make_unique<ThreadSafeQueue>();
  std::vector<ThreadSafeQueue *> positionControllerQueueList = {};
  std::vector<std::vector<uint32_t>> positionControllerIndexList = {};
  std::vector<ThreadSafeQueue *> velocityControllerQueueList = {};
  std::vector<std::vector<uint32_t>> velocityControllerIndexList = {};

  std::vector<physx::PxReal> driveQpos;
  bool controllerActive = false;

  // Cache
  std::vector<std::string> jointNames;

public:
  IArticulationDrivable *articulation;

private:
  void update(physx::PxReal timestep);
  void updateJointState();
  void driveFromPositionController();
  void driveFromVelocityController(physx::PxReal timestep);
  ThreadSafeQueue *getJointStateQueue();

public:
  explicit ControllableArticulationWrapper(IArticulationDrivable *articulation);
  bool add_position_controller(const std::vector<std::string> &jointName, ThreadSafeQueue *queue);
  bool add_velocity_controller(const std::vector<std::string> &jointName, ThreadSafeQueue *queue);

  // This function should be called every simulation step
  void update();
};
