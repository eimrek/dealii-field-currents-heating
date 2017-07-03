/*
 * currents_and_heating.h
 *
 *  Created on: Jul 28, 2016
 *      Author: kristjan
 */

#ifndef INCLUDE_CURRENTS_AND_HEATING_H_
#define INCLUDE_CURRENTS_AND_HEATING_H_

#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_reordering.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/lac/sparse_matrix.h>

#include <fstream>
#include <iostream>
#include <tuple>

#include "mesh_preparer.h"
#include "physical_quantities.h"
#include "laplace.h"

namespace fch {

// forward declaration for Laplace to exist when declaring CurrentsAndHeating
template<int dim> class Laplace;

using namespace dealii;

/** @brief Class to solve time dependent heat and continuity equations in 2D or 3D to obtain temperature and current density distribution in material.
 * It is inspired by the step-26 of Deal.II tutorial that includes only heat equation.
 * https://www.dealii.org/8.5.0/doxygen/deal.II/step_26.html
 * Current density calculation is inspired by the Laplace equation, which is demonstrated in the step-3 of Deal.II tutorial
 * https://www.dealii.org/8.5.0/doxygen/deal.II/step_3.html
 */
template<int dim>
class CurrentsAndHeating {
public:

    /**
     * Constructor for CurrentsAndHeating
     * Physical quantities will be set as NULL and default timestep will be used,
     * so they must be set separately
     */
    CurrentsAndHeating();

    /**
     * Constructor for CurrentsAndHeating
     * @param time_step_  timestep of time domain integration [sec]
     * @param pq_         object to evaluate tabulated physical quantities (sigma, kappa, gtf emission)
     */
    CurrentsAndHeating(double time_step_, PhysicalQuantities *pq_);

    /**
     * Imports mesh from file and sets the boundary indicators corresponding to copper
     * @param file_name file from the mesh is imported
     */
    void import_mesh_from_file(const std::string file_name);

    /**
     * imports mesh directly from vertex and cell data and sets the boundary indicators
     * @return true if success, otherwise false
     */
    bool import_mesh_directly(std::vector<Point<dim> > vertices,
            std::vector<CellData<dim> > cells);

    /** @brief set up dynamic sparsity pattern for current density calculation
     */
    void setup_current_system();

    /** @brief set up dynamic sparsity pattern for temperature calculation
     */
    void setup_heating_system();

    /** @brief assemble the matrix equation for current density calculation ()
     * Calculate sparse matrix elements and right-hand-side vector
     * according to the continuity equation weak formulation and to the boundary conditions.
     */
    void assemble_current_system();

    /** @brief assemble the matrix equation for temperature calculation using Crank-Nicolson time integration method
     * Calculate sparse matrix elements and right-hand-side vector
     * according to the time dependent heat equation weak formulation and to the boundary conditions.
     */
    void assemble_heating_system_crank_nicolson();

    /** @brief assemble the matrix equation for temperature calculation using implicit Euler time integration method
     * Calculate sparse matrix elements and right-hand-side vector
     * according to the time dependent heat equation weak formulation and to the boundary conditions.
     */
    void assemble_heating_system_euler_implicit();

    /** solves the matrix equation for current density calculations using conjugate gradient method
     * @param max_iter maximum number of iterations allowed
     * @param tol tolerance of the solution
     * @param pc_ssor flag to use SSOR preconditioner
     * @param ssor_param   parameter to SSOR preconditioner. 1.2 is known to work well with laplace.
     *                     its fine tuning optimises calculation time
     */
    unsigned int solve_current(int max_iter = 2000, double tol = 1e-9, bool pc_ssor = true,
            double ssor_param = 1.2);

    /** solves the matrix equation for temperature calculations using conjugate gradient method
     * @param max_iter maximum number of iterations allowed
     * @param tol tolerance of the solution
     * @param pc_ssor flag to use SSOR preconditioner
     * @param ssor_param   parameter to SSOR preconditioner. 1.2 is known to work well with laplace.
     *                     its fine tuning optimises calculation time
     */
    unsigned int solve_heat(int max_iter = 2000, double tol = 1e-9, bool pc_ssor = true,
            double ssor_param = 1.2);

    /** Output the electric potential [V] and field [V/nm] to a specified file in vtk format */
    void output_results_current(const std::string filename = "current_solution.vtk") const;

    /** Output the temperature [K] and electrical conductivity [1/(Ohm*nm)] to a specified file in vtk format */
    void output_results_heating(const std::string filename = "heating_solution.vtk") const;

