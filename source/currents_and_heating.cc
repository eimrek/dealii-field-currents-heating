/*
 * currents_and_heating.cc
 *
 *  Created on: Jul 28, 2016
 *      Author: kristjan
 */

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>

#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/dofs/dof_renumbering.h>

#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/fe_dgq.h>

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/logstream.h>
#include <deal.II/base/timer.h>

#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/data_out.h>

#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/solver_gmres.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/sparse_direct.h>	// UMFpack

#include <cassert>
#include <algorithm>

#include "currents_and_heating.h"
#include "utility.h"

namespace fch {
using namespace dealii;

template<int dim>
CurrentsAndHeating<dim>::CurrentsAndHeating() :
        time_step(1e-13), uniform_efield_bc(1.0),
        fe_current(currents_degree), dof_handler_current(triangulation),
        fe_heat(heating_degree), dof_handler_heat(triangulation), pq(NULL) {
}

template<int dim>
CurrentsAndHeating<dim>::CurrentsAndHeating(double time_step_, PhysicalQuantities *pq_) :
        time_step(time_step_), uniform_efield_bc(1.0),
        fe_current(currents_degree), dof_handler_current(triangulation),
        fe_heat(heating_degree), dof_handler_heat(triangulation), pq(pq_) {
}

template<int dim>
void CurrentsAndHeating<dim>::import_mesh_from_file(const std::string file_name) {
    MeshPreparer<dim> mesh_preparer;

    mesh_preparer.import_mesh_from_file(&triangulation, file_name);
    mesh_preparer.mark_copper_boundary(&triangulation);
}

template<int dim>
bool CurrentsAndHeating<dim>::import_mesh_directly(std::vector<Point<dim> > vertices,
        std::vector<CellData<dim> > cells) {
    try {
        SubCellData subcelldata;
        // Do some clean-up on vertices...
        GridTools::delete_unused_vertices(vertices, cells, subcelldata);
        // ... and on cells
        GridReordering<dim, dim>::invert_all_cells_of_negative_grid(vertices, cells);
        triangulation.create_triangulation_compatibility(vertices, cells, SubCellData());
    } catch (exception &exc) {
        return false;
    }
    MeshPreparer<dim> mesh_preparer;
    mesh_preparer.mark_copper_boundary(&triangulation);
    return true;
}


template<int dim>
void CurrentsAndHeating<dim>::setup_current_system() {

    dof_handler_current.distribute_dofs(fe_current);
    //std::cout << "Number of degrees of freedom: " << dof_handler.n_dofs()
    //      << std::endl;

    DynamicSparsityPattern dsp(dof_handler_current.n_dofs());
    DoFTools::make_sparsity_pattern(dof_handler_current, dsp);
    sparsity_pattern_current.copy_from(dsp);

    system_matrix_current.reinit(sparsity_pattern_current);

    solution_current.reinit(dof_handler_current.n_dofs());
    old_solution_current.reinit(dof_handler_current.n_dofs());
    system_rhs_current.reinit(dof_handler_current.n_dofs());

    for (std::size_t i = 0; i < solution_current.size(); i++) {
        solution_current[i] = 0;
        old_solution_current[i] = 0;
    }
}

template<int dim>
void CurrentsAndHeating<dim>::setup_heating_system() {

    dof_handler_heat.distribute_dofs(fe_heat);
    //std::cout << "Number of degrees of freedom: " << dof_handler.n_dofs()
    //      << std::endl;

    DynamicSparsityPattern dsp(dof_handler_heat.n_dofs());
    DoFTools::make_sparsity_pattern(dof_handler_heat, dsp);
    sparsity_pattern_heat.copy_from(dsp);

    system_matrix_heat.reinit(sparsity_pattern_heat);

    solution_heat.reinit(dof_handler_heat.n_dofs());
    old_solution_heat.reinit(dof_handler_heat.n_dofs());
    system_rhs_heat.reinit(dof_handler_heat.n_dofs());

    const_temperature_solution.reinit(dof_handler_heat.n_dofs());

    // Initialize the solution to ambient temperature
    for (std::size_t i = 0; i < solution_heat.size(); i++) {
        solution_heat[i] = ambient_temperature;
        old_solution_heat[i] = ambient_temperature;
        const_temperature_solution[i] = 1000;
    }
}

template<int dim>
void CurrentsAndHeating<dim>::assemble_current_system() {
    QGauss<dim> quadrature_formula(currents_degree+1);
    QGauss<dim-1> face_quadrature_formula(currents_degree+1);

    // Current finite element values
    FEValues<dim> fe_values(fe_current, quadrature_formula,
            update_gradients | update_quadrature_points | update_JxW_values);
    FEFaceValues<dim> fe_face_values(fe_current, face_quadrature_formula,
                update_values | update_quadrature_points | update_JxW_values);

    // Temperature finite element values (only for accessing previous iteration solution)
    FEValues<dim> fe_values_heat(fe_heat, quadrature_formula,
            update_values);
    FEFaceValues<dim> fe_face_values_heat(fe_heat, face_quadrature_formula,
                update_values);

    const unsigned int dofs_per_cell = fe_current.dofs_per_cell;
    const unsigned int n_q_points = quadrature_formula.size();
    const unsigned int n_face_q_points = face_quadrature_formula.size();

    FullMatrix<double> cell_matrix(dofs_per_cell, dofs_per_cell);
    Vector<double> cell_rhs(dofs_per_cell);

    std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

    // ---------------------------------------------------------------------------------------------
    // The previous solution values in the cell quadrature points
    std::vector<double> prev_sol_temperature_values(n_q_points);
    // The previous solution values in the face quadrature points
    std::vector<double> prev_sol_face_temperature_values(n_face_q_points);
    // ---------------------------------------------------------------------------------------------

    typename DoFHandler<dim>::active_cell_iterator cell = dof_handler_current.begin_active(),
            endc = dof_handler_current.end();
    typename DoFHandler<dim>::active_cell_iterator heat_cell = dof_handler_heat.begin_active();

    for (; cell != endc; ++cell, ++heat_cell) {
        fe_values.reinit(cell);
        cell_matrix = 0;
        cell_rhs = 0;

        fe_values_heat.reinit(heat_cell);
        fe_values_heat.get_function_values(solution_heat, prev_sol_temperature_values);

        // ----------------------------------------------------------------------------------------
        // Local matrix assembly
        // ----------------------------------------------------------------------------------------
        for (unsigned int q = 0; q < n_q_points; ++q) {

            double temperature = prev_sol_temperature_values[q];
            double sigma = pq->sigma(temperature);

            for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                for (unsigned int j = 0; j < dofs_per_cell; ++j)
                    cell_matrix(i, j) += (fe_values.shape_grad(i, q) * fe_values.shape_grad(j, q)
                            * sigma * fe_values.JxW(q));
            }
        }
        // ----------------------------------------------------------------------------------------
        // Local right-hand side assembly
        // ----------------------------------------------------------------------------------------
        // Emission current BC at the copper surface
        for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; ++f) {
            if (cell->face(f)->at_boundary() && cell->face(f)->boundary_id() == BoundaryId::copper_surface) {
                fe_face_values.reinit(cell, f);

                fe_face_values_heat.reinit(heat_cell, f);
                fe_face_values_heat.get_function_values(solution_heat, prev_sol_face_temperature_values);

                // ----------------------------------------------------------------------------------
                // Cell and face info
                std::pair<unsigned, unsigned> cop_cell_info = std::pair<unsigned, unsigned>(
                                                              cell->index(), f);
                // ----------------------------------------------------------------------------------

                for (unsigned int q = 0; q < n_face_q_points; ++q) {

                    double temperature = prev_sol_face_temperature_values[q];
                    double emission_current = get_emission_current_bc(cop_cell_info, temperature);

                    for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                        cell_rhs(i) += (fe_face_values.shape_value(i, q)
                                * emission_current * fe_face_values.JxW(q));
                    }
                }
            }
        }

        cell->get_dof_indices(local_dof_indices);
        for (unsigned int i = 0; i < dofs_per_cell; ++i) {
            for (unsigned int j = 0; j < dofs_per_cell; ++j)
                system_matrix_current.add(local_dof_indices[i], local_dof_indices[j], cell_matrix(i, j));

            system_rhs_current(local_dof_indices[i]) += cell_rhs(i);
        }
    }

    std::map<types::global_dof_index, double> boundary_values;
    VectorTools::interpolate_boundary_values(dof_handler_current, BoundaryId::copper_bottom,
            ZeroFunction<dim>(), boundary_values);
    MatrixTools::apply_boundary_values(boundary_values, system_matrix_current, solution_current, system_rhs_current);
}


