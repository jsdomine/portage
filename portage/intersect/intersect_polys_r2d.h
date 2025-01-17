/*
This file is part of the Ristra portage project.
Please see the license file at the root of this repository, or at:
    https://github.com/laristra/portage/blob/master/LICENSE
*/

#ifndef INTERSECT_POLYS_R2D_H
#define INTERSECT_POLYS_R2D_H

#include <array>
#include <stdexcept>
#include <vector>
#include <algorithm>

#include "wonton/support/CoordinateSystem.h"
#include "wonton/support/wonton.h"
#include "wonton/support/Point.h"

extern "C" {
#include "r2d.h"
}

#include "portage/support/portage.h"

namespace Portage {

// intersect one source polygon (possibly non-convex) with a
// triangular decomposition of a target polygon

inline
std::vector<double>
intersect_polys_r2d(std::vector<Wonton::Point<2>> const & source_poly,
                    std::vector<Wonton::Point<2>> const & target_poly,
                    NumericTolerances_t num_tols,
                    bool trg_convex=true,
                    Wonton::CoordSysType coord_sys = Wonton::CoordSysType::Cartesian) {

  int poly_order = 1;  // max degree of moments to calculate
  if (coord_sys == Wonton::CoordSysType::CylindricalAxisymmetric)
    poly_order = 2;

  int nmoments = R2D_NUM_MOMENTS(poly_order);
  std::vector<double> moments(nmoments, 0);

  // Initialize source polygon

  const int size1 = source_poly.size();
  const int size2 = target_poly.size();
  if (!size1 || !size2)
    return moments;  // could allow top level code to avoid an 'if' statement

  std::vector<r2d_rvec2> verts1(size1);
  for (int i = 0; i < size1; ++i) {
    verts1[i].xy[0] = source_poly[i][0];
    verts1[i].xy[1] = source_poly[i][1];
  }

  r2d_poly srcpoly_r2d;
  r2d_init_poly(&srcpoly_r2d, &verts1[0], size1);


  // Initialize target polygon and check for convexity

  std::vector<r2d_plane> faces(size2);
  std::vector<r2d_rvec2> verts2(size2);
  for (int i = 0; i < size2; ++i) {
    verts2[i].xy[0] = target_poly[i][0];
    verts2[i].xy[1] = target_poly[i][1];
  }

  // case 1:  target_poly is convex
  // can simply use faces of target_poly as clip planes
  if (trg_convex) {
    r2d_poly_faces_from_verts(&faces[0], &verts2[0], size2);

    // clip the first poly against the faces of the second
    r2d_clip(&srcpoly_r2d, &faces[0], size2);

    // find the moments (up to quadratic order) of the clipped poly
    r2d_real om[nmoments];
    r2d_reduce(&srcpoly_r2d, om, poly_order);

    // Copy and optionally shift moments:
    for (int j = 0; j < nmoments; ++j)
      moments[j] = om[j];

    if (coord_sys == Wonton::CoordSysType::CylindricalAxisymmetric)
      Wonton::CylindricalAxisymmetricCoordinates::shift_moments_list<2>(moments);

  } else {  // case 2:  target_poly is non-convex

    // Must divide target_poly into triangles for clipping.  Choice
    // of the central point is crucial. Try the centroid first -
    // computed by the area weighted sum of centroids of any
    // triangulation of the polygon
    bool center_point_ok = true;
    Wonton::Point<2> cen(0.0, 0.0);
    r2d_rvec2 cenr2d;
    cenr2d.xy[0] = 0.0; cenr2d.xy[1] = 0.0;
    double area_sum = 0.0;
    for (int i = 1; i < size2; ++i) {
      double area = r2d_orient(verts2[0], verts2[i], verts2[(i+1)%size2]);
      area_sum += area;
      Wonton::Point<2> tricen =
          (target_poly[0] + target_poly[i] + target_poly[(i+1)%size2])/3.0;
      cen += area*tricen;
    }
    cen /= area_sum;
    cenr2d.xy[0] = cen[0]; cenr2d.xy[1] = cen[1];
    
    for (int i = 0; i < size2; ++i)
      if (r2d_orient(cenr2d, verts2[i], verts2[(i+1)%size2]) < 0.)
        center_point_ok = false;
    
    if (!center_point_ok) {
      // If the centroid is not ok, we have to find the center of
      // the feasible set of the polygon. This means clipping the
      // target_poly with its own face planes/lines. For a
      // non-convex polygon, this will give a new polygon whose
      // interior (See Garimella/Shashkov/Pavel paper on untangling)
      
      r2d_poly fspoly;
      r2d_init_poly(&fspoly, &verts2[0], size2);
      
      r2d_poly_faces_from_verts(&faces[0], &verts2[0], size2);
      
      r2d_clip(&fspoly, &faces[0], size2);
      
      // If the resulting polygon is empty, we are out of luck
      if (fspoly.nverts == 0)
        std::runtime_error("intersect_polys_r2d.h: Could not find a valid center point to triangulate non-convex polygon");
      
      // Have R2D compute first and second moments of polygon and
      // get its centroid from that
      
      r2d_real fspoly_moments[R2D_NUM_MOMENTS(poly_order)];
      r2d_reduce(&fspoly, fspoly_moments, poly_order);
      
      cen[0] = cenr2d.xy[0] = fspoly_moments[1]/fspoly_moments[0];
      cen[1] = cenr2d.xy[1] = fspoly_moments[2]/fspoly_moments[0];
      
      // Even if the resulting feasible set polygon has vertices,
      // maybe its degenerate. So we have to verify that its centroid
      // indeed is a point that will give valid triangles when
      // paired with the edges of the target polygon.
      
      for (int i = 0; i < size2; ++i)
        if (r2d_orient(cenr2d, verts2[i], verts2[(i+1)%size2]) < 0.)
          center_point_ok = false;
    }

    // If we still don't have a good center point, we are out of luck
    if (!center_point_ok)
      std::runtime_error("intersect_polys_r2d.h: Could not find a valid center point to triangulate non-convex polygon");
    
    
    verts2[0].xy[0] = cen[0];
    verts2[0].xy[1] = cen[1];
    for (int i = 0; i < size2; ++i) {
      verts2[1].xy[0] = target_poly[i][0];
      verts2[1].xy[1] = target_poly[i][1];
      verts2[2].xy[0] = target_poly[(i+1)%size2][0];
      verts2[2].xy[1] = target_poly[(i+1)%size2][1];
      
      r2d_poly_faces_from_verts(&faces[0], &verts2[0], 3);
      
      r2d_poly srcpoly_r2d_copy = srcpoly_r2d;
      
      // clip the first poly against the faces of the second
      r2d_clip(&srcpoly_r2d_copy, &faces[0], 3);
      
      // find the moments (up to quadratic order) of the clipped poly
      r2d_real om[R2D_NUM_MOMENTS(poly_order)];
      r2d_reduce(&srcpoly_r2d_copy, om, poly_order);
      
      // Accumulate moments:
      for (int j = 0; j < 3; ++j)
        moments[j] += om[j];
    }  // for i
  }  // if convex {} else {}

  return moments;
}

}  // namespace Portage

#endif // INTERSECT_POLYS_R2D_H
