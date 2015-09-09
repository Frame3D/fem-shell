/*************************************************************
 * todo
 *
 * MPI testen
 * Konvergenz der verschiedenen Orientierungen testen
 * calc stresses and write into output (steinke 230 für membran,
 *
 * ***********************************************************/

// C++ include files that we need
#include <iostream>
#include <algorithm>
#include <math.h>

// libMesh includes
#include "libmesh/libmesh.h"
#include "libmesh/mesh.h"
#include "libmesh/mesh_generation.h"
#include "libmesh/exodusII_io.h"
#include "libmesh/vtk_io.h"
#include "libmesh/gmsh_io.h"
#include "libmesh/gnuplot_io.h"
#include "libmesh/linear_implicit_system.h"
#include "libmesh/equation_systems.h"
#include "libmesh/fe.h"
#include "libmesh/quadrature_gauss.h"
#include "libmesh/dof_map.h"
#include "libmesh/sparse_matrix.h"
#include "libmesh/numeric_vector.h"
#include "libmesh/dense_matrix.h"
#include "libmesh/dense_submatrix.h"
#include "libmesh/dense_vector.h"
#include "libmesh/dense_subvector.h"
#include "libmesh/perf_log.h"
#include "libmesh/elem.h"
#include "libmesh/boundary_info.h"
#include "libmesh/zero_function.h"
#include "libmesh/dirichlet_boundaries.h"
#include "libmesh/string_to_enum.h"
#include "libmesh/getpot.h"

// Bring in everything from the libMesh namespace
using namespace libMesh;

// Matrix and right-hand side assemble
void assemble_elasticity(EquationSystems& es,
                         const std::string& system_name);

void eval_B(DenseMatrix<Real>& Hcoeffs, Real L1, Real L2, DenseMatrix<Real> &dphi_p, DenseMatrix<Real> &out);

bool debug = false;
Real nu = 0.4;
Real em = 100000.0;
Real thickness = 1.0;
int iter = 1;
std::vector<DenseVector<Real> > forces;

// Begin the main program.
int main (int argc, char** argv)
{
    // Initialize libMesh and any dependent libaries
    LibMeshInit init (argc, argv);

    // Initialize the cantilever mesh
    const unsigned int dim = 2;

    int pid = processor_id();

    std::cout << "Start of process " << pid << "\n";

    if (argc < 7)
    {
        if (pid == 0)
        {
            err << "Usage: " << argv[0] << " -d -nu -e -mesh -out\n"
                << "-d: Debug-Mode (1=on, 0=off (default))\n"
                << "-nu: Possion-Number nu (0.4 default)\n"
                << "-e: Elasticity Modulus E (1000000.0 default)\n"
                << "-t: Thickness (1.0 default)\n"
                << "-mesh: Input mesh file (*.xda or *.msh)\n"
                << "-out: Output file name (without extension)\n";
        }

        libmesh_error_msg("Error, must choose valid parameters.");
    }

    // Parse command line
    GetPot command_line (argc, argv);

    debug = false;
    if ( command_line.search(1, "-d") )
        debug = command_line.next(0) == 1? true : false;
    nu = 0.4;
    if ( command_line.search(1, "-nu") )
        nu = command_line.next(nu);
    em = 1000000.0;
    if ( command_line.search(1, "-e") )
        em = command_line.next(em);
    thickness = 1.0;
    if ( command_line.search(1, "-t") )
        thickness = command_line.next(thickness);
    std::string filename;
    if ( command_line.search(1, "-mesh") )
        filename = command_line.next("1_tri.xda");
    std::string outfile;
    if ( command_line.search(1, "-out") )
        outfile = command_line.next("out");
    if ( command_line.search(1, "-iter") )
        iter = command_line.next(1);

    // Skip this 2D example if libMesh was compiled as 1D-only.
    libmesh_example_requires(dim <= LIBMESH_DIM, "2D support");

    // Create a 2D mesh distributed across the default MPI communicator.
    Mesh mesh(init.comm(), dim);
    mesh.allow_renumbering(false);
    if (mesh.allow_renumbering())
        std::cout << "mesh erlaubt renumbering\n";
    mesh.read(filename);

    if (filename.find(".msh") != std::string::npos) // if we load a GMSH mesh file, we need to execute a preparation step
    {
        std::cout << "we use gmsh\n";
        //mesh.prepare_for_use(true, false);// skip renumbering, skip find neighbors (depricated)
    }

    // Print information about the mesh to the screen.
    if (debug)
        mesh.print_info();

    // Load file with forces (only needed for stand-alone version)
    std::filebuf fb;
    if (filename.find(".xda") != std::string::npos ||
        filename.find(".msh") != std::string::npos)
        filename.resize(filename.size()-4);

    /*
     * load mesh file and create a map linking the real node IDs to the libmesh created Ids
     * needed to get the force vectors to the right positions (nodes)
     */
    // open mesh file
    // case .xda, .msh, etc
    // look for elements definition
    // go through all elements
    //    go through all nodes n of element el
    //       if (map.find(n) == map.end()) // node-id was not yet processed
    //          map.insert(n,i);
    //          i++;

    filename += "_f";

    if (fb.open (filename.c_str(),std::ios::in))
    {
        std::istream input(&fb);
        int n_Forces;
        input >> n_Forces;
        double factor = 1.0;
        input >> factor;
        for (int i = 0; i < n_Forces; i++)
        {
            DenseVector<Real> p(3);
            for (int j = 0; j < 3; j++)
                input >> p(j);
            p *= factor;
            forces.push_back(p);
        }
    }

    if (debug)
    {
        std::cout << "Forces-vector has " << forces.size() << " entries: [\n";
        for (unsigned int i = 0; i < forces.size(); i++)
            std::cout << "(" << forces[i](0) << "," << forces[i](1) << "," << forces[i](2) << ")\n";
        std::cout << "]\n";
    }
    // Create an equation systems object.
    EquationSystems equation_systems (mesh);

    // Declare the system and its variables.
    // Create a system named "Elasticity"
    LinearImplicitSystem& system =
        equation_systems.add_system<LinearImplicitSystem> ("Elasticity");

    // Add three displacement variables, u, v and W,
    // as well as three drilling variables theta_x, theta_y and theta_z to the system
    unsigned int u_var  = system.add_variable("u",  FIRST, LAGRANGE);
    unsigned int v_var  = system.add_variable("v",  FIRST, LAGRANGE);
    unsigned int w_var  = system.add_variable("w",  FIRST, LAGRANGE);
    unsigned int tx_var = system.add_variable("tx", FIRST, LAGRANGE);
    unsigned int ty_var = system.add_variable("ty", FIRST, LAGRANGE);
    unsigned int tz_var = system.add_variable("tz", FIRST, LAGRANGE);

    system.attach_assemble_function (assemble_elasticity);

    // Construct a Dirichlet boundary condition object
    // We impose a "clamped" boundary condition on the
    // nodes with bc_id = 0
    std::set<boundary_id_type> boundary_ids;
    boundary_ids.insert(0);

    // Create a vector storing the variable numbers which the BC applies to
    std::vector<unsigned int> variables(6);
    variables[0] = u_var; variables[1] = v_var;
    variables[2] = w_var; variables[3] = tx_var; variables[4] = ty_var;
    variables[5] = tz_var;

    // Create a ZeroFunction to initialize dirichlet_bc
    ZeroFunction<> zf;

    DirichletBoundary dirichlet_bc(boundary_ids,
                                   variables,
                                   &zf);

    // We must add the Dirichlet boundary condition _before_
    // we call equation_systems.init()
    system.get_dof_map().add_dirichlet_boundary(dirichlet_bc);

    if (debug) std::cout << "before systems.init\n";

    // Initialize the data structures for the equation system.
    equation_systems.init();

    if (debug) std::cout << "after systems.init\n";

    // mesh-export object:
    VTKIO vtkio(mesh);

    //for (int x = 0; x < iter; x++)
    {//BEGIN TIMESTEP FOR-LOOP
        //std::cout << "progress =" << x << "/" << iter << " (" << (x/(float)iter) << "%)\n";

        //equation_systems.reinit();

        if (debug)
            std::cout << "after systems.reinit\n";

        // Print information about the system to the screen.
        //equation_systems.print_info();

        /**
         * Solve the system
         **/
        Parallel::barrier();
        equation_systems.solve();
        Parallel::barrier();
        if (debug)
            std::cout << "after solve\n";
//if (processor_id() == 0){
        //std::vector<Number> sols;
        //equation_systems.build_solution_vector(sols);

        if (debug)
            std::cout << "after build solution vector\n";

        //if (debug)
        {
            system.matrix->print(std::cout);
            system.rhs->print(std::cout);
        }

        //MeshBase::const_node_iterator no = mesh.nodes_begin();
        //const MeshBase::const_node_iterator end_no = mesh.nodes_end();

        //if (debug) {
            //std::cout << "Solution: x=[";
            //for (int i = 0 ; no != end_no; ++no,++i)
            //    std::cout << "uvw_" << i << " = " << sols[6*i] << ", " << sols[6*i+1] << ", " << sols[6*i+2] << "\n";
            //std::cout << "]\n" << std::endl;
        //}

/*        unsigned int ln = mesh.n_local_nodes();
        DenseVector<Real> displacements;
        displacements.resize(ln*3);
        std::vector<short> processedNodes(ln,0);

        // iterator for iterating through the elements of the mesh:
        MeshBase::const_element_iterator       el     = mesh.active_local_elements_begin();
        const MeshBase::const_element_iterator end_el = mesh.active_local_elements_end();

        std::vector<dof_id_type> dof_indices_u;

        const DofMap& dof_map = system.get_dof_map();

        // do the actual displacing of the mesh's nodes
        // for all elements elem in mesh do:
        for ( ; el != end_el; ++el)
        {
            const Elem* elem = *el;

            dof_map.dof_indices (elem, dof_indices_u, u_var);

            for (unsigned int i = 0; i < elem->n_nodes(); i++)
            {
                dof_id_type id = dof_indices_u[i]; // 0,6,12,...,i*6
                if (debug)
                    std::cout << "elem-node-id: " << id << ", uvw = (" << sols[id] << "," << sols[id+1] << "," << sols[id+2] << ")\n";
                displacements(3*id/6)   += sols[id];
                displacements(3*id/6+1) += sols[id+1];
                displacements(3*id/6+2) += sols[id+2];
                processedNodes[id/6]++;
            }
        }*/

        /*no = mesh.nodes_begin();
        for (int i = 0 ; no != end_no; ++no,++i)
        {
            Node *nd = (*no);
            //if (processedNodes[i] > 0)
            {
                (*nd)(0) += sols[6*i];//displacements(3*i)  /(float)processedNodes[i];
                (*nd)(1) += sols[6*i+1];//displacements(3*i+1)/(float)processedNodes[i];
                (*nd)(2) += sols[6*i+2];//displacements(3*i+2)/(float)processedNodes[i];
            }
        }*/

        // Plot the solution
        std::ostringstream file_name;
        file_name << "out/" << outfile << "_"
                  << std::setw(3)
                  << std::setfill('0')
                  << std::right
                  << processor_id()
                  << ".vtu";

        vtkio.write_equation_systems(file_name.str(), equation_systems);

    std::cout << "All done ;)\n";

}
    // All done.
    return 0;
}


