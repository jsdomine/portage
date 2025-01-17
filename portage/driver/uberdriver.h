/*
  This file is part of the Ristra portage project.
  Please see the license file at the root of this repository, or at:
  https://github.com/laristra/portage/blob/master/LICENSE
*/

#ifndef PORTAGE_GENREMAP_DRIVER_H_
#define PORTAGE_GENREMAP_DRIVER_H_

#include <sys/time.h>

#include <algorithm>
#include <vector>
#include <iterator>
#include <string>
#include <utility>
#include <iostream>
#include <type_traits>
#include <memory>
#include <limits>

#include "wonton/support/wonton.h"
#include "wonton/mesh/flat/flat_mesh_wrapper.h"
#include "wonton/state/flat/flat_state_mm_wrapper.h"
#include "wonton/support/Point.h"
#include "wonton/state/state_vector_multi.h"

#include "portage/support/portage.h"

#ifdef PORTAGE_HAS_TANGRAM
#include "tangram/driver/driver.h"
#include "tangram/intersect/split_rNd.h"   // included for the easy
                                           // access of Split_RnD,
                                           // Clip_RnD classes by apps
                                           // instantiating this class
#endif

#include "portage/intersect/dummy_interface_reconstructor.h"

#include "portage/search/search_kdtree.h"
#include "portage/search/search_swept_face.h"
#include "portage/intersect/intersect_rNd.h"
#include "portage/intersect/intersect_swept_face.h"
#include "portage/interpolate/interpolate_nth_order.h"
#include "portage/driver/coredriver.h"


#ifdef WONTON_ENABLE_MPI
#include "portage/distributed/mpi_bounding_boxes.h"
#endif

/*!
  @file genremapdriver.h
  @brief Uber remapping driver that does EVERYTHING :-)

  Remap mesh and material variables from mesh to mesh (in serial or
  distributed settings) with the option of doing a part-by-part remap
  (remap between sets of source and target entities)
*/

namespace Portage {

using Wonton::CELL;
using Wonton::NODE;


/*!
  @class UberDriver "driver.h"
  @brief UberDriver provides the API to mapping multi-material data from one mesh to another in a general way

  @tparam SourceMesh A lightweight wrapper to a specific input mesh
  implementation that provides certain functionality.

  @tparam SourceState A lightweight wrapper to a specific input state
  manager implementation that provides certain functionality.

  @tparam TargetMesh A lightweight wrapper to a specific target mesh
  implementation that provides certain functionality.

  @tparam TargetState A lightweight wrapper to a specific target state
  manager implementation that provides certain functionality.

  @tparam InterfaceReconstructorType An interface reconstruction class
  that takes the raw interface reconstruction method, the dimension of
  the problem and the source mesh class as template parameters

  @tparam Matpoly_Splitter A polygon/polyhedron splitting class (returns both pieces of the polygon)

  @tparam Matpoly_Clipper A polygon/polyhedron clipping class (returns only the piece below/behind the clipping plane)

  @tparam CoordinateSystem 
*/
template <int D,
          class SourceMesh, class SourceState,
          class TargetMesh = SourceMesh, class TargetState = SourceState,
          template <class, int, class, class> class InterfaceReconstructorType =
          DummyInterfaceReconstructor,
          class Matpoly_Splitter = void, class Matpoly_Clipper = void,
          class CoordSys = Wonton::DefaultCoordSys
          >
class UberDriver {
  // A couple of shorthand notations
  using NodeRemapper = CoreDriver<D, Wonton::NODE, SourceMesh, SourceState,
                                  TargetMesh, TargetState, InterfaceReconstructorType,
                                  Matpoly_Splitter, Matpoly_Clipper, CoordSys>;