template<int dim>
void CurrentsAndHeating<dim>::assemble_heating_system_crank_nicolson() {

    const double const_k = time_step/(2.0*cu_rho_cp);

    double max_heating_power = 0.0;

    QGauss<dim> quadrature_formula(heating_degree+1);
    QGauss<dim-1> face_quadrature_formula(heating_degree+1);

    // Heating finite element values
    FEValues<dim> fe_values(fe_heat, quadrature_formula,
            update_values | update_gradients
            | update_quadrature_points | update_JxW_values);
    FEFaceValues<dim> fe_face_values(fe_heat, face_quadrature_formula,
                update_values | update_quadrature_points | update_JxW_values);

    // Finite element values for accessing current calculation
    FEValues<dim> fe_values_current(fe_current, quadrature_formula,
            update_gradients);

    const unsigned int dofs_per_cell = fe_heat.dofs_per_cell;
    const unsigned int n_q_points = quadrature_formula.size();
    const unsigned int n_face_q_points = face_quadrature_formula.size();

    FullMatrix<double> cell_matrix(dofs_per_cell, dofs_per_cell);
    Vector<double> cell_rhs(dofs_per_cell);

    std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

    // ---------------------------------------------------------------------------------------------
    // The other solution values in the cell quadrature points
    std::vector<Tensor<1, dim>> potential_gradients(n_q_points);
    std::vector<Tensor<1, dim>> prev_sol_potential_gradients(n_q_points);
    std::vector<double> prev_sol_temperature_values(n_q_points);
    std::vector<Tensor<1, dim>> prev_sol_temperature_gradients(n_q_points);

    std::vector<double> prev_sol_face_temperature_values(n_face_q_points);
    // ---------------------------------------------------------------------------------------------

    typename DoFHandler<dim>::active_cell_iterator cell = dof_handler_heat.begin_active(),
            endc = dof_handler_heat.end();
    typename DoFHandler<dim>::active_cell_iterator current_cell = dof_handler_current.begin_active();

    for (; cell != endc; ++cell, ++current_cell) {
        fe_values.reinit(cell);
        cell_matrix = 0;
        cell_rhs = 0;

        fe_values.get_function_values(old_solution_heat, prev_sol_temperature_values);
        fe_values.get_function_gradients(old_solution_heat, prev_sol_temperature_gradients);

        fe_values_current.reinit(current_cell);
        fe_values_current.get_function_gradients(solution_current, potential_gradients);
        fe_values_current.get_function_gradients(old_solution_current, prev_sol_potential_gradients);

        // ----------------------------------------------------------------------------------------
        // Local matrix assembly
        // ----------------------------------------------------------------------------------------
        for (unsigned int q = 0; q < n_q_points; ++q) {

            double prev_temperature = prev_sol_temperature_values[q];
            double kappa = pq->kappa(prev_temperature);
            double sigma = pq->sigma(prev_temperature);

            Tensor<1, dim> prev_temperature_grad = prev_sol_temperature_gradients[q];

            double pot_grad_squared = potential_gradients[q].norm_square();
            double prev_pot_grad_squared = prev_sol_potential_gradients[q].norm_square();

            for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                for (unsigned int j = 0; j < dofs_per_cell; ++j) {
                    cell_matrix(i, j) += (
                            fe_values.shape_value(i, q) * fe_values.shape_value(j, q) // Mass matrix
                            + const_k*kappa*fe_values.shape_grad(i, q) * fe_values.shape_grad(j, q)
                    ) * fe_values.JxW(q);
                }
                cell_rhs(i) += (
                        fe_values.shape_value(i, q)*prev_temperature
                        + const_k*fe_values.shape_value(i, q)*sigma*(pot_grad_squared+prev_pot_grad_squared)
                        - const_k*kappa*fe_values.shape_grad(i, q)*prev_temperature_grad
                ) * fe_values.JxW(q);
                if (1/2.0*sigma*(pot_grad_squared+prev_pot_grad_squared) > max_heating_power) {
                    max_heating_power = 1/2.0*sigma*(pot_grad_squared+prev_pot_grad_squared);
                }
            }
        }
        // ----------------------------------------------------------------------------------------
        // Local right-hand side assembly
        // ----------------------------------------------------------------------------------------
        // Nottingham BC at the copper surface
        for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; ++f) {
            if (cell->face(f)->at_boundary() && cell->face(f)->boundary_id() == BoundaryId::copper_surface) {
                fe_face_values.reinit(cell, f);

                fe_face_values.get_function_values(old_solution_heat, prev_sol_face_temperature_values);

                // ----------------------------------------------------------------------------------
                // Cell & face info
                std::pair<unsigned, unsigned> cop_cell_info = std::pair<unsigned, unsigned>(
                                                              cell->index(), f);
                // ----------------------------------------------------------------------------------

                for (unsigned int q = 0; q < n_face_q_points; ++q) {

                    double prev_temperature = prev_sol_face_temperature_values[q];
                    double nottingham_heat = get_nottingham_heat_bc(cop_cell_info, prev_temperature);

                    //nottingham_heat = 0.0;
                    for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                        cell_rhs(i) += (const_k * fe_face_values.shape_value(i, q)
                                * 2.0 * nottingham_heat * fe_face_values.JxW(q));
                    }
                }
            }
        }

        cell->get_dof_indices(local_dof_indices);
        for (unsigned int i = 0; i < dofs_per_cell; ++i) {
            for (unsigned int j = 0; j < dofs_per_cell; ++j)
                system_matrix_heat.add(local_dof_indices[i], local_dof_indices[j], cell_matrix(i, j));

            system_rhs_heat(local_dof_indices[i]) += cell_rhs(i);
        }
    }

    //std::cout << "Maximum heating power in system: " << max_heating_power << std::endl;

    std::map<types::global_dof_index, double> boundary_values;
    VectorTools::interpolate_boundary_values(dof_handler_heat, BoundaryId::copper_bottom,
            ConstantFunction<dim>(ambient_temperature), boundary_values);
    MatrixTools::apply_boundary_values(boundary_values, system_matrix_heat, solution_heat, system_rhs_heat);
}


