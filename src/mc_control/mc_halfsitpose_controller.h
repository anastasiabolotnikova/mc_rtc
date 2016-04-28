#pragma once

#include <mc_control/mc_controller.h>

#include <mc_control/api.h>
#include <vector>

namespace mc_control
{

struct MC_CONTROL_DLLAPI MCHalfSitPoseController : public MCController
{
public:
  /* Common stuff */
  MCHalfSitPoseController(std::shared_ptr<mc_rbdyn::RobotModule> robot, double dt);

  virtual bool run() override;

private:
  std::vector<std::vector<double>> halfSitPose;

};

}

SIMPLE_CONTROLLER_CONSTRUCTOR("HalfSitPose", mc_control::MCHalfSitPoseController)