  using CellRemapper = CoreDriver<D, Wonton::CELL, SourceMesh, SourceState,
                                  TargetMesh, TargetState, InterfaceReconstructorType,
                                  Matpoly_Splitter, Matpoly_Clipper, CoordSys>;

public:
  /*!
    @brief Constructor for the remap driver.
    @param[in] source_mesh A @c  wrapper to the source mesh.
    @param[in] source_state A @c wrapper for the data that lives on the
    source mesh
    @param[in] target_mesh A @c TargetMesh to the target mesh
    @param[in,out] target_state A @c TargetState for the data that will
    be mapped to the target mesh
    @param[in] source_vars_to_remap  Optional list of source variables to remap
    (if not everything will be remapped)
    @param[in] executor  pointer to an executor allowing us choose between serial and parallel runs
  */
  UberDriver(SourceMesh const& source_mesh,
              SourceState & source_state,
              TargetMesh const& target_mesh,
              TargetState& target_state,
              std::vector<std::string> const& source_vars_to_remap,
              Wonton::Executor_type const *executor = nullptr)
      : source_mesh_(source_mesh),
        target_mesh_(target_mesh),
        source_state_(source_state),
        target_state_(target_state),
        dim_(source_mesh.space_dimension()),
        source_vars_to_remap_(source_vars_to_remap),
        executor_(executor) {

    assert(source_mesh.space_dimension() == target_mesh.space_dimension());

    // Record all the field types we are remapping and all the kinds
    // of entities we are remapping on
    for (auto const& source_varname : source_vars_to_remap_) {
      Entity_kind onwhat = source_state_.get_entity(source_varname);
      remap_kind_[onwhat] = true;
      Field_type fieldtype = source_state_.field_type(onwhat, source_varname);
      if (not contains(field_types_, fieldtype))
        field_types_.emplace_back(fieldtype);

      if (fieldtype == Field_type::MULTIMATERIAL_FIELD)
        have_multi_material_fields_ = true;
    }
        
    // Make the internal drivers for each entity kind
    
    instantiate_core_drivers();
  }

  /*!
    @brief Constructor for the remap driver.
    @param[in] sourceMesh A @c  wrapper to the source mesh.
    @param[in] sourceState A @c wrapper for the data that lives on the
    source mesh
    @param[in] targetMesh A @c TargetMesh to the target mesh
    @param[in,out] targetState A @c TargetState for the data that will
    be mapped to the target mesh
  */
  UberDriver(SourceMesh const& source_mesh,
              SourceState & source_state,
              TargetMesh const& target_mesh,
              TargetState& target_state,
              Wonton::Executor_type const *executor = nullptr)
      : source_mesh_(source_mesh),
        target_mesh_(target_mesh),
        source_state_(source_state),
        target_state_(target_state),
        dim_(source_mesh.space_dimension()),
        executor_(executor) {

    assert(source_mesh.space_dimension() == target_mesh.space_dimension());

    // if the variables to remap were not listed, assume all variables are
    // to be remapped
    if (source_vars_to_remap_.empty())
      source_vars_to_remap_ = source_state_.names();

    // Record all the field types we are remapping and all the kinds
    // of entities we are remapping on
    for (auto const& source_varname : source_vars_to_remap_) {
      Entity_kind onwhat = source_state_.get_entity(source_varname);
      remap_kind_[onwhat] = true;
      Field_type fieldtype = source_state_.field_type(onwhat, source_varname);
      if (not contains(field_types_, fieldtype))
        field_types_.emplace_back(fieldtype);

      if (fieldtype == Field_type::MULTIMATERIAL_FIELD)
        have_multi_material_fields_ = true;
    }

    // Make the core drivers for each entity kind
    instantiate_core_drivers();
  }


  /// Copy constructor (disabled)
  UberDriver(const UberDriver &) = delete;

  /// Assignment operator (disabled)
  UberDriver & operator = (const UberDriver &) = delete;

  /// Enable move semantics
  UberDriver(UberDriver&&) noexcept = default;

  /// Destructor
  ~UberDriver() = default;

  /// Is this a distributed (multi-rank) run?

