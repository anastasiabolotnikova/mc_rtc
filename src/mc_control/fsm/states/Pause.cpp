#include <mc_control/fsm/states/Pause.h>

#include <mc_control/fsm/Controller.h>

namespace mc_control
{

namespace fsm
{

void PauseState::configure(const mc_rtc::Configuration & config)
{
  config("duration", duration_);
}

void PauseState::start(Controller & ctl)
{
  assert(duration_ >= 0);
  tick_ = 0;
  goal_ = std::ceil(duration_ / ctl.solver().dt());
}

bool PauseState::run(Controller&)
{
  if(++tick_ > goal_)
  {
    output("OK");
    return true;
  }
  return false;
}

} // namespace fsm

} // namespace mc_control

EXPORT_SINGLE_STATE("Pause", mc_control::fsm::PauseState)
