/*
This file is part of the Ristra portage project.
Please see the license file at the root of this repository, or at:
    https://github.com/laristra/portage/blob/master/LICENSE
*/


#include <iostream>
#include <memory>

#include "gtest/gtest.h"
#include "mpi.h"

#include "wonton/support/wonton.h"
#include "wonton/mesh/jali/jali_mesh_wrapper.h"
#include "wonton/state/jali/jali_state_wrapper.h"

#include "portage/support/portage.h"
#include "portage/driver/mmdriver.h"

#include "Mesh.hh"
#include "MeshFactory.hh"
#include "JaliState.h"
#include "JaliStateVector.h"


double TOL = 1e-12;

TEST(Test_MultiVar_Remap, Test1) {
  Jali::MeshFactory mf(MPI_COMM_WORLD);
  if (Jali::framework_available(Jali::MSTK))
    mf.framework(Jali::MSTK);
  mf.included_entities({Jali::Entity_kind::CORNER, Jali::Entity_kind::WEDGE});

  std::shared_ptr<Jali::Mesh> source_mesh = mf(0.0, 0.0, 1.0, 1.0, 4, 4);
  std::shared_ptr<Jali::Mesh> target_mesh = mf(0.0, 0.0, 1.0, 1.0, 5, 5);

  const int ncells_target =
      target_mesh->num_entities(Jali::Entity_kind::CELL,
                                Jali::Entity_type::PARALLEL_OWNED);
  const int nnodes_target =
      target_mesh->num_entities(Jali::Entity_kind::NODE,
                                Jali::Entity_type::PARALLEL_OWNED);

  // Create state objects for source and target mesh

  std::shared_ptr<Jali::State> source_state(Jali::State::create(source_mesh));
  std::shared_ptr<Jali::State> target_state(Jali::State::create(target_mesh));

  // Add a constant value state vector on source cells

  double Constant1 = 1.25;
  Jali::UniStateVector<double> myvec1("srccellvars1", source_mesh, nullptr,
                                      Jali::Entity_kind::CELL,
                                      Jali::Entity_type::PARALLEL_OWNED,
                                      Constant1);
  source_state->add(myvec1);

  // Add another constant value state vector on source cells

  double Constant2 = -91.5;
  Jali::UniStateVector<double> myvec2("srccellvars2", source_mesh, nullptr,
                                      Jali::Entity_kind::CELL,
                                      Jali::Entity_type::PARALLEL_OWNED,
                                      Constant2);
  source_state->add(myvec2);

  // Add a constant value state vector on source nodes

  double Constant3 = 3.14;
  Jali::UniStateVector<double> myvec3("srcnodevars", source_mesh, nullptr,
                                      Jali::Entity_kind::NODE,
                                      Jali::Entity_type::PARALLEL_OWNED,
                                      Constant3);
  source_state->add(myvec3);


  // Add zero value state vectors on target cells and nodes - once with
  // the old name and once with the new name

  Jali::UniStateVector<double> myvec4("trgcellvars1", target_mesh, nullptr,
                                      Jali::Entity_kind::CELL,
                                      Jali::Entity_type::PARALLEL_OWNED);
  
  target_state->add(myvec4);
  Jali::UniStateVector<double> myvec5("srccellvars1", target_mesh, nullptr,
                                      Jali::Entity_kind::CELL,
                                      Jali::Entity_type::PARALLEL_OWNED);
  target_state->add(myvec5);

  Jali::UniStateVector<double> myvec6("trgcellvars2", target_mesh, nullptr,
                                      Jali::Entity_kind::CELL,
                                      Jali::Entity_type::PARALLEL_OWNED);
  
  target_state->add(myvec6);
  Jali::UniStateVector<double> myvec7("srccellvars2", target_mesh, nullptr,
                                      Jali::Entity_kind::CELL,
                                      Jali::Entity_type::PARALLEL_OWNED);
  target_state->add(myvec7);

  std::vector<double> zerodata2(nnodes_target, 0.0);
  Jali::UniStateVector<double> myvec8("trgnodevars", target_mesh, nullptr,
                                      Jali::Entity_kind::NODE,
                                      Jali::Entity_type::PARALLEL_OWNED);
  
  target_state->add(myvec8);
  Jali::UniStateVector<double> myvec9("srcnodevars", target_mesh, nullptr,
                                      Jali::Entity_kind::NODE,
                                      Jali::Entity_type::PARALLEL_OWNED);
  target_state->add(myvec9);

  // Wrappers for interfacing with the underlying mesh data structures.

  Wonton::Jali_Mesh_Wrapper sourceMeshWrapper(*source_mesh);
  Wonton::Jali_Mesh_Wrapper targetMeshWrapper(*target_mesh);

  // Wrappers for the source and target state managers

  Wonton::Jali_State_Wrapper sourceStateWrapper(*source_state);
  Wonton::Jali_State_Wrapper targetStateWrapper(*target_state);

  // Build the main driver object

  /////////

  Portage::MMDriver<Portage::SearchKDTree,
                  Portage::IntersectRnD,
                  Portage::Interpolate_1stOrder,
                  2,
                  Wonton::Jali_Mesh_Wrapper,
                  Wonton::Jali_State_Wrapper>
      remapper(sourceMeshWrapper, sourceStateWrapper,
               targetMeshWrapper, targetStateWrapper);
  /////////

  // Specify the fields to be remapped

  std::vector<std::string> source_var_names;
  source_var_names.push_back("srccellvars1");
  source_var_names.push_back("srccellvars2");
  source_var_names.push_back("srcnodevars");

  std::vector<std::string> target_var_names;
  target_var_names.push_back("trgcellvars1");
  target_var_names.push_back("trgcellvars2");
  target_var_names.push_back("trgnodevars");

  remapper.set_remap_var_names(source_var_names, target_var_names);

  // Execute remapper in serial
  Wonton::SerialExecutor_type executor;
  remapper.run(&executor);

  // Verify that we got the fields we wanted

  double *outcellvec1;
  targetStateWrapper.mesh_get_data(Portage::Entity_kind::CELL, "trgcellvars1", &outcellvec1);

  for (int i = 0; i < ncells_target; i++)
    ASSERT_NEAR(Constant1, outcellvec1[i], TOL);

  double *outcellvec2;
  targetStateWrapper.mesh_get_data(Portage::Entity_kind::CELL, "trgcellvars2", &outcellvec2);

  for (int i = 0; i < ncells_target; i++)
    ASSERT_NEAR(Constant2, outcellvec2[i], TOL);

  // double *outnodevec;
  // targetStateWrapper.get_data(Portage::Entity_kind::NODE, "trgnodevars", &outnodevec);

  // for (int i = 0; i < nnodes_target; i++)
  //   ASSERT_NEAR(Constant3, outnodevec[i], TOL);


  // Remap between same name variables

  remapper.set_remap_var_names(source_var_names);

  // Execute remapper in serial

  remapper.run();

  // Verify that we got the fields we wanted

  targetStateWrapper.mesh_get_data(Portage::Entity_kind::CELL, "srccellvars1", &outcellvec1);

  for (int i = 0; i < ncells_target; i++)
    ASSERT_NEAR(Constant1, outcellvec1[i], TOL);

  targetStateWrapper.mesh_get_data(Portage::Entity_kind::CELL, "srccellvars2", &outcellvec2);

  for (int i = 0; i < ncells_target; i++)
    ASSERT_NEAR(Constant2, outcellvec2[i], TOL);

  // targetStateWrapper.mesh_get_data(Portage::Entity_kind::NODE, "srcnodevars", &outnodevec);
  // for (int i = 0; i < ncells_target; i++)
  //   ASSERT_NEAR(Constant3, outnodevec[i], TOL);
}