  bool is_distributed_run(Wonton::Executor_type const *executor = nullptr) {
    distributed_ = false;

#ifdef WONTON_ENABLE_MPI
    mycomm_ = MPI_COMM_NULL;
    auto mpiexecutor = dynamic_cast<Wonton::MPIExecutor_type const *>(executor);
    if (mpiexecutor && mpiexecutor->mpicomm != MPI_COMM_NULL) {
      mycomm_ = mpiexecutor->mpicomm;
      MPI_Comm_size(mycomm_, &nprocs_);
      if (nprocs_ > 1)
        distributed_ = true;
    }
#endif

    return distributed_;
  }

  /*!
    Does the source mesh need redistribution due to geometric
    mismatch of partitons (different from mismatch of overall domain
    geometry)
  */
  
  bool source_needs_redistribution(Wonton::Executor_type const *executor = nullptr) {
    // for now if it is a distributed run, we always "redistribute"
    // even if that means copying the data into the flat mesh/state
    // but not moving data around. Eventually, we will determine if
    // we need to redistribute based on the partition check and
    // and construct the flat mesh/state wrappers only if we need to

    return is_distributed_run(executor);
  }


  /*! @brief Compute interpolation weights in advance of actual
    interpolation of variables

    @tparam Search A search method that takes the dimension, source
    mesh class and target mesh class as template parameters

    @tparam Intersect A polyhedron-polyhedron intersection class that
    takes the source and taget mesh classes as template parameters
  */

  template<
    template <int, Entity_kind, class, class> class Search,
    template <int, Entity_kind, class, class, class,
              template <class, int, class, class> class,
              class, class> class Intersect
    >
  void compute_interpolation_weights() {
    if (remap_kind_[CELL]) {
      auto const& candidates = search<CELL, Search>();
      source_weights_[CELL] = intersect_meshes<CELL, Intersect>(candidates);

      if (have_multi_material_fields_) {
        source_weights_by_mat_ = intersect_materials<Intersect>(candidates);
      }
    }

    if (remap_kind_[NODE]) {
      auto const& candidates = search<NODE, Search>();
      source_weights_[NODE] = intersect_meshes<NODE, Intersect>(candidates);
    }
  }



  /*!
    @brief Set numerical tolerances for small distances and volumes
    in core driver

    @tparam Entity_kind  what kind of entity are we setting for

    @param min_absolute_distance selected minimal distance

    @param min_absolute_volume selected minimal volume
  */
  void set_num_tols(double min_absolute_distance, double min_absolute_volume) {
    if (remap_kind_[CELL])
      driver_cell_->set_num_tols(min_absolute_distance, min_absolute_volume);
    if (remap_kind_[NODE])
      driver_node_->set_num_tols(min_absolute_distance, min_absolute_volume);
  }
  
  /*!
    @brief set numerical tolerances in core driver

    @tparam Entity_kind  what kind of entity are we setting for

    @param num_tols     struct of selected numerical tolerances
  */
  void set_num_tols(const NumericTolerances_t& num_tols) {
    if (remap_kind_[CELL]) driver_cell_->set_num_tols(num_tols);
    if (remap_kind_[NODE]) driver_node_->set_num_tols(num_tols);
  }

  /*!
    @brief set flag whether we want to check for mesh mismatch

    @param do_check_mismatch  boolean flag indicating if we want to check for
                              boundary mismatch between meshes

    This check is used to determine if the boundaries of the two
    meshes overlap exactly. If they don't conservation is
    violated. Callers can ask to compensate for the mismatch when
    interpolating a mesh variable.
  */
  void set_check_mismatch_flag(const bool do_check_mismatch) {
    do_check_mismatch_ = do_check_mismatch;
  }
  
  /*!
    @brief search for candidate source entities whose control volumes
     (cells, dual cells) overlap the control volumes of target cells

     @tparam Entity_kind  what kind of entity are we searching on/for

     @tparam Search       search functor

     @returns    vector of candidate cells for each target cell
  */