template<int dim>
void CurrentsAndHeating<dim>::assemble_heating_system_euler_implicit() {

    const double gamma = time_step/(cu_rho_cp);

    double max_heating_power = 0.0;

    QGauss<dim> quadrature_formula(heating_degree+1);
    QGauss<dim-1> face_quadrature_formula(heating_degree+1);

    // Heating finite element values
    FEValues<dim> fe_values(fe_heat, quadrature_formula,
            update_values | update_gradients
            | update_quadrature_points | update_JxW_values);
    FEFaceValues<dim> fe_face_values(fe_heat, face_quadrature_formula,
                update_values | update_quadrature_points | update_JxW_values);

    // Finite element values for accessing current calculation
    FEValues<dim> fe_values_current(fe_current, quadrature_formula,
            update_gradients);

    const unsigned int dofs_per_cell = fe_heat.dofs_per_cell;
    const unsigned int n_q_points = quadrature_formula.size();
    const unsigned int n_face_q_points = face_quadrature_formula.size();

    FullMatrix<double> cell_matrix(dofs_per_cell, dofs_per_cell);
    Vector<double> cell_rhs(dofs_per_cell);

    std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

    // ---------------------------------------------------------------------------------------------
    // The other solution values in the cell quadrature points
    std::vector<Tensor<1, dim>> potential_gradients(n_q_points);
    std::vector<double> prev_sol_temperature_values(n_q_points);

    std::vector<double> prev_sol_face_temperature_values(n_face_q_points);
    // ---------------------------------------------------------------------------------------------

    typename DoFHandler<dim>::active_cell_iterator cell = dof_handler_heat.begin_active(),
            endc = dof_handler_heat.end();
    typename DoFHandler<dim>::active_cell_iterator current_cell = dof_handler_current.begin_active();

    for (; cell != endc; ++cell, ++current_cell) {
        fe_values.reinit(cell);
        cell_matrix = 0;
        cell_rhs = 0;

        fe_values.get_function_values(old_solution_heat, prev_sol_temperature_values);

        fe_values_current.reinit(current_cell);
        fe_values_current.get_function_gradients(solution_current, potential_gradients);

        // ----------------------------------------------------------------------------------------
        // Local matrix assembly
        // ----------------------------------------------------------------------------------------
        for (unsigned int q = 0; q < n_q_points; ++q) {

            double prev_temperature = prev_sol_temperature_values[q];
            double kappa = pq->kappa(prev_temperature);
            double sigma = pq->sigma(prev_temperature);

            double pot_grad_squared = potential_gradients[q].norm_square();

            for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                for (unsigned int j = 0; j < dofs_per_cell; ++j) {
                    cell_matrix(i, j) += (
                            fe_values.shape_value(i, q) * fe_values.shape_value(j, q) // Mass matrix
                            + gamma*kappa*fe_values.shape_grad(i, q) * fe_values.shape_grad(j, q)
                    ) * fe_values.JxW(q);
                }
                cell_rhs(i) += (
                        fe_values.shape_value(i, q)*prev_temperature
                        + gamma*fe_values.shape_value(i, q)*sigma*pot_grad_squared
                ) * fe_values.JxW(q);
                if (sigma*pot_grad_squared > max_heating_power) {
                    max_heating_power = sigma*pot_grad_squared;
                }
            }
        }
        // ----------------------------------------------------------------------------------------
        // Local right-hand side assembly
        // ----------------------------------------------------------------------------------------
        // Nottingham BC at the copper surface
        for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; ++f) {
            if (cell->face(f)->at_boundary() && cell->face(f)->boundary_id() == BoundaryId::copper_surface) {
                fe_face_values.reinit(cell, f);

                fe_face_values.get_function_values(old_solution_heat, prev_sol_face_temperature_values);

                // ----------------------------------------------------------------------------------
                // cell & face info
                std::pair<unsigned, unsigned> cop_cell_info = std::pair<unsigned, unsigned>(
                                                              cell->index(), f);
                // ----------------------------------------------------------------------------------

                for (unsigned int q = 0; q < n_face_q_points; ++q) {

                    double prev_temperature = prev_sol_face_temperature_values[q];
                    double nottingham_heat = get_nottingham_heat_bc(cop_cell_info, prev_temperature);

                    //nottingham_heat = 0.0;
                    for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                        cell_rhs(i) += (gamma * fe_face_values.shape_value(i, q)
                                * nottingham_heat * fe_face_values.JxW(q));
                    }
                }
            }
        }

        cell->get_dof_indices(local_dof_indices);
        for (unsigned int i = 0; i < dofs_per_cell; ++i) {
            for (unsigned int j = 0; j < dofs_per_cell; ++j)
                system_matrix_heat.add(local_dof_indices[i], local_dof_indices[j], cell_matrix(i, j));

            system_rhs_heat(local_dof_indices[i]) += cell_rhs(i);
        }
    }

    //std::cout << "Maximum heating power in system: " << max_heating_power << std::endl;

    std::map<types::global_dof_index, double> boundary_values;
    VectorTools::interpolate_boundary_values(dof_handler_heat, BoundaryId::copper_bottom,
            ConstantFunction<dim>(ambient_temperature), boundary_values);
    MatrixTools::apply_boundary_values(boundary_values, system_matrix_heat, solution_heat, system_rhs_heat);
}

