#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Matrix4.h>

#include <mc_rtc/gui/types.h>

inline Magnum::Color4 convert(const mc_rtc::gui::Color & c)
{
  return {static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b), static_cast<float>(c.a)};
}

inline Magnum::Vector3 translation(const Eigen::Vector3d & v)
{
  return {static_cast<float>(v.x()), static_cast<float>(v.y()), static_cast<float>(v.z())};
}

inline Magnum::Vector3 translation(const sva::PTransformd & pt)
{
  return translation(pt.translation());
}

inline Magnum::Matrix3 convert(const Eigen::Matrix3d & m)
{
  Eigen::Matrix3f m3f = m.transpose().cast<float>();
  return Magnum::Matrix3::from(m3f.data());
}

inline Magnum::Matrix4 convert(const sva::PTransformd & pt)
{
  return Magnum::Matrix4::from(convert(pt.rotation()), translation(pt.translation()));
}

inline Magnum::Matrix4 convert(const Eigen::Vector3d & t)
{
  return Magnum::Matrix4::translation(translation(t));
}