  template<
    Entity_kind ONWHAT,
    template <int, Entity_kind, class, class> class Search
    >
  Wonton::vector<std::vector<int>> search() {
    search_completed_[ONWHAT] = true;
    switch (ONWHAT) {
      case CELL: return driver_cell_->template search<Search>();
      case NODE: return driver_node_->template search<Search>();
      default: throw std::runtime_error("unsupported field type");
    }
  }


  /*!
    @brief intersect target control volumes with source control volumes

     @tparam Entity_kind  what kind of entities are we intersecting
 
     @tparam Intersect    intersect functor

     @param[in] candidates Intersection candidates for each target cell

     @returns             vector of weights for each target cell
  */

  template<
    Entity_kind ONWHAT,
    template <int, Entity_kind, class, class, class,
              template <class, int, class, class> class,
              class, class> class Intersect
    >
  Wonton::vector<std::vector<Portage::Weights_t>>         // return type
  intersect_meshes(Wonton::vector<std::vector<int>> const& candidates) {
    switch (ONWHAT) {
      case CELL: {
        auto const& weights = driver_cell_->template intersect_meshes<Intersect>(candidates);
        // Check the mesh mismatch once, to make sure the mismatch is cached
        // prior to interpolation with fixup. This is the correct place to automatically do the
        // check because it reqires the intersection weights which were just computed.
        if (do_check_mismatch_) driver_cell_->check_mismatch(weights);
        mesh_intersection_completed_[CELL] = true;
        return weights;
      }
      case NODE: {
        auto const& weights = driver_node_->template intersect_meshes<Intersect>(candidates);
        // Check the mesh mismatch once, to make sure the mismatch is cached
        // prior to interpolation with fixup. This is the correct place to automatically do the
        // check because it reqires the intersection weights which were just computed.
        if (do_check_mismatch_) driver_node_->check_mismatch(weights);
        mesh_intersection_completed_[NODE] = true;
        return weights;
      } break;
      default: throw std::runtime_error("unsupported field type");
    }
  }

#ifdef PORTAGE_HAS_TANGRAM
  /*!
    @brief set options for interface reconstructor driver
    @param all_convex Should be set to false if the source mesh contains
    non-convex cells.
    @param tols The vector of tolerances for each moment during reconstruction.
    By default, the values are chosen based on tolerances specified for Portage
    in NumericTolerances_t struct. If both the tolerances for Portage and for
    Tangram are explicitly set by a user, they need to make sure that selected
    values are synced. If only the tolerances for Tangram are set by a user,
    then values in Portage's NumericTolerances_t are set based on the tols
    argument.
  */
  void set_interface_reconstructor_options(bool all_convex,
                                           const std::vector<Tangram::IterativeMethodTolerances_t> &tols = {}) {
    assert(remap_kind_[CELL]);
    assert(have_multi_material_fields_);
    driver_cell_->set_interface_reconstructor_options(all_convex, tols);
  }

#endif

  

  /* @brief intersect target cells with source material polygons

     @tparam Entity_kind  what kind of entities are we intersecting

     @tparam Intersect    intersect functor

     @param[in] candidates intersection candidates for each target cells

     @returns vector(s) of weights for each target cell organized by
     material (hence the additional outer std::vector compared to the
     return type of intersect_meshes)
  */

  template<
    template <int, Entity_kind, class, class, class,
              template <class, int, class, class> class,
              class, class> class Intersect
    >
  std::vector<Wonton::vector<std::vector<Portage::Weights_t>>>
  intersect_materials(Wonton::vector<std::vector<int>> const& candidates) {
    assert(remap_kind_[CELL]);
    mat_intersection_completed_ = true;
    return driver_cell_->template intersect_materials<Intersect>(candidates);
  }
  
 
  /*!
    Interpolate a mesh variable of type T residing on entity kind
    ONWHAT using previously computed intersection weights (same
    variable name on source and target)

    @tparam T  type of variable being remapped (default double) - underlying
    interpolator must be able to handle this type

    @tparam ONWHAT  Entity kind on which variable resides

    @tparam Interpolate  Interpolate functor to do the actual interpolation

    @param[in] srcvarname   Variable name on source and target meshes

    @param[in] lower_bound  Lower bound for variable

    @param[in] upper_bound  Upper bound for variable

    @param[in] limiter      Limiter to use for second order reconstruction

    @param[in] bnd_limiter  Boundary limiter to use for second order reconstruction

    @param[in] partial_fixup_type  Method to populate fields on partially filled target entities (cells or dual cells)

    @param[in] empty_fixup_type    Method to populate fields on empty target entities (cells or dual cells)

    @param[in] conservation_tol   Tolerance to which source and target integral quantities are to be matched

    @param[in] max_fixup_iter     Max number of iterations for global repair

    See support/portage.h for options on limiter, partial_fixup_type and
    empty_fixup_type
  */