template<int dim>
unsigned int CurrentsAndHeating<dim>::solve_current(int max_iter, double tol, bool pc_ssor, double ssor_param) {

    old_solution_current = solution_current;

    SolverControl solver_control(max_iter, tol);
    SolverCG<> solver(solver_control);

    if (pc_ssor) {
        PreconditionSSOR<> preconditioner;
        preconditioner.initialize(system_matrix_current, ssor_param);
        solver.solve(system_matrix_current, solution_current, system_rhs_current, preconditioner);
    } else {
        solver.solve(system_matrix_current, solution_current, system_rhs_current, PreconditionIdentity());
    }
    //std::cout << "   J: " << solver_control.last_step()
    //        << " CG iterations needed to obtain convergence." << std::endl;

    return solver_control.last_step();
}

template<int dim>
unsigned int CurrentsAndHeating<dim>::solve_heat(int max_iter, double tol, bool pc_ssor, double ssor_param) {

    old_solution_heat = solution_heat;

    SolverControl solver_control(max_iter, tol);
    SolverCG<> solver(solver_control);

    if (pc_ssor) {
        PreconditionSSOR<> preconditioner;
        preconditioner.initialize(system_matrix_heat, ssor_param);
        solver.solve(system_matrix_heat, solution_heat, system_rhs_heat, preconditioner);
    } else {
        solver.solve(system_matrix_heat, solution_heat, system_rhs_heat, PreconditionIdentity());
    }
    //std::cout << "   T: " << solver_control.last_step()
    //        << " CG iterations needed to obtain convergence." << std::endl;

    return solver_control.last_step();
}