TEST(Test_MultiVar_Remap, Nested_Meshes) {
  Jali::MeshFactory mf(MPI_COMM_WORLD);
  if (Jali::framework_available(Jali::MSTK))
    mf.framework(Jali::MSTK);
  std::shared_ptr<Jali::Mesh> source_mesh = mf(0.0, 0.0, 1.0, 1.0, 2, 2);
  std::shared_ptr<Jali::Mesh> target_mesh = mf(0.0, 0.0, 1.0, 1.0, 4, 4);

  const int ncells_source =
      source_mesh->num_entities(Jali::Entity_kind::CELL,
                                Jali::Entity_type::PARALLEL_OWNED);
  const int ncells_target =
      target_mesh->num_entities(Jali::Entity_kind::CELL,
                                Jali::Entity_type::PARALLEL_OWNED);

  // Create state objects for source and target mesh

  std::shared_ptr<Jali::State> source_state(Jali::State::create(source_mesh));
  std::shared_ptr<Jali::State> target_state(Jali::State::create(target_mesh));

  // Add a state vector on source cells with values dependent on the
  // centroid of each cell

  Jali::UniStateVector<double> sourcevec("cellvars", source_mesh, nullptr,
                                      Jali::Entity_kind::CELL,
                                      Jali::Entity_type::PARALLEL_OWNED);
  for (int c = 0; c < ncells_source; ++c) {
    JaliGeometry::Point ccen = source_mesh->cell_centroid(c);
    sourcevec[c] = ccen[0] + ccen[1];
  }
  source_state->add(sourcevec);

  // Add zero value state vectors on target cells and nodes - once with
  // the old name and once with the new name

  Jali::UniStateVector<double, Jali::Mesh>& targetvec =
      target_state->add<double, Jali::Mesh, Jali::UniStateVector>("cellvars",
                                  target_mesh,
                                  Jali::Entity_kind::CELL,
                                  Jali::Entity_type::PARALLEL_OWNED);

  // Wrappers for interfacing with the underlying mesh data structures.

  Wonton::Jali_Mesh_Wrapper sourceMeshWrapper(*source_mesh);
  Wonton::Jali_Mesh_Wrapper targetMeshWrapper(*target_mesh);

  // Wrappers for the source and target state managers

  Wonton::Jali_State_Wrapper sourceStateWrapper(*source_state);
  Wonton::Jali_State_Wrapper targetStateWrapper(*target_state);

  // Build the main driver object

  Portage::MMDriver<Portage::SearchKDTree,
                  Portage::IntersectRnD,
                  Portage::Interpolate_1stOrder,
                  2,
                  Wonton::Jali_Mesh_Wrapper,
                  Wonton::Jali_State_Wrapper> remapper1(sourceMeshWrapper,
                                                         sourceStateWrapper,
                                                         targetMeshWrapper,
                                                         targetStateWrapper);

  // Specify the fields to be remapped

  std::vector<std::string> source_var_names;
  source_var_names.push_back("cellvars");

  std::vector<std::string> target_var_names;
  target_var_names.push_back("cellvars");


  remapper1.set_remap_var_names(source_var_names, target_var_names);

  // Execute remapper (No arguments implies serial execution)

  remapper1.run();

  // Verify that we got the fields we wanted
  for (int c = 0; c < ncells_target; c++) {
    JaliGeometry::Point ccen = target_mesh->cell_centroid(c);
    double x, y;
    if (ccen[0] < 0.5) x = 0.25; else x = 0.75;
    if (ccen[1] < 0.5) y = 0.25; else y = 0.75;
    double expval = x + y;

    ASSERT_NEAR(expval, targetvec[c], TOL);
  }


  // Build the main driver object

  Portage::MMDriver<Portage::SearchKDTree,
                  Portage::IntersectRnD,
                  Portage::Interpolate_2ndOrder,
                  2,
                  Wonton::Jali_Mesh_Wrapper,
                  Wonton::Jali_State_Wrapper> remapper2(sourceMeshWrapper,
                                                         sourceStateWrapper,
                                                         targetMeshWrapper,
                                                         targetStateWrapper);


  remapper2.set_remap_var_names(source_var_names, target_var_names);
  remapper2.set_limiter(Portage::NOLIMITER);
  remapper2.set_bnd_limiter(Portage::BND_NOLIMITER);

  // Execute remapper in serial

  remapper2.run();

  // Verify that we got the fields we wanted
  for (int c = 0; c < ncells_target; c++) {
    JaliGeometry::Point ccen = target_mesh->cell_centroid(c);
    double expval = ccen[0] + ccen[1];

    ASSERT_NEAR(expval, targetvec[c], TOL);
  }


}