  template<typename T = double,
           Entity_kind ONWHAT,
           template<int, Entity_kind, class, class, class, class, class,
                    template<class, int, class, class> class,
                    class, class, class> class Interpolate
           >
  void interpolate(std::string srcvarname,
                   T lower_bound, T upper_bound,
                   Limiter_type limiter = DEFAULT_LIMITER,
                   Boundary_Limiter_type bnd_limiter = DEFAULT_BND_LIMITER,
                   Partial_fixup_type partial_fixup_type = DEFAULT_PARTIAL_FIXUP_TYPE,
                   Empty_fixup_type empty_fixup_type = DEFAULT_EMPTY_FIXUP_TYPE,
                   double conservation_tol = DEFAULT_NUMERIC_TOLERANCES<D>.relative_conservation_eps,
                   int max_fixup_iter = DEFAULT_NUMERIC_TOLERANCES<D>.max_num_fixup_iter) {

    interpolate<T, ONWHAT, Interpolate>(srcvarname, srcvarname,
                                        lower_bound, upper_bound,
                                        limiter, bnd_limiter,
                                        partial_fixup_type, empty_fixup_type,
                                        conservation_tol, max_fixup_iter);
  }

  /*!
    Interpolate a mesh variable of type T residing on entity kind
    ONWHAT using previously computed intersection weights (different
    variable name on source and target)

    @tparam T  type of variable being remapped (default double) - underlying
    interpolator must be able to handle this type

    @param[in] srcvarname   Variable name on source mesh

    @param[in] trgvarname   Variable name on target mesh

    @param[in] lower_bound  Lower bound for variable

    @param[in] upper_bound  Upper bound for variable

    @param[in] limiter      Limiter to use for second order reconstruction

    @param[in] bnd_limiter  Boundary limiter to use for second order reconstruction

    @param[in] partial_fixup_type  Method to populate fields on partially filled target entities (cells or dual cells)

    @param[in] empty_fixup_type    Method to populate fields on empty target entities (cells or dual cells)

    @param[in] conservation_tol   Tolerance to which source and target integral quantities are to be matched

    @param[in] max_fixup_iter     Max number of iterations for global repair

    See support/portage.h for options on limiter, partial_fixup_type and
    empty_fixup_type
  */