    /** Set the electric field boundary condition on copper-vacuum boundary.
     * The electric field is extracted directly from the Laplace solver.
     */
    void set_electric_field_bc(const Laplace<dim> &laplace);

    /** Set the electric field boundary condition on copper-vacuum boundary.
     * The electric fields must be on the centroids of the vacuum-material boundary faces
     * in the order specified in the get_surface_nodes() method.
     */
    void set_electric_field_bc(const std::vector<double> &e_fields);

    /** Set the electric field boundary condition on copper-vacuum boundary.
     * The electric field on every face will be the same. */
    void set_electric_field_bc(const double uniform_efield);

    /** Set timestep of time domain integration [sec] */
    void set_timestep(const double time_step_);

    /** Set the emission current and Nottingham boundary condition on copper-vacuum boundary.
     * The values must be on the centroids of the vacuum-material boundary faces
     * in the order specified in the get_surface_nodes() method.
     */
    void set_emission_bc(const std::vector<double> &emission_currents,
            const std::vector<double> &nottingham_heats);

    /** Sets the physical quantities object */
    void set_physical_quantities(PhysicalQuantities *pq_);

    /**
     * Method to obtain the temperature values in selected nodes.
     * @param cell_indexes global cell indexes, where the corresponding nodes are situated
     * @param vert_indexes the vertex indexes of the nodes inside the cell
     * @return temperatur  ///< default temperature applied on bottom of the materiale values in the specified nodes
     */
    std::vector<double> get_temperature(const std::vector<int> &cell_indexes,
            const std::vector<int> &vert_indexes);

    /**
     * Method to obtain the current density values in selected nodes.
     * @param cell_indexes global cell indexes, where the corresponding nodes are situated
     * @param vert_indexes the vertex indexes of the nodes inside the cell
     * @return current density vectors in the specified nodes
     */
    std::vector<Tensor<1, dim>> get_current(const std::vector<int> &cell_indexes,
            const std::vector<int> &vert_indexes);

    /** export the centroids of surface faces */
    void get_surface_nodes(std::vector<Point<dim>>& nodes);

    double get_max_temperature();

    /** Provide triangulation object to get access to the mesh data */
    Triangulation<dim>* get_triangulation();

    /** Provide dof_handler object to get access to the mesh data */
    DoFHandler<dim>* get_dof_handler_current();

private:

    double get_efield_bc(std::pair<unsigned, unsigned> cop_cell_info);
    double get_emission_current_bc(std::pair<unsigned, unsigned> cop_cell_info, const double temperature);
    double get_nottingham_heat_bc(std::pair<unsigned, unsigned> cop_cell_info, const double temperature);

    static constexpr unsigned int currents_degree = 1; ///< degree of the shape functions in current density solver
    static constexpr unsigned int heating_degree = 1;  ///< degree of the shape functions in temperature solver

    static constexpr double ambient_temperature = 300.0; ///< temperature boundary condition, i.e temperature applied on bottom of the material

    static constexpr double cu_rho_cp = 3.4496e-21;    ///< volumetric heat capacity of copper J/(K*nm^3)

    double time_step;

    double uniform_efield_bc;

    Triangulation<dim> triangulation;

    // Current specific variables
    FE_Q<dim> fe_current;
    DoFHandler<dim> dof_handler_current;
    SparsityPattern sparsity_pattern_current;
    SparseMatrix<double> system_matrix_current;
    Vector<double> system_rhs_current;

    Vector<double> solution_current;
    Vector<double> old_solution_current;

    // Heating specific variables
    FE_Q<dim> fe_heat;
    DoFHandler<dim> dof_handler_heat;
    SparsityPattern sparsity_pattern_heat;
    SparseMatrix<double> system_matrix_heat;
    Vector<double> system_rhs_heat;

    Vector<double> solution_heat;
    Vector<double> old_solution_heat;

    Vector<double> const_temperature_solution;

    PhysicalQuantities *pq;

    /** Mapping of copper interface faces to vacuum side e field norm
     * (copper_cell_index, copper_cell_face) <-> (electric field norm)
     */
    std::map<std::pair<unsigned, unsigned>, double> interface_map_field;

    /** Mapping of copper interface faces to emission BCs
     * (copper_cell_index, copper_cell_face) <-> (emission_current/nottingham_heat)
     */
     std::map<std::pair<unsigned, unsigned>, double> interface_map_emission_current;
     std::map<std::pair<unsigned, unsigned>, double> interface_map_nottingham;
};

} // end fch namespace

#endif /* INCLUDE_CURRENTS_AND_HEATING_H_ */