void assemble_elasticity(EquationSystems& es,
                         const std::string& system_name)
{
    libmesh_assert_equal_to (system_name, "Elasticity");

    // get the mesh
    const MeshBase& mesh = es.get_mesh();
    // get a reference to the system
    LinearImplicitSystem& system = es.get_system<LinearImplicitSystem>("Elasticity");
    // get the ids for the unknowns
    const unsigned int u_var = system.variable_number ("u");
    const unsigned int v_var = system.variable_number ("v");
    const unsigned int w_var = system.variable_number ("w");

    // A reference to the DofMap object for this system.
    // The DofMap object handles the index translation from node and element numbers to degree of freedom numbers.
    const DofMap& dof_map = system.get_dof_map();

    // stiffness matrix Ke_m for element (membrane part),
    //                 Ke_p for element (plate part)
    DenseMatrix<Number> Ke;
    DenseVector<Number> Fe;

    DenseMatrix<Number> Ke_m, Ke_p;

    //
    std::vector<dof_id_type> dof_indices;
    std::vector<dof_id_type> dof_indices_u;
    std::vector<dof_id_type> dof_indices_v;
    std::vector<dof_id_type> dof_indices_w;

    // iterator for iterating through the elements of the mesh:
    MeshBase::const_element_iterator       el     = mesh.active_local_elements_begin();
    const MeshBase::const_element_iterator end_el = mesh.active_local_elements_end();

    Node *ndi = nullptr, *ndj = nullptr;
    DenseMatrix<Real> transTri;
    DenseMatrix<Real> transUV, dphi_p; // xij, yij
    std::vector<Real> sidelen; // lij^2
    DenseMatrix<Real> Hcoeffs; // ak, ..., ek
    Real A_tri;
    //DenseMatrix<Real> AA;
    DenseMatrix<Real> CC;

    DenseMatrix<Real> Dp, Dm;
    Dp.resize(3,3);
    Dp(0,0) = 1.0; Dp(0,1) = nu;
    Dp(1,0) = nu;  Dp(1,1) = 1.0;
    Dp(2,2) = (1.0-nu)/2.0;
    Dm = Dp;
    Dp *= em*pow(thickness,3.0)/(12.0*(1.0-nu*nu)); // material matrix for plate part
    Dm *= em/(1.0-nu*nu); // material matrix for membrane part
    std::vector<std::vector<Real> > qps;

    unsigned int ln = mesh.n_nodes();
    std::vector<bool> processedNodes(ln,false);

    // for all elements elem in mesh do:
    for ( ; el != end_el; ++el)
    {
        if (debug) std::cout << "START ELEMENT\n";
        const Elem* elem = *el;

        // get the local to global DOF-mappings for this element
        dof_map.dof_indices (elem, dof_indices);
        dof_map.dof_indices (elem, dof_indices_u, u_var);
        dof_map.dof_indices (elem, dof_indices_v, v_var);
        dof_map.dof_indices (elem, dof_indices_w, w_var);
        // get the number of DOFs for this element

        // resize the current element matrix and vector to an appropriate size
        Ke.resize (18, 18);
        Fe.resize (18);

        // transform arbirtrary 3d triangle down to xy-plane with node a at origin (implicit):
        Node U, V, W;
        ndi = elem->get_node(0); // node A
        ndj = elem->get_node(1); // node B
        U = (*ndj)-(*ndi); // U = B-A
        ndj = elem->get_node(2); // node C
        V = (*ndj)-(*ndi); // V = C-A
        transUV.resize(3,2);
        for (int i = 0; i < 3; i++)
        { // node A lies in local origin (per definition)
            transUV(i,0) = U(i); // node B in global coordinates (triangle translated s.t. A lies in origin)
            transUV(i,1) = V(i); // node C in global coordinates ( -"- )
        }
        /* transUV [ b_x, c_x ]
         *         [ b_y, c_y ]
         *         [ b_z, c_z ]
         */

        // area of triangle is half the length of the cross product of U and V
        W = U.cross(V);
        A_tri = 0.5*W.size();

        U = U.unit();   // local x-axis unit vector
        W = W.unit();   // local z-axis unit vector, normal to triangle
        V = W.cross(U); // local y-axis unit vector (cross prod of 2 normalized vectors is automatically normalized)

        transTri.resize(3,3); // global to local transformation matrix
        for (int j = 0; j < 3; j++)
        {
            transTri(0,j) = U(j);
            transTri(1,j) = V(j);
            transTri(2,j) = W(j);
        }
        /* transTri [ u_x, u_y, u_z ]
         *          [ v_x, v_y, v_z ]
         *          [ w_x, w_y, w_z ]
         */

        // transform B and C to local coordinates and store results in the same place
        transUV.left_multiply(transTri);

        if (debug) {
std::cout << "trafsUV:\n";
transUV.print(std::cout);
std::cout << std::endl;
std::cout << "trafo:\n";
transTri.print(std::cout);
std::cout << std::endl;
        }
/*****************************************
 * BEGIN OF PLATE COMPUTATION            *
 *****************************************/
        qps.resize(3);
        for (unsigned int i = 0; i < qps.size(); i++)
            qps[i].resize(2);

        dphi_p.resize(3,2); // resizes matrix to 3 rows, 2 columns and zeros entries
        dphi_p(0,0) = -transUV(0,0); // x12 = x1-x2 = 0-x2 = -x2
        dphi_p(1,0) =  transUV(0,1); // x31 = x3-x1 = x3-0 = x3
        dphi_p(2,0) =  transUV(0,0)-transUV(0,1); // x23 = x2-x3
        dphi_p(0,1) = -transUV(1,0); // y12 = 0, stays zero, as node B and A lies on local x-axis and therefore y=0 for both
        dphi_p(1,1) =  transUV(1,1); // y31 = y3-y1 = y3-0 = y3
        dphi_p(2,1) =  transUV(1,0)-transUV(1,1); // y23 = y2-y3 = 0-y3 = -y3

        //std::cout << "A_tri old = " << A_tri << "\n";
        //A_tri = 0.5*(dphi_p(1,1)*dphi_p(2,0)-dphi_p(2,1)*dphi_p(1,0));
        //std::cout << "A_tri new = " << A_tri << "\n";

        /*for (int k = 0; k <= 1; k++)
        {
            // TODO: frage: werden hier die punkte in geometrischen koordinaten gebraucht, oder grundsätzlich 0.5 / 0.5 also in dreieckskoordinaten
            // jetzt: geometrische lokale koordinaten
            // wahrscheinlich aber eher: (1/2,0), (1/2,1/2), (0,1/2) für xhi,eta (und damit immer die gleichen)
            qps[0][k] = transUV(k,0)*0.5; // midpoint of side AB, so 1/2*B
            qps[1][k] = transUV(k,1)*0.5; // midpoint of side AC, so 1/2*C
            qps[2][k] = 0.5*(transUV(k,0)+transUV(k,1)); // midpoint of side BC, so 1/2*(B+C)
        }*/
        qps[0][0] = 1.0/6.0; qps[0][1] = 1.0/6.0;
        qps[1][0] = 2.0/3.0; qps[1][1] = 1.0/6.0;
        qps[2][0] = 1.0/6.0; qps[2][1] = 2.0/3.0;
        //qps[0][0] = 1.0; qps[0][1] = 0.0;
        //qps[1][0] = 0.0; qps[1][1] = 1.0;
        //qps[2][0] = 0.0; qps[2][1] = 0.0;

        // side-lengths squared:
        sidelen.resize(3);
        sidelen[0] = pow(dphi_p(0,0), 2.0) + pow(dphi_p(0,1), 2.0); // side AB, x12^2 + y12^2 (=0) -> x12^2 = x2^2
        sidelen[1] = pow(dphi_p(1,0), 2.0) + pow(dphi_p(1,1), 2.0); // side AC, x31^2 + y31^2
        sidelen[2] = pow(dphi_p(2,0), 2.0) + pow(dphi_p(2,1), 2.0); // side BC, x23^2 + y23^2

        Hcoeffs.resize(1,3);
        for (int i = 0; i < 3; i++)
            Hcoeffs(0,i) = sidelen[i];
        /*Hcoeffs.resize(3,5); // 4,5,6 -> 3, a-e -> 5
        for (unsigned int k = 0; k < 3; k++) // k=0 <-> 23, k=1 <-> 31, k=2 <-> 12
        {
            Hcoeffs(k,0) = -dphi_p(2-k,0)/sidelen[2-k];
            Hcoeffs(k,1) = 0.75 * dphi_p(2-k,0) * dphi_p(2-k,1) / sidelen[2-k];
            Hcoeffs(k,2) = (0.25*pow(dphi_p(2-k,0),2.0) - 0.5*pow(dphi_p(2-k,1),2.0)) / sidelen[2-k];
            Hcoeffs(k,3) = -dphi_p(2-k,1)/sidelen[2-k];
            Hcoeffs(k,4) = (0.25*pow(dphi_p(2-k,1),2.0) - 0.5*pow(dphi_p(2-k,0),2.0)) / sidelen[2-k];
        }*/

        /*Hcoeffs.resize(3,4); // 4,5,6 -> 3, a-e -> 5
        for (unsigned int k = 0; k < 3; k++) // k=0 <-> 23, k=1 <-> 31, k=2 <-> 12
        {
            Hcoeffs(k,0) = -6.0 * dphi_p(2-k,0)                 / sidelen[2-k];
            Hcoeffs(k,1) =  3.0 * dphi_p(2-k,0) * dphi_p(2-k,1) / sidelen[2-k];
            Hcoeffs(k,2) = -6.0 * dphi_p(2-k,1)                 / sidelen[2-k];
            Hcoeffs(k,3) =  3.0 * pow(dphi_p(2-k,1),2.0)        / sidelen[2-k];
        }*/

        // resize the current element matrix and vector to an appropriate size
        Ke_p.resize(9, 9);
        for (unsigned int i = 0; i < qps.size(); i++)
        {
            if (debug)
                std::cout << "quadrature point (" << qps[i][0] << "," << qps[i][1] << ")\n";

            if (debug) {
            std::cout << "Hcoeffs:\n";
            Hcoeffs.print(std::cout);
            std::cout << std::endl;
            }

            eval_B(Hcoeffs, qps[i][0], qps[i][1], dphi_p, CC);

            DenseMatrix<Real> Y(3,3);
            Y(0,0) = pow(dphi_p(2,1),2.0);
            Y(0,1) = pow(dphi_p(1,1),2.0);
            Y(0,2) = dphi_p(2,1)*dphi_p(1,1);
            Y(1,0) = pow(dphi_p(2,0),2.0);
            Y(1,1) = pow(dphi_p(1,0),2.0);
            Y(1,2) = dphi_p(1,0)*dphi_p(2,0);
            Y(2,0) = -2.0*dphi_p(2,0)*dphi_p(2,1);
            Y(2,1) = -2.0*dphi_p(1,0)*dphi_p(1,0);
            Y(2,2) = -dphi_p(2,0)*dphi_p(1,1)-dphi_p(1,0)*dphi_p(2,1);

            Y *= 1.0/(4.0*pow(A_tri,2.0));
            //CC *= 1.0/(2.0*A_tri); // CC entspricht nun B

            if (debug) {
            std::cout << "B:\n";
            CC.print(std::cout);
            std::cout << std::endl;
            }

            DenseMatrix<Real> temp;
            temp = Dp; // temp = 3x3
            temp.right_multiply(Y); // temp = 9x3
            temp.right_multiply(CC);
            temp.left_multiply_transpose(Y); // temp = 9x9
            temp.left_multiply_transpose(CC);

            temp *= 1.0/6.0; // gauss-weight

            Ke_p += temp;
        }
        Ke_p *= 2.0*A_tri;
/*****************************************
 * END OF PLATE COMPUTATION            *
 *****************************************/
/*****************************************
 * BEGIN OF PLANE COMPUTATION            *
 *****************************************/
        DenseMatrix<Real> B_m(3,6);
        B_m(0,0) =  dphi_p(2,1); //  y23
        B_m(0,2) =  dphi_p(1,1); //  y31
        B_m(0,4) =  dphi_p(0,1); //  y12
        B_m(1,1) = -dphi_p(2,0); // -x23
        B_m(1,3) = -dphi_p(1,0); // -x31
        B_m(1,5) = -dphi_p(0,0); // -x12
        B_m(2,0) = -dphi_p(2,0); // -x23
        B_m(2,1) =  dphi_p(2,1); //  y23
        B_m(2,2) = -dphi_p(1,0); // -x31
        B_m(2,3) =  dphi_p(1,1); //  y31
        B_m(2,4) = -dphi_p(0,0); // -x12
        B_m(2,5) =  dphi_p(0,1); //  y12
        B_m *= 1.0/(2.0*A_tri);

        // Ke_m = t*A* B^T * Dm * B
        Ke_m = Dm;
        Ke_m.right_multiply(B_m);
        Ke_m.left_multiply_transpose(B_m);
        Ke_m *= thickness*A_tri;

/*************************************************
 * END OF PLANE COMPUTATION                      *
 *************************************************/
        // copy values from submatrices into overall element matrix:
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                // submatrix K_ij [6x6]
                Ke(6*i,  6*j)       = Ke_m(2*i,  2*j);   // uu
                Ke(6*i,  6*j+1)     = Ke_m(2*i,  2*j+1); // uv
                Ke(6*i+1,6*j)       = Ke_m(2*i+1,2*j);   // vu
                Ke(6*i+1,6*j+1)     = Ke_m(2*i+1,2*j+1); // vv
                Ke(2+6*i,  2+6*j)   = Ke_p(3*i,  3*j);   // ww
                Ke(2+6*i,  2+6*j+1) = Ke_p(3*i,  3*j+1); // wx
                Ke(2+6*i,  2+6*j+2) = Ke_p(3*i,  3*j+2); // wy
                Ke(2+6*i+1,2+6*j)   = Ke_p(3*i+1,3*j);   // xw
                Ke(2+6*i+1,2+6*j+1) = Ke_p(3*i+1,3*j+1); // xx
                Ke(2+6*i+1,2+6*j+2) = Ke_p(3*i+1,3*j+2); // xy
                Ke(2+6*i+2,2+6*j)   = Ke_p(3*i+2,3*j);   // yw
                Ke(2+6*i+2,2+6*j+1) = Ke_p(3*i+2,3*j+1); // yx
                Ke(2+6*i+2,2+6*j+2) = Ke_p(3*i+2,3*j+2); // yy
            }
        }

        Real max_value;
        for (int zi = 0; zi < 3; zi++)
        {
            for (int zj = 0; zj < 3; zj++)
            {
                // search for max value in uv-matrix
                max_value = Ke_m(2*zi,2*zj); // begin with uu value
                max_value = std::max(max_value, Ke_m(2*zi+1,2*zj+1)); // test for vv
                // search for max value in w-matrix
                max_value = std::max(max_value, Ke_p(3*zi,  3*zj)); // test for ww
                max_value = std::max(max_value, Ke_p(3*zi+1,3*zj+1)); // test for t_x t_x
                max_value = std::max(max_value, Ke_p(3*zi+2,3*zj+2)); // test for t_y t_y
                // take max from both and divide it by 1000
                max_value /= 1000.0;
                // set it at corresponding place
                Ke(5+6*zi,5+6*zj) = max_value;
            }
        }

        if (debug) {
std::cout << "K_m:\n";
Ke_m.print(std::cout);
std::cout << std::endl;
std::cout << "K_p:\n";
Ke_p.print(std::cout);
std::cout << std::endl;
        }

        Fe.resize(18);

        const BoundaryInfo binfo = mesh.get_boundary_info();
        DenseVector<Real> arg;
        for (unsigned int side = 0; side < elem->n_sides(); side++)
        {
            if (!binfo.has_boundary_id(elem,side,0)) // only process non-sticky nodes
            {
                dof_id_type id = dof_indices_u[side];
                if (debug) std::cout << "id_u = " << id << ", id_v = " << dof_indices_v[side] << ", id_w = " << dof_indices_w[side] << "\n";
                arg = forces[id/6];
                //if (!processedNodes[id/6])
                {
                    if (debug) std::cout << "force = " << arg(0) << "," << arg(1) << "," << arg(2) << "\n";
                    // forces don't need to be transformed since we bring the local stiffness matrix back to global co-sys
                    // directly in libmesh-format:
                    Fe(side)   = arg(0);//thickness*A_tri*arg(0)/3.0; // u_i
                    Fe(side+3) = arg(1);//thickness*A_tri*arg(1)/3.0; // v_i
                    Fe(side+6) = 2*A_tri*arg(2);//thickness*A_tri*arg(2)/3.0; // w_i

                    processedNodes[id/6] = true;
                }
            }
        }
        /*
        arg = forces[dof_indices_u[0]/6];
        Fe(9) += A_tri*arg(2)/24.0*(-dphi_p(0,1)+dphi_p(1,1));
        Fe(12) += A_tri*arg(2)/24.0*(-dphi_p(1,0)+dphi_p(0,0));
        arg = forces[dof_indices_u[1]/6];
        Fe(10) += A_tri*arg(2)/24.0*(-dphi_p(2,1)+dphi_p(0,1));
        Fe(13) += A_tri*arg(2)/24.0*(-dphi_p(0,0)+dphi_p(2,0));
        arg = forces[dof_indices_u[2]/6];
        Fe(11) += A_tri*arg(2)/24.0*( dphi_p(2,1)-dphi_p(1,1));
        Fe(14) += A_tri*arg(2)/24.0*( dphi_p(1,0)-dphi_p(2,0));
        */

        if (debug) {
std::cout << "Fe:\n";
Fe.print(std::cout);
std::cout << std::endl;
        }

        if (debug) {
std::cout << "Ke local:\n";
Ke.print(std::cout);
std::cout << std::endl;
        }

        // transform Ke from local back to global with transformation matrix T:
        DenseMatrix<Real> KeSub(6,6);
        DenseMatrix<Real> KeNew(18,18);
        DenseMatrix<Real> TSub(6,6);
        for (int k = 0; k < 2; k++)
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    TSub(3*k+i,3*k+j) = transTri(i,j);

        if (debug) {
        std::cout << "TSub:\n";
        TSub.print(std::cout);
        std::cout << std::endl;
        }

        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                for (int k = 0; k < 6; k++)
                    for (int l = 0; l < 6; l++)
                        KeSub(k,l) = Ke(i*6+k,j*6+l);

                if (debug) {
                std::cout << "KeSub(" << i << "," << j << "):\n";
                KeSub.print(std::cout);
                std::cout << std::endl;
                }

                KeSub.right_multiply(TSub);
                KeSub.left_multiply_transpose(TSub);

                if (debug) {
                std::cout << "Transformed:\n";
                KeSub.print(std::cout);
                std::cout << std::endl;
                }

                for (int k = 0; k < 6; k++)
                    for (int l = 0; l < 6; l++)
                        KeNew(i*6+k,j*6+l) = KeSub(k,l);
            }
        }

        if (debug) {
        std::cout << "Ke global:\n";
        KeNew.print(std::cout);
        std::cout << std::endl;
        }

        for (int alpha = 0; alpha < 6; alpha++)
        {
            for (int beta = 0; beta < 6; beta++)
            {
                for (int i = 0; i < 3; i++)
                {
                    for (int j = 0; j < 3; j++)
                    {
                        Ke(3*alpha+i,3*beta+j) = KeNew(6*i+alpha,6*j+beta);
                    }
                }
            }
        }

        if (debug) {
        std::cout << "Ke global in libmesh-format:\n";
        Ke.print(std::cout);
        std::cout << std::endl;
        }

        dof_map.constrain_element_matrix_and_vector (Ke, Fe, dof_indices);

        system.matrix->add_matrix (Ke, dof_indices);
        system.rhs->add_vector    (Fe, dof_indices);

        if (debug) std::cout << "END ELEMENT\n";
    }

}

