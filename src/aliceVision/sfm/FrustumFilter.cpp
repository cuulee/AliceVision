// This file is part of the AliceVision project.
// Copyright (c) 2016 AliceVision contributors.
// Copyright (c) 2012 openMVG contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <aliceVision/sfm/FrustumFilter.hpp>
#include <aliceVision/sfm/sfm.hpp>
#include <aliceVision/stl/mapUtils.hpp>
#include <aliceVision/types.hpp>
#include <aliceVision/geometry/HalfPlane.hpp>
#include <aliceVision/config.hpp>

#include <boost/progress.hpp>

#include <fstream>

namespace aliceVision {
namespace sfm {

using namespace aliceVision::camera;
using namespace aliceVision::geometry;
using namespace aliceVision::geometry::halfPlane;

// Constructor
FrustumFilter::FrustumFilter(const SfMData & sfm_data,
  const double zNear, const double zFar)
{
  //-- Init Z_Near & Z_Far for all valid views
  init_z_near_z_far_depth(sfm_data, zNear, zFar);
  const bool bComputed_Z = (zNear == -1. && zFar == -1.) && !sfm_data.structure.empty();
  _bTruncated = (zNear != -1. && zFar != -1.) || bComputed_Z;
  initFrustum(sfm_data);
}

// Init a frustum for each valid views of the SfM scene
void FrustumFilter::initFrustum(const SfMData & sfm_data)
{
  for (NearFarPlanesT::const_iterator it = z_near_z_far_perView.begin();
      it != z_near_z_far_perView.end(); ++it)
  {
    const View * view = sfm_data.getViews().at(it->first).get();
    if (!sfm_data.isPoseAndIntrinsicDefined(view))
      continue;
    Intrinsics::const_iterator iterIntrinsic = sfm_data.getIntrinsics().find(view->getIntrinsicId());
    if (!isPinhole(iterIntrinsic->second.get()->getType()))
      continue;

    const Pose3 pose = sfm_data.getPose(*view).getTransform();

    const Pinhole * cam = dynamic_cast<const Pinhole*>(iterIntrinsic->second.get());
    if (cam == nullptr)
      continue;

    if (!_bTruncated) // use infinite frustum
    {
      const Frustum f(
        cam->w(), cam->h(), cam->K(),
        pose.rotation(), pose.center());
      frustum_perView[view->getViewId()] = f;
    }
    else // use truncated frustum with defined Near and Far planes
    {
      const Frustum f(cam->w(), cam->h(), cam->K(),
        pose.rotation(), pose.center(), it->second.first, it->second.second);
      frustum_perView[view->getViewId()] = f;
    }
  }
}

PairSet FrustumFilter::getFrustumIntersectionPairs() const
{
  PairSet pairs;
  // List active view Id
  std::vector<IndexT> viewIds;
  viewIds.reserve(z_near_z_far_perView.size());
  std::transform(z_near_z_far_perView.begin(), z_near_z_far_perView.end(),
    std::back_inserter(viewIds), stl::RetrieveKey());

  boost::progress_display my_progress_bar(
    viewIds.size() * (viewIds.size()-1)/2,
    std::cout, "\nCompute frustum intersection\n");

  // Exhaustive comparison (use the fact that the intersect function is symmetric)
  #pragma omp parallel for
  for (int i = 0; i < (int)viewIds.size(); ++i)
  {
    for (size_t j = i+1; j < viewIds.size(); ++j)
    {
      if (frustum_perView.at(viewIds[i]).intersect(frustum_perView.at(viewIds[j])))
      {
        #pragma omp critical
        {
          pairs.insert(std::make_pair(viewIds[i], viewIds[j]));
        }
      }
      // Progress bar update
      #pragma omp critical
      {
        ++my_progress_bar;
      }
    }
  }
  return pairs;
}

// Export defined frustum in PLY file for viewing
bool FrustumFilter::export_Ply(const std::string & filename) const
{
  std::ofstream of(filename.c_str());
  if (!of.is_open())
    return false;
  // Vertex count evaluation
  // Faces count evaluation
  size_t vertex_count = 0;
  size_t face_count = 0;
  for (FrustumsT::const_iterator it = frustum_perView.begin();
    it != frustum_perView.end(); ++it)
  {
    if (it->second.isInfinite())
    {
      vertex_count += 5;
      face_count += 5; // 4 triangles + 1 quad
    }
    else // truncated
    {
      vertex_count += 8;
      face_count += 6; // 6 quads
    }
  }

  of << "ply" << '\n'
    << "format ascii 1.0" << '\n'
    << "element vertex " << vertex_count << '\n'
    << "property float x" << '\n'
    << "property float y" << '\n'
    << "property float z" << '\n'
    << "element face " << face_count << '\n'
    << "property list uchar int vertex_index" << '\n'
    << "end_header" << '\n';

  // Export frustums points
  for (FrustumsT::const_iterator it = frustum_perView.begin();
    it != frustum_perView.end(); ++it)
  {
    const std::vector<Vec3> & points = it->second.frustum_points();
    for (int i=0; i < points.size(); ++i)
      of << points[i].transpose() << '\n';
  }

  // Export frustums faces
  IndexT count = 0;
  for (FrustumsT::const_iterator it = frustum_perView.begin();
    it != frustum_perView.end(); ++it)
  {
    if (it->second.isInfinite()) // infinite frustum: drawn normalized cone: 4 faces
    {
      of << "3 " << count + 0 << ' ' << count + 1 << ' ' << count + 2 << '\n'
        << "3 " << count + 0 << ' ' << count + 2 << ' ' << count + 3 << '\n'
        << "3 " << count + 0 << ' ' << count + 3 << ' ' << count + 4 << '\n'
        << "3 " << count + 0 << ' ' << count + 4 << ' ' << count + 1 << '\n'
        << "4 " << count + 1 << ' ' << count + 2 << ' ' << count + 3 << ' ' << count + 4 << '\n';
      count += 5;
    }
    else // truncated frustum: 6 faces
    {
      of << "4 " << count + 0 << ' ' << count + 1 << ' ' << count + 2 << ' ' << count + 3 << '\n'
        << "4 " << count + 0 << ' ' << count + 1 << ' ' << count + 5 << ' ' << count + 4 << '\n'
        << "4 " << count + 1 << ' ' << count + 5 << ' ' << count + 6 << ' ' << count + 2 << '\n'
        << "4 " << count + 3 << ' ' << count + 7 << ' ' << count + 6 << ' ' << count + 2 << '\n'
        << "4 " << count + 0 << ' ' << count + 4 << ' ' << count + 7 << ' ' << count + 3 << '\n'
        << "4 " << count + 4 << ' ' << count + 5 << ' ' << count + 6 << ' ' << count + 7 << '\n';
      count += 8;
    }
  }
  of.flush();
  bool bOk = of.good();
  of.close();
  return bOk;
}

void FrustumFilter::init_z_near_z_far_depth(const SfMData & sfm_data,
  const double zNear, const double zFar)
{
  // If z_near & z_far are -1 and structure if not empty,
  //  compute the values for each camera and the structure
  const bool bComputed_Z = (zNear == -1. && zFar == -1.) && !sfm_data.structure.empty();
  if (bComputed_Z)  // Compute the near & far planes from the structure and view observations
  {
    for (Landmarks::const_iterator itL = sfm_data.getLandmarks().begin();
      itL != sfm_data.getLandmarks().end(); ++itL)
    {
      const Landmark & landmark = itL->second;
      const Vec3 & X = landmark.X;
      for (Observations::const_iterator iterO = landmark.observations.begin();
        iterO != landmark.observations.end(); ++iterO)
      {
        const IndexT id_view = iterO->first;
        const Observation & ob = iterO->second;
        const View * view = sfm_data.getViews().at(id_view).get();
        if (!sfm_data.isPoseAndIntrinsicDefined(view))
          continue;

        Intrinsics::const_iterator iterIntrinsic = sfm_data.getIntrinsics().find(view->getIntrinsicId());
        const Pose3 pose = sfm_data.getPose(*view).getTransform();
        const double z = pose.depth(X);
        NearFarPlanesT::iterator itZ = z_near_z_far_perView.find(id_view);
        if (itZ != z_near_z_far_perView.end())
        {
          if ( z < itZ->second.first)
            itZ->second.first = z;
          else
          if ( z > itZ->second.second)
            itZ->second.second = z;
        }
        else
          z_near_z_far_perView[id_view] = std::make_pair(z,z);
      }
    }
  }
  else
  {
    // Init the same near & far limit for all the valid views
    for (Views::const_iterator it = sfm_data.getViews().begin();
    it != sfm_data.getViews().end(); ++it)
    {
      const View * view = it->second.get();
      if (!sfm_data.isPoseAndIntrinsicDefined(view))
        continue;
      z_near_z_far_perView[view->getViewId()] = std::make_pair(zNear, zFar);
    }
  }
}

} // namespace sfm
} // namespace aliceVision