  template<typename T = double,
           Entity_kind ONWHAT,
           template<int, Entity_kind, class, class, class, class, class,
                    template<class, int, class, class> class,
                    class, class, class> class Interpolate
           >
  void interpolate(std::string srcvarname, std::string trgvarname,
                   T lower_bound, T upper_bound,
                   Limiter_type limiter = DEFAULT_LIMITER,
                   Boundary_Limiter_type bnd_limiter = DEFAULT_BND_LIMITER,
                   Partial_fixup_type partial_fixup_type = DEFAULT_PARTIAL_FIXUP_TYPE,
                   Empty_fixup_type empty_fixup_type = DEFAULT_EMPTY_FIXUP_TYPE,
                   double conservation_tol = DEFAULT_NUMERIC_TOLERANCES<D>.relative_conservation_eps,
                   int max_fixup_iter = DEFAULT_NUMERIC_TOLERANCES<D>.max_num_fixup_iter) {
    
    assert(source_state_.get_entity(srcvarname) == ONWHAT);
    assert(mesh_intersection_completed_[ONWHAT]);

    if (not contains(source_vars_to_remap_, srcvarname)) {
      throw std::runtime_error(srcvarname + " not in field variables list");
    }

    if (source_state_.field_type(ONWHAT, srcvarname) == Field_type::MULTIMATERIAL_FIELD) {

#ifdef PORTAGE_HAS_TANGRAM
      assert(mat_intersection_completed_);
      assert(ONWHAT == CELL);
      
      interpolate_mat_var<T, Interpolate>
          (srcvarname, trgvarname, source_weights_by_mat_,
           lower_bound, upper_bound, limiter, bnd_limiter, partial_fixup_type,
           empty_fixup_type, conservation_tol, max_fixup_iter);
#endif

    } else {

      assert(mesh_intersection_completed_[ONWHAT]);
      
      interpolate_mesh_var<T, ONWHAT, Interpolate>
          (srcvarname, trgvarname, source_weights_[ONWHAT],
           lower_bound, upper_bound, limiter, bnd_limiter, partial_fixup_type,
           empty_fixup_type, conservation_tol, max_fixup_iter);
    }

  }  // interpolate

  /*!
    Interpolate a mesh variable of type T residing on entity kind ONWHAT using
    previously computed intersection weights
    
    @tparam T   type of variable
    
    @tparam ONWHAT  Entity_kind that field resides on

    @tparam Interpolate  Functor for doing the interpolate from mesh to mesh

    @param[in] srcvarname   Variable name on source mesh

    @param[in] trgvarname   Variable name on target mesh

    @param[in] lower_bound  Lower bound for variable

    @param[in] upper_bound  Upper bound for variable

    @param[in] limiter      Limiter to use for second order reconstruction

    @param[in] bnd_limiter  Boundary limiter to use for second order reconstruction

    @param[in] partial_fixup_type Method to populate fields on
    partially filled target entities (cells or dual cells)

    @param[in] empty_fixup_type Method to populate fields on empty
    target entities (cells or dual cells)

    @param[in] conservation_tol Tolerance to which source and target
    integral quantities are to be matched

    @param[in] max_fixup_iter     Max number of iterations for global repair

    See support/portage.h for options on limiter, partial_fixup_type and
    empty_fixup_type
    
    Since this call explicitly takes intersection weights we don't have to
    check if intersection step is complete
  */
  
  template<typename T = double,
           Entity_kind ONWHAT,
           template<int, Entity_kind, class, class, class, class, class,
                    template <class, int, class, class> class,
                    class, class, class> class Interpolate
           >
  void interpolate_mesh_var(std::string srcvarname, std::string trgvarname,
                            Wonton::vector<std::vector<Weights_t>> const& sources_and_weights_in,
                            T lower_bound, T upper_bound,
                            Limiter_type limiter,
                            Boundary_Limiter_type bnd_limiter,
                            Partial_fixup_type partial_fixup_type,
                            Empty_fixup_type empty_fixup_type,
                            double conservation_tol,
                            int max_fixup_iter) {

    assert(source_state_.get_entity(srcvarname) == ONWHAT);
    if (not contains(source_vars_to_remap_, srcvarname)) {
      throw std::runtime_error(srcvarname + " not in field variables list");
    }

    using Interpolator = Interpolate<D, ONWHAT,
                                     SourceMesh, TargetMesh,
                                     SourceState, TargetState,
                                     T,
                                     InterfaceReconstructorType,
                                     Matpoly_Splitter, Matpoly_Clipper,
                                     CoordSys>;

    switch (ONWHAT) {
      case CELL: {
        if (Interpolator::order == 2) {
          auto gradients =
            driver_cell_->compute_source_gradient(srcvarname, limiter, bnd_limiter);

          driver_cell_->template interpolate_mesh_var<T, Interpolate>(
            srcvarname, trgvarname, sources_and_weights_in, &gradients
          );
        } else {
          driver_cell_->template interpolate_mesh_var<T, Interpolate>(
            srcvarname, trgvarname, sources_and_weights_in
          );
        }

        if (do_check_mismatch_ && driver_cell_->has_mismatch()) {
          driver_cell_->fix_mismatch(srcvarname, trgvarname,
                                     lower_bound, upper_bound,
                                     conservation_tol, max_fixup_iter,
                                     partial_fixup_type, empty_fixup_type);
        }
      } break;
      case NODE: {
        if (Interpolator::order == 2) {
          auto gradients =
            driver_node_->compute_source_gradient(srcvarname, limiter, bnd_limiter);

          driver_node_->template interpolate_mesh_var<T, Interpolate>(
            srcvarname, trgvarname, sources_and_weights_in, &gradients
          );
        } else {
          driver_node_->template interpolate_mesh_var<T, Interpolate>(
            srcvarname, trgvarname, sources_and_weights_in
          );
        }

        if (do_check_mismatch_ && driver_node_->has_mismatch()) {
          driver_node_->fix_mismatch(srcvarname, trgvarname,
                                     lower_bound, upper_bound,
                                     conservation_tol, max_fixup_iter,
                                     partial_fixup_type, empty_fixup_type);
        }
      } break;
      default: break;
    }
  }