template<int dim>
void CurrentsAndHeating<dim>::set_physical_quantities(PhysicalQuantities *pq_) {
    pq = pq_;
}

template<int dim>
void CurrentsAndHeating<dim>::set_timestep(const double time_step_) {
    time_step = time_step_;
}

template<int dim>
void CurrentsAndHeating<dim>::set_electric_field_bc(const Laplace<dim> &laplace) {

    double eps = 1e-9;

    interface_map_field.clear();

    // ---------------------------------------------------------------------------------------------
    // Loop over vacuum interface cells

    std::vector<Point<dim> > vacuum_interface_centers;
    std::vector<double> vacuum_interface_efield;

    QGauss<dim-1> face_quadrature_formula(1); // Quadrature with one point
    FEFaceValues<dim> vacuum_fe_face_values(laplace.fe, face_quadrature_formula,
            update_gradients | update_quadrature_points);

    // Electric field values from laplace solver
    std::vector<Tensor<1, dim> > electric_field_value(1);

    typename DoFHandler<dim>::active_cell_iterator vac_cell = laplace.dof_handler.begin_active(),
            vac_endc = laplace.dof_handler.end();
    for (; vac_cell != vac_endc; ++vac_cell) {
        for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; f++) {
            if (vac_cell->face(f)->boundary_id() == BoundaryId::copper_surface) {
                // ---
                // Electric field norm in the center (only quadrature point) of the face
                vacuum_fe_face_values.reinit(vac_cell, f);
                vacuum_fe_face_values.get_function_gradients(laplace.solution,
                        electric_field_value);
                double efield_norm = electric_field_value[0].norm();
                // ---

                vacuum_interface_efield.push_back(efield_norm);
                vacuum_interface_centers.push_back(vac_cell->face(f)->center());
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Loop over copper interface cells

    typename DoFHandler<dim>::active_cell_iterator cop_cell = dof_handler_current.begin_active(),
            cop_endc = dof_handler_current.end();

    for (; cop_cell != cop_endc; ++cop_cell) {
        for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; f++) {
            if (cop_cell->face(f)->boundary_id() == BoundaryId::copper_surface) {
                Point<dim> cop_face_center = cop_cell->face(f)->center();
                std::pair<unsigned, unsigned> cop_face_info(cop_cell->index(), f);
                // Loop over vacuum side and find corresponding (cell, face) pair
                for (unsigned int i = 0; i < vacuum_interface_centers.size(); i++) {
                    if (cop_face_center.distance(vacuum_interface_centers[i]) < eps) {
                        std::pair<std::pair<unsigned, unsigned>, double> pair(cop_face_info,
                                vacuum_interface_efield[i]);
                        interface_map_field.insert(pair);
                        break;
                    }
                }
                if (interface_map_field.count(cop_face_info) == 0)
                    std::cerr << "Error: probably a mismatch between copper and vacuum meshes." << std::endl;
            }
        }
    }
}

template<int dim>
void CurrentsAndHeating<dim>::set_electric_field_bc(const std::vector<double>& elfields) {
    const unsigned n_faces_per_cell = GeometryInfo<dim>::faces_per_cell;
    interface_map_field.clear();

    // Loop over copper interface cells
    typename DoFHandler<dim>::active_cell_iterator cell;
    unsigned i = 0;
    for (cell = dof_handler_current.begin_active(); cell != dof_handler_current.end(); ++cell)
        for (unsigned f = 0; f < n_faces_per_cell; ++f)
            if (cell->face(f)->boundary_id() == BoundaryId::copper_surface) {
                std::pair<unsigned, unsigned> face_info(cell->index(), f);
                interface_map_field.insert(
                        std::pair<std::pair<unsigned, unsigned>, double>(face_info, elfields[i++]));
            }
}

template<int dim>
void CurrentsAndHeating<dim>::set_electric_field_bc(const double uniform_efield) {
    uniform_efield_bc = uniform_efield;
}

template<int dim>
void CurrentsAndHeating<dim>::set_emission_bc(const std::vector<double> &emission_currents,
        const std::vector<double> &nottingham_heats) {
    const unsigned n_faces_per_cell = GeometryInfo<dim>::faces_per_cell;
    interface_map_emission_current.clear();
    interface_map_nottingham.clear();

    // Loop over copper interface cells
    typename DoFHandler<dim>::active_cell_iterator cell;
    unsigned i = 0;
    for (cell = dof_handler_current.begin_active(); cell != dof_handler_current.end(); ++cell)
        for (unsigned f = 0; f < n_faces_per_cell; ++f)
            if (cell->face(f)->boundary_id() == BoundaryId::copper_surface) {
                std::pair<unsigned, unsigned> face_info(cell->index(), f);
                interface_map_emission_current.insert(
                        std::pair<std::pair<unsigned, unsigned>, double>(face_info, emission_currents[i]));
                interface_map_nottingham.insert(
                        std::pair<std::pair<unsigned, unsigned>, double>(face_info, nottingham_heats[i++]));
            }
}

template<int dim>
std::vector<double> CurrentsAndHeating<dim>::get_temperature(const std::vector<int> &cell_indexes,
        const std::vector<int> &vert_indexes) {

    // Initialize vector with a value that is immediately visible if it's not changed to proper one
    std::vector<double> temperatures(cell_indexes.size(), 1e15);

    for (unsigned i = 0; i < cell_indexes.size(); i++) {
        // Using DoFAccessor (groups.google.com/forum/?hl=en-GB#!topic/dealii/azGWeZrIgR0)
        // NB: only works without refinement !!!
        typename DoFHandler<dim>::active_cell_iterator dof_cell(&triangulation, 0, cell_indexes[i],
                &dof_handler_heat);

        double temperature = solution_heat[dof_cell->vertex_dof_index(vert_indexes[i], 0)];
        temperatures[i] = temperature;
    }
    return temperatures;
}

template<int dim>
std::vector<Tensor<1, dim> > CurrentsAndHeating<dim>::get_current(
        const std::vector<int> &cell_indexes, const std::vector<int> &vert_indexes) {
    QGauss<dim> quadrature_formula(currents_degree + 1);
    FEValues<dim> fe_values(fe_current, quadrature_formula, update_gradients);

    std::vector<Tensor<1, dim> > potential_gradients(quadrature_formula.size());
    const FEValuesExtractors::Scalar potential(0);

    std::vector<Tensor<1, dim> > currents(cell_indexes.size());

    for (unsigned i = 0; i < cell_indexes.size(); i++) {
        // Using DoFAccessor (groups.google.com/forum/?hl=en-GB#!topic/dealii/azGWeZrIgR0)
        // NB: only works without refinement !!!
        typename DoFHandler<dim>::active_cell_iterator dof_cell(&triangulation, 0, cell_indexes[i],
                &dof_handler_current);

        double temperature = solution_heat[dof_cell->vertex_dof_index(vert_indexes[i], 0)];

        fe_values.reinit(dof_cell);
        fe_values.get_function_gradients(solution_current, potential_gradients);

        Tensor<1, dim> field = -1.0 * potential_gradients.at(vert_indexes[i]);
        Tensor<1, dim> current = pq->sigma(temperature) * field;

        currents[i] = current;
    }
    return currents;
}

template<int dim>
void CurrentsAndHeating<dim>::get_surface_nodes(std::vector<Point<dim>>& nodes) {
    const int n_faces_per_cell = GeometryInfo<dim>::faces_per_cell;
    nodes.clear();

    // Loop over copper interface cells
    typename DoFHandler<dim>::active_cell_iterator cell;
    for (cell = dof_handler_current.begin_active(); cell != dof_handler_current.end(); ++cell)
        for (int f = 0; f < n_faces_per_cell; f++)
            if (cell->face(f)->boundary_id() == BoundaryId::copper_surface)
                nodes.push_back(cell->face(f)->center());
}

template<int dim>
double CurrentsAndHeating<dim>::get_efield_bc(const std::pair<unsigned, unsigned> cop_cell_info) {
    double e_field = 1.0;
    if (interface_map_field.empty()) {
        e_field = uniform_efield_bc;
    } else {
        assert(interface_map_field.count(cop_cell_info) == 1);
        e_field = interface_map_field[cop_cell_info];
    }
    return e_field;
}

template<int dim>
double CurrentsAndHeating<dim>::get_emission_current_bc(const std::pair<unsigned, unsigned> cop_cell_info,
        const double temperature) {
    double emission_current = 0.0;
    if (interface_map_emission_current.empty()) {
        double e_field = get_efield_bc(cop_cell_info);
        emission_current = pq->emission_current(e_field, temperature);
    } else {
        assert(interface_map_emission_current.count(cop_cell_info) == 1);
        emission_current = interface_map_emission_current[cop_cell_info];
    }
    return emission_current;
}

template<int dim>
double CurrentsAndHeating<dim>::get_nottingham_heat_bc(const std::pair<unsigned, unsigned> cop_cell_info,
        const double temperature) {
    double nottingham_heat = 0.0;
    if (interface_map_nottingham.empty()) {
        double e_field = get_efield_bc(cop_cell_info);
        double emission_current = pq->emission_current(e_field, temperature);
        nottingham_heat = -1.0 * pq->nottingham_de(e_field, temperature) * emission_current;
    } else {
        assert(interface_map_nottingham.count(cop_cell_info) == 1);
        nottingham_heat = interface_map_nottingham[cop_cell_info];
    }
    return nottingham_heat;
}

template<int dim>
double CurrentsAndHeating<dim>::get_max_temperature() {
    return solution_heat.linfty_norm();
}

template<int dim>
Triangulation<dim>* CurrentsAndHeating<dim>::get_triangulation() {
    return &triangulation;
}

template<int dim>
DoFHandler<dim>* CurrentsAndHeating<dim>::get_dof_handler_current() {
    return &dof_handler_current;
}

// ----------------------------------------------------------------------------------------
// Class for outputting the resulting field distribution (calculated from potential distr.)
template <int dim>
class FieldPostProcessor : public DataPostprocessorVector<dim> {
public:
    FieldPostProcessor() : DataPostprocessorVector<dim>("field", update_gradients) {}

    void
    compute_derived_quantities_scalar ( const std::vector<double>               &/*uh*/,
                                        const std::vector<Tensor<1,dim> >       &duh,
                                        const std::vector<Tensor<2,dim> >       &/*dduh*/,
                                        const std::vector<Point<dim> >          &/*normals*/,
                                        const std::vector<Point<dim> >          &/*evaluation_points*/,
                                        std::vector<Vector<double> >            &computed_quantities) const {
        for (unsigned int i=0; i<computed_quantities.size(); i++) {
            for (unsigned int d=0; d<dim; ++d)
                computed_quantities[i](d) = duh[i][d];
        }
    }
};
// ----------------------------------------------------------------------------------------
// Class for outputting the resulting field distribution (calculated from potential distr.)
template <int dim>
class SigmaPostProcessor : public DataPostprocessorScalar<dim> {
    PhysicalQuantities *pq;
public:
    SigmaPostProcessor(PhysicalQuantities *pq_) :
            DataPostprocessorScalar<dim>("sigma", update_values), pq(pq_) {
    }
    void
    compute_derived_quantities_scalar ( const std::vector<double>               &uh,
                                        const std::vector<Tensor<1,dim> >       &/*duh*/,
                                        const std::vector<Tensor<2,dim> >       &/*dduh*/,
                                        const std::vector<Point<dim> >          &/*normals*/,
                                        const std::vector<Point<dim> >          &/*evaluation_points*/,
                                        std::vector<Vector<double> >            &computed_quantities) const {
        for (unsigned int i=0; i<computed_quantities.size(); i++) {
            double temperature = uh[i];
            computed_quantities[i](0) = pq->sigma(temperature);
        }
    }
};
// ----------------------------------------------------------------------------

template<int dim>
void CurrentsAndHeating<dim>::output_results_current(const std::string filename) const {

    FieldPostProcessor<dim> field_post_processor; // needs to be before data_out
    DataOut<dim> data_out;

    data_out.attach_dof_handler(dof_handler_current);
    data_out.add_data_vector(solution_current, "potential");
    data_out.add_data_vector(solution_current, field_post_processor);

    data_out.build_patches();

    try {
        std::ofstream output(filename);
        data_out.write_vtk(output);
    } catch (...) {
        std::cerr << "WARNING: Couldn't open " + filename << ". ";
        std::cerr << "Output is not saved." << std::endl;
    }
}

template<int dim>
void CurrentsAndHeating<dim>::output_results_heating(const std::string filename) const {

    SigmaPostProcessor<dim> sigma_post_processor(pq);
    DataOut<dim> data_out;

    data_out.attach_dof_handler(dof_handler_heat);
    data_out.add_data_vector(solution_heat, "temperature");
    data_out.add_data_vector(solution_heat, sigma_post_processor);

    data_out.build_patches();

    try {
        std::ofstream output(filename);
        data_out.write_vtk(output);
    } catch (...) {
        std::cerr << "WARNING: Couldn't open " + filename << ". ";
        std::cerr << "Output is not saved." << std::endl;
    }
}

template class CurrentsAndHeating<2> ;
template class CurrentsAndHeating<3> ;

} // namespace fch