void eval_B(DenseMatrix<Real>& C, Real L1, Real L2, DenseMatrix<Real>& dphi_p, DenseMatrix<Real> &out)
{
    out.resize(3,9);

    Real mu1 = (C(0,0)-C(0,1))/C(0,2);
    Real mu2 = (C(0,2)-C(0,0))/C(0,1);
    Real mu3 = (C(0,1)-C(0,2))/C(0,0);

    Real L3 = 1-L1-L2;
    Real f13mu1 = 1+3*mu1;
    Real f13mu2 = 1+3*mu2;
    Real f13mu3 = 1+3*mu3;
    Real f1m3mu3 = 1-3*mu3;
    Real fm13mu2 = -1+3*mu2;
    Real fm1m3mu3 = -1-3*mu3;
    Real f1mmu1 = 1-mu1;
    Real f1mmu2 = 1-mu2;
    Real f1mmu3 = 1-mu3;

    Real a = 3*f1mmu3*L1-f13mu3*L2+f13mu3*L3;
    Real b = 3*f1mmu2*L3-f13mu2*L1+f13mu2*L2;
    Real c = 3*f1mmu1*L2-f13mu1*L3+f13mu1*L1;

    out(0,0) = 6
            +L2*(-4-2*a)
            +4*f1m3mu3*(L2*L3-L1*L2)
            -12*L1
            +2*L2*b
            +8*(L2*L3-L1*L2);

    out(0,1) = -dphi_p(1,1)*(-2+6*L1+4*L2-L2*b-4*L2*L3+4*L1*L2)
            -dphi_p(0,1)*(2*L2-L2*a+L2*L3*2*f1m3mu3-L1*L2*2*f1m3mu3);

    out(0,2) =  dphi_p(1,0)*(-2+6*L1+4*L2-L2*b-4*L2*L3+4*L1*L2)
            +dphi_p(0,0)*(2*L2-L2*a+L2*L3*2*f1m3mu3-L1*L2*2*f1m3mu3);

    out(0,3) = -2*L2*c
            +4*f13mu1*(L2*L3-L1*L2)
            -4*L2
            +2*L2*a
            +4*f1m3mu3*(-L2*L3+L1*L2);

    out(0,4) = -dphi_p(0,1)*(2*L2-L2*a+L2*L3*2*f1m3mu3-L1*L2*2*f1m3mu3)
            -dphi_p(2,1)*(-L2*c+L2*L3*2*f13mu1-L1*L2*2*f13mu1);

    out(0,5) = dphi_p(0,0)*(2*L2-L2*a+L2*L3*2*f1m3mu3-L1*L2*2*f1m3mu3)
            +dphi_p(2,0)*(-L2*c+L2*L3*2*f13mu1-L1*L2*2*f13mu1);

    out(0,6) = -6
            +12*L1
            +8*L2
            -2*L2*b
            +8*(L1*L2-L2*L3)
            +2*L2*c
            +4*f13mu1*(L1*L2-L2*L3);

    out(0,7) = -dphi_p(2,1)*(-L2*c+L2*L3*2*f13mu1-L1*L2*2*f13mu1)
            -dphi_p(1,1)*(-4+6*L1+4*L2-L2*b-4*L2*L3+4*L1*L2);

    out(0,8) = dphi_p(2,0)*(-L2*c+L2*L3*2*f13mu1-L1*L2*2*f13mu1)
            +dphi_p(1,0)*(-4+6*L1+4*L2-L2*b-4*L2*L3+4*L1*L2);

    out(1,0) = -2*L1*a
            +2*L1*L3*2*fm1m3mu3
            -2*L1*L2*2*fm1m3mu3
            -4*L1+2*L1*b
            -2*L1*L3*2*fm13mu2
            +2*L1*L2*2*fm13mu2;

    out(1,1) = -dphi_p(1,1)*(2*L1-1*L1*b+1*L1*L3*2*fm13mu2-1*L1*L2*2*fm13mu2)
            -dphi_p(0,1)*(-1*L1*a+1*L1*L3*2*fm1m3mu3-1*L1*L2*2*fm1m3mu3);

    out(1,2) = dphi_p(1,0)*(2*L1-1*L1*b+1*L1*L3*2*fm13mu2-1*L1*L2*2*fm13mu2)
            +dphi_p(0,0)*(-1*L1*a+1*L1*L3*2*fm1m3mu3-1*L1*L2*2*fm1m3mu3);

    out(1,3) = 6
            -12*L2
            -4*L1-2*L1*c
            +8*L3*L1
            -8*L1*L2
            +2*L1*a
            -2*L1*L3*2*fm1m3mu3
            +2*L1*L2*2*fm1m3mu3;

    out(1,4) = -dphi_p(0,1)*(-1*L1*a+1*L1*L3*2*fm1m3mu3-1*L1*L2*2*fm1m3mu3)
            -dphi_p(2,1)*(-6*L2+2-2*L1-1*L1*c+4*L3*L1-4*L1*L2);

    out(1,5) = dphi_p(0,0)*(-1*L1*a+1*L1*L3*2*fm1m3mu3-1*L1*L2*2*fm1m3mu3)
            +dphi_p(2,0)*(-6*L2+2-2*L1-1*L1*c+4*L3*L1-4*L1*L2);

    out(1,6) = -6
            +8*L1
            -2*L1*b
            +2*L1*L3*2*fm13mu2
            -2*L1*L2*2*fm13mu2
            +12*L2
            +2*L1*c
            -8*L3*L1
            +8*L1*L2;

    out(1,7) = -dphi_p(2,1)*(-6*L2+4-2*L1-1*L1*c+4*L3*L1-4*L1*L2)
            -dphi_p(1,1)*(2*L1-1*L1*b+1*L1*L3*2*fm13mu2-1*L1*L2*2*fm13mu2);

    out(1,8) = dphi_p(2,0)*(-6*L2+4-2*L1-1*L1*c+4*L3*L1-4*L1*L2)
            +dphi_p(1,0)*(2*L1-1*L1*b+1*L1*L3*2*fm13mu2-1*L1*L2*2*fm13mu2);

    out(2,0) = 2
           -4*L1
           +L3*a
           -L2*a
           +L2*L3*2*fm1m3mu3
           -L1*a
           -L1*L2*2*fm1m3mu3+L1*L3*2*f1m3mu3
           -L1*L2*2*f1m3mu3
           -4*L2
           -L3*b
           +L2*b
           -L2*L3*2*fm13mu2
           +L1*b
           +L1*L2*2*fm13mu2
           +4*L3*L1
           -4*L1*L2;

    out(2,1) = -dphi_p(1,1)*(-1
                   +4*L1
                   +2*L2
                   +0.5*L3*b
                   -0.5*L2*b
                   +0.5*L2*L3*2*fm13mu2
                   -0.5*L1*b
                   -0.5*L1*L2*2*fm13mu2
                   -2*L3*L1
                   +2*L1*L2)
             -dphi_p(0,1)*(2*L1
                 +0.5*L3*a
                 -0.5*L2*a
                 +0.5*L2*L3*2*fm1m3mu3
                 -0.5*L1*a
                 -0.5*L1*L2*2*fm1m3mu3
                 +0.5*L1*L3*2*f1m3mu3
                 -0.5*L1*L2*2*f1m3mu3);

    out(2,2) = dphi_p(1,0)*(-1
                  +4*L1
                  +2*L2
                  +0.5*L3*b
                  -0.5*L2*b
                  +0.5*L2*L3*2*fm13mu2
                  -0.5*L1*b
                  -0.5*L1*L2*2*fm13mu2
                  -2*L3*L1
                  +2*L1*L2)
            +dphi_p(0,0)*(2*L1
                  +0.5*L3*a
                  -0.5*L2*a
                  +0.5*L2*L3*2*fm1m3mu3
                  -0.5*L1*a
                  -0.5*L1*L2*2*fm1m3mu3
                  +0.5*L1*L3*2*f1m3mu3
                  -0.5*L1*L2*2*f1m3mu3);

    out(2,3) = 2
            -4*L2
            +L3*c
            -L2*c
            +4*L2*L3
            -L1*c
            -4*L1*L2
            +L1*L3*2*f13mu1
            -L1*L2*2*f13mu1
            -4*L1
            -L3*a
            +L2*a
            -L2*L3*2*fm1m3mu3
            +L1*a
            +L1*L2*2*fm1m3mu3
            -L1*L3*2*f1m3mu3
            +L1*L2*2*f1m3mu3;

    out(2,4) = -dphi_p(0,1)*(2*L1
                   +0.5*L3*a
                   -0.5*L2*a
                   +0.5*L2*L3*2*fm1m3mu3
                   -0.5*L1*a
                   -0.5*L1*L2*2*fm1m3mu3
                   +0.5*L1*L3*2*f1m3mu3
                   -0.5*L1*L2*2*f1m3mu3
                   -1)
             -dphi_p(2,1)*(-2*L2
                   +0.5*L3*c
                   -0.5*L2*c
                   +2*L2*L3
                   -0.5*L1*c
                   -2*L1*L2
                   +0.5*L1*L3*2*f13mu1
                   -0.5*L1*L2*2*f13mu1
                   );

    out(2,5) = dphi_p(0,0)*(2*L1
                  +0.5*L3*a
                  -0.5*L2*a
                  +0.5*L2*L3*2*fm1m3mu3
                  -0.5*L1*a
                  -0.5*L1*L2*2*fm1m3mu3
                  +0.5*L1*L3*2*f1m3mu3
                  -0.5*L1*L2*2*f1m3mu3
                  -1)
             +dphi_p(2,0)*(-2*L2
                   +0.5*L3*c
                   -0.5*L2*c
                   +2*L2*L3
                   -0.5*L1*c
                   -2*L1*L2
                   +0.5*L1*L3*2*f13mu1
                   -0.5*L1*L2*2*f13mu1
                   );

    out(2,6) = -4
             +8*L1
             +8*L2
             +L3*b
             -L2*b
             +L2*L3*2*fm13mu2
             -L1*b
             -L1*L2*2*fm13mu2
             -4*L3*L1
             +8*L1*L2
             -L3*c
             +L2*c
             -4*L2*L3
             +L1*c
             -L1*L3*2*f13mu1
             +L1*L2*2*f13mu1;

    out(2,7) = -dphi_p(2,1)*(-2*L2
                   +0.5*L3*c
                   -0.5*L2*c
                   +2*L2*L3
                   -0.5*L1*c
                   -2*L1*L2
                   +0.5*L1*L3*2*f13mu1
                   -0.5*L1*L2*2*f13mu1
                   +1
                   )
             -dphi_p(1,1)*(-2
                   +4*L1
                   +2*L2
                   +0.5*L3*b
                   -0.5*L2*b
                   +0.5*L2*L3*2*fm13mu2
                   -0.5*L1*b
                   -0.5*L1*L2*2*fm13mu2
                   -2*L3*L1
                   +2*L1*L2
                   );

    out(2,8) = dphi_p(2,0)*(-2*L2
                  +0.5*L3*c
                  -0.5*L2*c
                  +2*L2*L3
                  -0.5*L1*c
                  -2*L1*L2
                  +0.5*L1*L3*2*f13mu1
                  -0.5*L1*L2*2*f13mu1
                  +1
                  )
            +dphi_p(1,0)*(-2
                  +4*L1
                  +2*L2
                  +0.5*L3*b
                  -0.5*L2*b
                  +0.5*L2*L3*2*fm13mu2
                  -0.5*L1*b
                  -0.5*L1*L2*2*fm13mu2
                  -2*L3*L1
                  +2*L1*L2
                 );
    for (int i = 0; i < 9; i++)
        out(2,i) *= 2.0;

    /*
    // P = C(,0 q=C(,1 t=C(,2 r=C(,3
    // 4 = C(0, 5=C(1, 6=C(2,
    out.resize(3,9);
    Real z = 1.0 - 2.0*x;
    Real w = 1.0 - 2.0*y;
    DenseVector<Real> Hx_xhi(9), Hx_eta(9), Hy_xhi(9), Hy_eta(9);

    Hx_xhi(0) = C(2,0)*z + (C(1,0)-C(2,0))*y;
    Hx_xhi(1) = C(2,1)*z - (C(1,1)+C(2,1))*y;
    Hx_xhi(2) = -4.0+6.0*(x+y)+C(2,3)*z-(C(1,3)+C(2,3))*y;
    Hx_xhi(3) = -C(2,0)*z + (C(0,0)+C(2,0))*y;
    Hx_xhi(4) = C(2,1)*z-(C(2,1)-C(0,1))*y;
    Hx_xhi(5) = -2.0+6.0*x+C(2,3)*z+(C(0,3)-C(2,3))*y;
    Hx_xhi(6) = -(C(1,0)+C(0,0))*y;
    Hx_xhi(7) = (C(0,1)-C(1,1))*y;
    Hx_xhi(8) = (C(1,3)-C(0,3))*y;

    Hx_eta(0) = -C(1,0)*w - (C(2,0)-C(1,0))*x;
    Hx_eta(1) = C(1,1)*w - (C(1,1)+C(2,1))*x;
    Hx_eta(2) = -4.0+6.0*(x+y)+C(1,3)*w-(C(1,3)+C(2,3))*x;
    Hx_eta(3) = x*(C(0,0)+C(2,0));
    Hx_eta(4) = x*(C(0,1)-C(2,1));
    Hx_eta(5) = -x*(C(2,3)-C(0,3));
    Hx_eta(6) = C(1,0)*w-x*(C(0,0)+C(1,0));
    Hx_eta(7) = C(1,1)*w+x*(C(0,1)-C(1,1));
    Hx_eta(8) = -2.0+6.0*y+C(1,3)*w+x*(C(0,3)-C(1,3));

    Hy_xhi(0) = C(2,2)*z + (C(1,2)-C(2,2))*y;
    Hy_xhi(1) = 1.0+C(2,3)*z-(C(1,3)+C(2,3))*y;
    Hy_xhi(2) = -C(2,1)*z+(C(1,1)+C(2,1))*y;
    Hy_xhi(3) = -C(2,2)*z+(C(0,2)+C(2,2))*y;
    Hy_xhi(4) = -1.0+C(2,3)*z+(C(0,3)-C(2,3))*y;
    Hy_xhi(5) = -C(2,1)*z-(C(0,1)-C(2,1))*y;
    Hy_xhi(6) = -(C(0,2)+C(1,2))*y;
    Hy_xhi(7) = (C(0,3)-C(1,3))*y;
    Hy_xhi(8) = -(C(0,1)-C(1,1))*y;

    Hy_eta(0) = -C(1,2)*w-x*(C(2,2)-C(1,2));
    Hy_eta(1) = 1.0+C(1,3)*w-x*(C(1,3)+C(2,3));
    Hy_eta(2) = -C(1,1)*w+x*(C(1,1)+C(2,1));
    Hy_eta(3) = x*(C(0,2)+C(2,2));
    Hy_eta(4) = x*(C(0,3)-C(2,3));
    Hy_eta(5) = -x*(C(0,1)-C(2,1));
    Hy_eta(6) = C(1,2)*w-x*(C(0,2)+C(1,2));
    Hy_eta(7) = -1.0+C(1,3)*w+x*(C(0,3)-C(1,3));
    Hy_eta(8) = -C(1,1)*w-x*(C(0,1)-C(1,1));

    for (int i = 0; i < 9; i++)
    {
        out(0,i) =  dphi(1,1)*Hx_xhi(i) + dphi(0,1)*Hx_eta(i);
        out(1,i) = -dphi(1,0)*Hy_xhi(i) - dphi(0,0)*Hy_eta(i);
        out(2,i) = -dphi(1,0)*Hx_xhi(i) - dphi(0,0)*Hx_eta(i) + dphi(1,1)*Hy_xhi(i) + dphi(0,1)*Hy_eta(i);
    }
    */
    /*
    // eigentlich alternative Lösung, klappt aber nicht
    // a = C(,0 b=C(,1 c=C(,2 d=C(,3 e=C(,4
    // 4 = C(0, 5=C(1, 6=C(2,
    Hx_xhi(0) =  6.0*C(2,0)*z-6.0*C(2,0)*x+6.0*C(1,0)*y;
    Hx_xhi(1) = -4.0*C(1,1)*y+4.0*C(2,1)*z-4.0*C(2,1)*x;
    Hx_xhi(2) = -3.0+4.0*x+4.0*y+4.0*C(1,2)*y-4.0*C(2,2)*z+4.0*C(2,2)*x;
    Hx_xhi(3) =  6.0*C(0,0)*y-6.0*C(2,0)*z+6.0*C(2,0)*x;
    Hx_xhi(4) =  4.0*C(2,1)*z-4.0*C(2,1)*x+4.0*C(0,1)*y;
    Hx_xhi(5) =  4.0*x-1.0-4.0*C(2,2)*z+4.0*C(2,2)*x-4.0*C(0,2)*y;
    Hx_xhi(6) = -6.0*C(1,0)*y-6.0*C(0,0)*y;
    Hx_xhi(7) =  4.0*C(0,1)*y-4.0*C(1,1)*y;
    Hx_xhi(8) = -4.0*C(0,2)*y+4.0*C(1,2)*y;

    Hx_eta(0) = -6.0*C(2,0)*x-6.0*C(1,0)*z+6.0*C(1,0)*y;
    Hx_eta(1) =  4.0*C(1,1)*z-4.0*C(1,1)*y-4.0*C(2,1)*x;
    Hx_eta(2) = -3.0+4.0*x+4.0*y-4.0*C(1,2)*z+4.0*C(1,2)*y+4.0*C(2,2)*x;
    Hx_eta(3) =  6.0*C(0,0)*x+6.0*C(2,0)*x;
    Hx_eta(4) = -4.0*C(2,1)*x+4.0*C(0,1)*x;
    Hx_eta(5) =  4.0*C(2,2)*x-4.0*C(0,2)*x;
    Hx_eta(6) =  6.0*C(1,0)*z-6.0*C(1,0)*y-6.0*C(0,0)*x;
    Hx_eta(7) =  4.0*C(0,1)*x+4.0*C(1,1)*z-4.0*C(1,1)*y;
    Hx_eta(8) = -4.0*C(0,2)*x-4.0*C(1,2)*z+4.0*C(1,2)*y;

    Hy_xhi(0) =  6.0*C(2,3)*z-6.0*C(2,3)*x+6.0*C(1,3)*y;
    Hy_xhi(1) =  3.0-4.0*x-4.0*y-4.0*C(1,4)*y+4.0*C(2,4)*z-4.0*C(2,4)*x;
    Hy_xhi(2) =  4.0*C(1,1)*y-4.0*C(2,1)*z+4.0*C(2,1)*x;
    Hy_xhi(3) =  6.0*C(0,3)*y-6.0*C(2,3)*z+6.0*C(2,3)*x;
    Hy_xhi(4) =  4.0*C(2,4)*z-4.0*C(2,4)*x+4.0*C(0,4)*y;
    Hy_xhi(5) = -4.0*C(2,1)*z+4.0*C(2,1)*x-4.0*C(0,1)*y;
    Hy_xhi(6) = -6.0*C(1,3)*y-6.0*C(0,3)*y;
    Hy_xhi(7) =  4.0*C(0,4)*y-4.0*C(1,4)*y;
    Hy_xhi(8) = -4.0*C(0,1)*y+4.0*C(1,1)*y;

    Hy_eta(0) = -6.0*C(2,3)*x-6.0*C(1,3)*z+6.0*C(1,3)*y;
    Hy_eta(1) =  3.0-4.0*x-4.0*y+4.0*C(1,4)*z-4.0*C(1,4)*y-4.0*C(2,4)*x;
    Hy_eta(2) = -4.0*C(1,1)*z+4.0*C(1,1)*y+4.0*C(2,1)*x;
    Hy_eta(3) =  6.0*C(0,3)*x+6.0*C(2,3)*x;
    Hy_eta(4) = -4.0*C(2,4)*x+4.0*C(0,4)*x;
    Hy_eta(5) =  4.0*C(2,1)*x-4.0*C(0,1)*x;
    Hy_eta(6) =  6.0*C(1,3)*z-6.0*C(1,3)*y-6.0*C(0,3)*x;
    Hy_eta(7) =  4.0*C(0,4)*x+4.0*C(1,4)*z-4.0*C(1,4)*y;
    Hy_eta(8) = -4.0*C(0,1)*x-4.0*C(1,1)*z+4.0*C(1,1)*y;

    for (int i = 0; i < 9; i++)
    {
        out(0,i) =  dphi(1,1)*Hx_xhi(i) + dphi(0,1)*Hx_eta(i);
        out(1,i) = -dphi(1,0)*Hy_xhi(i) - dphi(0,0)*Hy_eta(i);
        out(2,i) = -dphi(1,0)*Hx_xhi(i) - dphi(0,0)*Hx_eta(i) + dphi(1,1)*Hy_xhi(i) + dphi(0,1)*Hy_eta(i);
    }
    */

    /*
    // ehemalige Lösung. Klappt, es sind aber andere Werte; könnte also korrekt oder eben falsch sein
    // a = C(0, b=C(1, c=C(2, d=C(3, e=C(4,
    // 4 = C(,0 5=C(,1 6=C(,2
    out.resize(9,4);
    Real z = 1.0 - x - y;
    out(0,0) = 6.0*C(2,0)*z - 6.0*C(2,0)*x + 6.0*C(1,0)*y;
    out(0,1) =-6.0*C(2,0)*x - 6.0*C(1,0)*z + 6.0*C(1,0)*y;
    out(0,2) = 6.0*C(2,3)*z - 6.0*C(2,3)*x + 6.0*C(1,3)*y;
    out(0,3) =-6.0*C(2,3)*x - 6.0*C(1,3)*z + 6.0*C(1,3)*y;

    out(1,0) =-4.0*C(1,1)*y + 4.0*C(2,1)*z - 4.0*C(2,1)*x;
    out(1,1) = 4.0*C(1,1)*z - 4.0*C(1,1)*y - 4.0*C(2,1)*x;
    out(1,2) =3.0 - 4.0*x - 4.0*y - 4.0*C(1,4)*y + 4.0*C(2,4)*z - 4.0*C(2,4)*x;
    out(1,3) =3.0 - 4.0*x - 4.0*y + 4.0*C(1,4)*z - 4.0*C(1,4)*y - 4.0*C(2,4)*x;

    out(2,0) =-3.0 + 4.0*x + 4.0*y + 4.0*C(1,2)*y - 4.0*C(2,2)*z + 4.0*C(2,2)*x;
    out(2,1) =-3.0 + 4.0*x + 4.0*y - 4.0*C(1,2)*z + 4.0*C(1,2)*y + 4.0*C(2,2)*x;
    out(2,2) = 4.0*C(1,1)*y - 4.0*C(2,1)*z + 4.0*C(2,1)*x;
    out(2,3) =-4.0*C(1,1)*z + 4.0*C(1,1)*y + 4.0*C(2,1)*x;

    out(3,0) =6.0*C(0,0)*y - 6.0*C(2,0)*z + 6.0*C(2,0)*x;
    out(3,1) =6.0*C(0,0)*x + 6.0*C(2,0)*x;
    out(3,2) =6.0*C(0,3)*y - 6.0*C(2,3)*z + 6.0*C(2,3)*x;
    out(3,3) =6.0*C(0,3)*x + 6.0*C(2,3)*x;

    out(4,0) = 4.0*C(2,1)*z - 4.0*C(2,1)*x + 4.0*C(0,1)*y;
    out(4,1) =-4.0*C(2,1)*x + 4.0*C(0,1)*x;
    out(4,2) =-4.0*x + 1.0 - 4.0*C(2,4)*z + 4.0*C(2,4)*x + 4.0*C(0,4)*y;
    out(4,3) = 4.0*C(2,4)*x + 4.0*C(0,4)*x;

    out(5,0) = 4.0*x - 1.0 - 4.0*C(2,2)*z + 4.0*C(2,2)*x - 4.0*C(0,2)*y;
    out(5,1) = 4.0*C(2,2)*x - 4.0*C(0,2)*x;
    out(5,2) =-4.0*C(2,1)*z + 4.0*C(2,1)*x - 4.0*C(0,1)*y;
    out(5,3) = 4.0*C(2,1)*x - 4.0*C(0,1)*x;

    out(6,0) =-6.0*C(1,0)*y - 6.0*C(0,0)*y;
    out(6,1) = 6.0*C(1,0)*z - 6.0*C(1,0)*y - 6.0*C(0,0)*x;
    out(6,2) =-6.0*C(1,3)*y - 6.0*C(0,3)*y;
    out(6,3) = 6.0*C(1,3)*z - 6.0*C(1,3)*y - 6.0*C(0,3)*x;

    out(7,0) = 4.0*C(0,1)*y - 4.0*C(1,1)*y;
    out(7,1) = 4.0*C(0,1)*x + 4.0*C(1,1)*z - 4.0*C(1,1)*y;
    out(7,2) = 4.0*C(0,4)*y - 4.0*C(1,4)*y;
    out(7,3) =-4.0*y + 1.0 + 4.0*C(0,4)*x + 4.0*C(1,4)*z - 4.0*C(1,4)*y;

    out(8,0) = 4.0*y - 1.0 - 4.0*C(0,2)*x - 4.0*C(1,2)*z + 4.0*C(1,2)*y;
    out(8,1) =-4.0*C(0,2)*y + 4.0*C(1,2)*y;
    out(8,2) =-4.0*C(0,1)*y + 4.0*C(1,1)*y;
    out(8,3) =-4.0*C(0,1)*x - 4.0*C(1,1)*z + 4.0*C(1,1)*y;
    */
}