  /*!
    Interpolate a (multi-)material variable of type T residing on CELLs
    
    @param[in] srcvarname   Variable name on source mesh

    @param[in] trgvarname   Variable name on target mesh

    @param[in] lower_bound  Lower bound for variable

    @param[in] upper_bound  Upper bound for variable

    @param[in] limiter      Limiter to use for second order reconstruction

    @param[in] bnd_limiter  Boundary limiter to use for second order reconstruction

    @param[in] partial_fixup_type Method to populate fields on
    partially filled target entities (cells or dual cells)

    @param[in] empty_fixup_type Method to populate fields on empty
    target entities (cells or dual cells)

    @param[in] conservation_tol Tolerance to which source and target
    integral quantities are to be matched

    @param[in] max_fixup_iter     Max number of iterations for global repair

    See support/portage.h for options on limiter, partial_fixup_type and
    empty_fixup_type
      
    Since this call explicitly takes intersection weights we don't have to
    check if intersection step is complete
  */
  
  template <typename T = double,
            template<int, Entity_kind, class, class, class, class, class,
                     template <class, int, class, class> class,
                     class, class, class> class Interpolate
            >
  void interpolate_mat_var(std::string srcvarname, std::string trgvarname,
                           std::vector<Wonton::vector<std::vector<Weights_t>>> const& sources_and_weights_by_mat_in,
                           T/* unused */, T/* unused */,
                           Limiter_type limiter,
                           Boundary_Limiter_type bnd_limiter,
                           Partial_fixup_type/* unused */,
                           Empty_fixup_type/* unused */,
                           double/* unused */,
                           int/* unused */) {

    assert(source_state_.get_entity(srcvarname) == CELL);

    if (not contains(source_vars_to_remap_, srcvarname)) {
      throw std::runtime_error(srcvarname + " not in field variables list");
    }

#ifdef PORTAGE_HAS_TANGRAM
    using Interpolator = Interpolate<D, CELL,
                                     SourceMesh, TargetMesh,
                                     SourceState, TargetState,
                                     T, InterfaceReconstructorType,
                                     Matpoly_Splitter, Matpoly_Clipper, CoordSys>;

    int const nb_mats = source_state_.num_materials();
    assert(nb_mats > 0);

    if (Interpolator::order == 2) {
      // cache gradient stencils first
      driver_cell_->cache_multimat_gradient_stencils();

      std::vector<Wonton::vector<Vector<D>>> gradients(nb_mats);
      for (int i = 0; i < nb_mats; ++i) {
        gradients[i] = driver_cell_->compute_source_gradient(srcvarname, limiter,
                                                             bnd_limiter, i);
      }
      driver_cell_->template interpolate_mat_var<T, Interpolate>(
        srcvarname, trgvarname, sources_and_weights_by_mat_in, &gradients
      );
    } else {
      driver_cell_->template interpolate_mat_var<T, Interpolate>(
        srcvarname, trgvarname, sources_and_weights_by_mat_in
      );
    }
#endif
  }
  
 private:

  // Inputs specified by calling app
  SourceMesh const& source_mesh_;
  TargetMesh const& target_mesh_;
  SourceState& source_state_;
  TargetState& target_state_;
  int dim_ = 0;

  // Component variables
  bool distributed_ = false;  // default is serial
  Wonton::Executor_type const *executor_;
#ifdef WONTON_ENABLE_MPI
  int nprocs_ = 1;
  MPI_Comm mycomm_ = MPI_COMM_NULL;
#endif

  std::vector<std::string> source_vars_to_remap_ {};
  std::vector<Field_type> field_types_ {};
  std::map<Entity_kind, bool> remap_kind_ {{CELL, false}, {NODE, false}};

  // Whether we are remapping multimaterial fields
  bool have_multi_material_fields_ = false;

  // Whether to do mismatch check or not. Checks exact overlap between
  // source and target boundaries if true. Callers can ask to
  // compensate for the mismatch during the interpolate call if this
  // check is performed and mismatch is detected
  bool do_check_mismatch_ = true;
  
  // Track what steps are completed
  std::map<Entity_kind, bool> search_completed_ {};
  std::map<Entity_kind, bool> mesh_intersection_completed_ {};
  bool mat_intersection_completed_ = false;

  // Pointers to core drivers designed to work on a particular
  // entity kind on native mesh/state. These work for serial runs, or
  // parallel runs where the distribution via flat mesh/state has already
  // occurred.
  std::unique_ptr<NodeRemapper> driver_node_ {};
  std::unique_ptr<CellRemapper> driver_cell_ {};

  // Weights of intersection b/w target entities and source entities
  // Each intersection is between the control volume (cell, dual cell)
  // of a target and source entity.

  //  over all entity kinds (CELL, NODE, etc.)
  //   ||
  //   ||               for all target entities
  //   ||                  of a particular kind
  //   ||                        ||
  //   ||                        ||     intersection moments list
  //   ||                        ||     for each target entity
  //   ||                        ||           ||
  //   \/                        \/           \/
  std::map<Entity_kind, Wonton::vector<std::vector<Weights_t>>> source_weights_ {};

  // Weights of intersection b/w target CELLS and source material polygons
  // Each intersection is between a target cell and material polygon in
  // a source cell for a particular material

  //  for each material
  //   ||
  //   ||      for all target entities
  //   ||         of a particular kind
  //   ||               ||
  //   ||               ||     intersection moments for
  //   ||               ||     each target entity
  //   ||               ||           ||
  //   \/               \/           \/
  std::vector<Wonton::vector<std::vector<Weights_t>>> source_weights_by_mat_ {};

  /*!
    @brief Instantiate core drivers that abstract away whether we
    are using a redistributed or native source mesh/state

    executor  An executor encoding parallel run parameters (if its parallel executor)
  */

  void instantiate_core_drivers() {

    if (remap_kind_[NODE]) {
      search_completed_[NODE] = false;
      mesh_intersection_completed_[NODE] = false;
      auto driver_node = new NodeRemapper(source_mesh_, source_state_,
                                          target_mesh_, target_state_, executor_);
      driver_node_ = std::unique_ptr<NodeRemapper>(driver_node);
    }

    if (remap_kind_[CELL]) {
      search_completed_[CELL] = false;
      mesh_intersection_completed_[CELL] = false;
      auto driver_cell = new CellRemapper(source_mesh_, source_state_,
                                          target_mesh_, target_state_, executor_);
      driver_cell_ = std::unique_ptr<CellRemapper>(driver_cell);
    }
  }  // UberDriver::instantiate_core_drivers

};  // UberDriver


}  // namespace Portage

#endif  // PORTAGE_GENREMAP_DRIVER_H_
