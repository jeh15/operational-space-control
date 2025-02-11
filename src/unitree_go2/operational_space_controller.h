#include <filesystem>
#include <vector>
#include <string>
#include <cstdlib>

#include "mujoco/mujoco.h"
#include "Eigen/Dense"

#include "src/unitree_go2/autogen/autogen_functions.h"
#include "src/unitree_go2/autogen/autogen_defines.h"
#include "src/unitree_go2/utilities.h"
#include "src/utilities.h"

using namespace constants;

// Anonymous Namespace for shorthand constants:
namespace {
    // s_size : Size of fully spatial vector representation for all bodies
    constexpr int s_size = 6 * model::body_ids_size;
    // p_size : Size of translation component to a spatial vector:
    constexpr int p_size = 3 * model::body_ids_size;
    // r_size : Size of rotation component to a spatial vector:
    constexpr int r_size = 3 * model::body_ids_size;

    template <int Rows_, int Cols_>
    using Matrix = Eigen::Matrix<double, Rows_, Cols_, Eigen::RowMajor>;
    
    template <int Rows_>
    using Vector = Eigen::Matrix<double, Rows_, 1>;

    template <int Rows_, int Cols_>
    using MatrixColMajor = Eigen::Matrix<double, Rows_, Cols_, Eigen::ColMajor>;

    // Map Casadi Functions to FunctionOperations Struct:
    FunctionOperations Aeq_ops = {
        Aeq_incref,
        Aeq_checkout,
        Aeq,
        Aeq_release,
        Aeq_decref
    };

    FunctionOperations beq_ops{
        .func_incref = &beq_incref,
        .func_checkout = &beq_checkout,
        .eval_func = &beq,
        .func_release = &beq_release,
        .func_decref = &beq_decref
    };

    FunctionOperations Aineq_ops{
        .func_incref = &Aineq_incref,
        .func_checkout = &Aineq_checkout,
        .eval_func = &Aineq,
        .func_release = &Aineq_release,
        .func_decref = &Aineq_decref
    };

    FunctionOperations bineq_ops{
        .func_incref = &bineq_incref,
        .func_checkout = &bineq_checkout,
        .eval_func = &bineq,
        .func_release = &bineq_release,
        .func_decref = &bineq_decref
    };

    FunctionOperations H_ops{
        .func_incref = &H_incref,
        .func_checkout = &H_checkout,
        .eval_func = &H,
        .func_release = &H_release,
        .func_decref = &H_decref
    };

    FunctionOperations f_ops{
        .func_incref = &f_incref,
        .func_checkout = &f_checkout,
        .eval_func = &f,
        .func_release = &f_release,
        .func_decref = &f_decref
    };

    // Casadi Functions
    using AeqParams = 
        FunctionParams<Aeq_SZ_ARG, Aeq_SZ_RES, Aeq_SZ_IW, Aeq_SZ_W, optimization::Aeq_rows, optimization::Aeq_cols, optimization::Aeq_sz, 4>;
    using beqParams =
        FunctionParams<beq_SZ_ARG, beq_SZ_RES, beq_SZ_IW, beq_SZ_W, optimization::beq_sz, 1, optimization::beq_sz, 4>;
    using AineqParams =
        FunctionParams<Aineq_SZ_ARG, Aineq_SZ_RES, Aineq_SZ_IW, Aineq_SZ_W, optimization::Aineq_rows, optimization::Aineq_cols, optimization::Aineq_sz, 1>;
    using bineqParams =
        FunctionParams<bineq_SZ_ARG, bineq_SZ_RES, bineq_SZ_IW, bineq_SZ_W, optimization::bineq_sz, 1, optimization::bineq_sz, 1>;
    using HParams =
        FunctionParams<H_SZ_ARG, H_SZ_RES, H_SZ_IW, H_SZ_W, optimization::H_rows, optimization::H_cols, optimization::H_sz, 4>;
    using fParams =
        FunctionParams<f_SZ_ARG, f_SZ_RES, f_SZ_IW, f_SZ_W, optimization::f_sz, 1, optimization::f_sz, 4>;
}

struct OSCData {
    Matrix<model::nv_size, model::nv_size> mass_matrix;    
    Vector<model::nv_size> coriolis_matrix;
    Matrix<model::nv_size, optimization::z_size> contact_jacobian;
    Matrix<s_size, model::nv_size> taskspace_jacobian;
    Vector<s_size> taskspace_bias;
    Vector<model::contact_site_ids_size> contact_mask;
    Vector<model::nq_size> previous_q;
    Vector<model::nv_size> previous_qd;
};

struct OptimizationData {
    MatrixColMajor<optimization::H_rows, optimization::H_cols> H;
    Vector<optimization::f_sz> f;
    MatrixColMajor<optimization::Aeq_rows, optimization::Aeq_cols> Aeq;
    Vector<optimization::beq_sz> beq;
    Matrix<optimization::Aineq_rows, optimization::Aineq_cols> Aineq;
    Vector<optimization::bineq_sz> bineq;
};

class OperationalSpaceController {
    public:
        OperationalSpaceController() {}
        ~OperationalSpaceController() {}

        void initialize(std::filesystem::path xml_path) {
            char error[1000];
            mj_model = mj_loadXML(xml_path.c_str(), nullptr, error, 1000);
            if( !mj_model ) {
                printf("%s\n", error);
                std::exit(EXIT_FAILURE);
            }
            // Physics timestep:
            mj_model->opt.timestep = 0.002;
            
            mj_data = mj_makeData(mj_model);

            for(const std::string_view& site : model::site_list) {
                std::string site_str = std::string(site);
                int id = mj_name2id(mj_model, mjOBJ_SITE, site_str.data());
                assert(id != -1 && "Site not found in model.");
                sites.push_back(site_str);
                site_ids.push_back(id);
            }
            for(const std::string_view& site : model::noncontact_site_list) {
                std::string site_str = std::string(site);
                int id = mj_name2id(mj_model, mjOBJ_SITE, site_str.data());
                assert(id != -1 && "Site not found in model.");
                noncontact_sites.push_back(site_str);
                noncontact_site_ids.push_back(id);
            }
            for(const std::string_view& site : model::contact_site_list) {
                std::string site_str = std::string(site);
                int id = mj_name2id(mj_model, mjOBJ_SITE, site_str.data());
                assert(id != -1 && "Site not found in model.");
                contact_sites.push_back(site_str);
                contact_site_ids.push_back(id);
            }
            for(const std::string_view& body : model::body_list) {
                std::string body_str = std::string(body);
                int id = mj_name2id(mj_model, mjOBJ_BODY, body_str.data());
                assert(id != -1 && "Body not found in model.");
                bodies.push_back(body_str);
                body_ids.push_back(id);
            }
            // Assert Number of Sites and Bodies are equal:
            assert(site_ids.size() == body_ids.size() && "Number of Sites and Bodies must be equal.");
        }

        void close() {
            mj_deleteData(mj_data);
            mj_deleteModel(mj_model);
        }

        OSCData get_data(Eigen::Matrix<double, model::body_ids_size, 3, Eigen::RowMajor>& points) {
            // Mass Matrix:
            Matrix<model::nv_size, model::nv_size> mass_matrix = 
                Matrix<model::nv_size, model::nv_size>::Zero();
            mj_fullM(mj_model, mass_matrix.data(), mj_data->qM);

            // Coriolis Matrix:
            Vector<model::nv_size> coriolis_matrix = 
                Eigen::Map<Vector<model::nv_size>>(mj_data->qfrc_bias);

            // Generalized Positions and Velocities:
            Vector<model::nq_size> generalized_positions = 
                Eigen::Map<Vector<model::nq_size> >(mj_data->qpos);
            Vector<model::nv_size> generalized_velocities = 
                Eigen::Map<Vector<model::nv_size>>(mj_data->qvel);

            // Jacobian Calculation:
            Matrix<p_size, model::nv_size> jacobian_translation = 
                Matrix<p_size, model::nv_size>::Zero();
            Matrix<r_size, model::nv_size> jacobian_rotation = 
                Matrix<r_size, model::nv_size>::Zero();
            Matrix<p_size, model::nv_size> jacobian_dot_translation = 
                Matrix<p_size, model::nv_size>::Zero();
            Matrix<r_size, model::nv_size> jacobian_dot_rotation = 
                Matrix<r_size, model::nv_size>::Zero();
            for (int i = 0; i < model::body_ids_size; i++) {
                // Temporary Jacobian Matrices:
                Matrix<3, model::nv_size> jacp = Matrix<3, model::nv_size>::Zero();
                Matrix<3, model::nv_size> jacr = Matrix<3, model::nv_size>::Zero();
                Matrix<3, model::nv_size> jacp_dot = Matrix<3, model::nv_size>::Zero();
                Matrix<3, model::nv_size> jacr_dot = Matrix<3, model::nv_size>::Zero();

                // Calculate Jacobian:
                mj_jac(mj_model, mj_data, jacp.data(), jacr.data(), points.row(i).data(), body_ids[i]);

                // Calculate Jacobian Dot:
                mj_jacDot(mj_model, mj_data, jacp_dot.data(), jacr_dot.data(), points.row(i).data(), body_ids[i]);

                // Append to Jacobian Matrices:
                int row_offset = i * 3;
                for(int row_idx = 0; row_idx < 3; row_idx++) {
                    for(int col_idx = 0; col_idx < model::nv_size; col_idx++) {
                        jacobian_translation(row_idx + row_offset, col_idx) = jacp(row_idx, col_idx);
                        jacobian_rotation(row_idx + row_offset, col_idx) = jacr(row_idx, col_idx);
                        jacobian_dot_translation(row_idx + row_offset, col_idx) = jacp_dot(row_idx, col_idx);
                        jacobian_dot_rotation(row_idx + row_offset, col_idx) = jacr_dot(row_idx, col_idx);
                    }
                }
            }

            // Stack Jacobian Matrices: Taskspace Jacobian: [jacp; jacr], Jacobian Dot: [jacp_dot; jacr_dot]
            Matrix<s_size, model::nv_size> taskspace_jacobian = Matrix<s_size, model::nv_size>::Zero();
            Matrix<s_size, model::nv_size> jacobian_dot = Matrix<s_size, model::nv_size>::Zero();
            taskspace_jacobian << jacobian_translation, jacobian_rotation;
            jacobian_dot << jacobian_dot_translation, jacobian_dot_rotation;

            // Calculate Taskspace Bias Acceleration:
            Vector<s_size> taskspace_bias = Vector<s_size>::Zero();
            taskspace_bias = jacobian_dot * generalized_velocities;

            // Contact Jacobian: Shape (NV, 3 * num_contacts) 
            // This assumes contact frames are the last rows of the translation component of the taskspace_jacobian (jacobian_translation).
            // contact_jacobian = jacobian_translation[end-(3 * contact_site_ids_size):end, :].T
            Matrix<model::nv_size, optimization::z_size> contact_jacobian = 
                Matrix<model::nv_size, optimization::z_size>::Zero();

            contact_jacobian = jacobian_translation(
                Eigen::seq(Eigen::placeholders::end - Eigen::fix<optimization::z_size>, Eigen::placeholders::last),
                Eigen::placeholders::all
            ).transpose();

            // Contact Mask: Shape (num_contacts, 1)
            Vector<model::contact_site_ids_size> contact_mask = Vector<model::contact_site_ids_size>::Zero();
            double contact_threshold = 1e-3;
            for(int i = 0; i < model::contact_site_ids_size; i++) {
                auto contact = mj_data->contact[i];
                contact_mask(i) = contact.dist < contact_threshold;
            }

            OSCData osc_data{
                .mass_matrix = mass_matrix,
                .coriolis_matrix = coriolis_matrix,
                .contact_jacobian = contact_jacobian,
                .taskspace_jacobian = taskspace_jacobian,
                .taskspace_bias = taskspace_bias,
                .contact_mask = contact_mask,
                .previous_q = generalized_positions,
                .previous_qd = generalized_velocities  
            };

            return osc_data;
        }

        OptimizationData get_optimization_data(OSCData& osc_data, Matrix<model::site_ids_size, 6>& desired_taskspace_accelerations) {
            // Convert OSCData to Column Major for Casadi Functions:
            auto mass_matrix = matrix_utils::transformMatrix<model::nv_size, model::nv_size, matrix_utils::ColumnMajor>(osc_data.mass_matrix.data());
            auto coriolis_matrix = matrix_utils::transformMatrix<model::nv_size, 1, matrix_utils::ColumnMajor>(osc_data.coriolis_matrix.data());
            auto contact_jacobian = matrix_utils::transformMatrix<model::nv_size, optimization::z_size, matrix_utils::ColumnMajor>(osc_data.contact_jacobian.data());
            auto taskspace_jacobian = matrix_utils::transformMatrix<s_size, model::nv_size, matrix_utils::ColumnMajor>(osc_data.taskspace_jacobian.data());
            auto taskspace_bias = matrix_utils::transformMatrix<s_size, 1, matrix_utils::ColumnMajor>(osc_data.taskspace_bias.data());
            auto desired_taskspace_ddx = matrix_utils::transformMatrix<model::site_ids_size, 6, matrix_utils::ColumnMajor>(desired_taskspace_accelerations.data());
            
            // Evaluate Casadi Functions:
            auto Aeq_matrix = evaluate_function<AeqParams>(Aeq_ops, {design_vector, mass_matrix, coriolis_matrix, contact_jacobian});
            auto beq_matrix = evaluate_function<beqParams>(beq_ops, {design_vector, mass_matrix, coriolis_matrix, contact_jacobian});
            auto Aineq_matrix = evaluate_function<AineqParams>(Aineq_ops, {design_vector});
            auto bineq_matrix = evaluate_function<bineqParams>(bineq_ops, {design_vector});
            auto H_matrix = evaluate_function<HParams>(H_ops, {design_vector, desired_taskspace_ddx, taskspace_jacobian, taskspace_bias});
            auto f_matrix = evaluate_function<fParams>(f_ops, {design_vector, desired_taskspace_ddx, taskspace_jacobian, taskspace_bias});

            OptimizationData opt_data{
                .H = H_matrix,
                .f = f_matrix,
                .Aeq = Aeq_matrix,
                .beq = beq_matrix,
                .Aineq = Aineq_matrix,
                .bineq = bineq_matrix
            };

            return opt_data;
        }


        // void initialize_optimization(void) {
        //     // Initialize OSQP Settings:
        //     settings = (OSQPSettings *)c_malloc(sizeof(OSQPSettings));
        //     if (settings) {
        //         osqp_set_default_settings(settings);
        //     }

        //     // Initialize Optimization Matrices:
        //     // Design variable bounds:
        //     Matrix<optimization::design_vector_size, optimization::design_vector_size> Abox = 
        //         Matrix<optimization::design_vector_size, optimization::design_vector_size>::Identity();
        //     // Joint Acceleration Bounds:
        //     Vector<optimization::dv_size> dv_lb = Vector<optimization::dv_size>::Constant(-OSQP_INFTY)
        //     Vector<optimization::dv_size> dv_ub = Vector<optimization::dv_size>::Constant(OSQP_INFTY)
        //     // Control Input Bounds: TODO(jeh15) Make this match model
        //     Vector<optimization::u_size> u_lb = Vector<optimization::u_size>::Constant(PLACEHOLDER)
        //     Vector<optimization::u_size> u_lb = Vector<optimization::u_size>::Constant(PLACEHOLDER)
        //     // Reaction Force Bounds: z_ub remains uninitialized, it depends on the contact mask.
        //     Vector<optimization::z_size> z_lb = Vector<optimization::z_size>::Zero();
        //     Vector<optimization::z_size> z_ub = Vector<optimization::z_size>::Zero();
        //     z_lb << -OSQP_INFTY, -OSQP_INFTY, 0.0,
        //             -OSQP_INFTY, -OSQP_INFTY, 0.0,
        //             -OSQP_INFTY, -OSQP_INFTY, 0.0,
        //             -OSQP_INFTY, -OSQP_INFTY, 0.0;
        //     // Create lb and ub for Box constraints:
        //     Vector<optimization::design_vector_size> lb_box = Vector<optimization::design_vector_size>::Zero();
        //     Vector<optimization::design_vector_size> ub_box = Vector<optimization::design_vector_size>::Zero();
        //     lb_box << dv_lb, u_lb, z_lb;
        //     ub_box << dv_ub, u_ub, z_ub;

        //     // Inequality Constraints are constants:
        //     Matrix<optimization::Aineq_rows, optimization::Aineq_cols> Aineq = Aineq_function();
        //     Vector<optimization::bineq_sz> bineq_ub = bineq_function();
        //     Vector<optimization::bineq_sz> bineq_lb = Vector<optimization::bineq_sz>::Constant(-OSQP_INFTY);
        // }

        // Chnage this to private after testing:
        public:
            mjModel* mj_model;
            mjData* mj_data;
            std::vector<std::string> sites;
            std::vector<std::string> bodies;
            std::vector<std::string> noncontact_sites;
            std::vector<std::string> contact_sites;
            std::vector<int> site_ids;
            std::vector<int> noncontact_site_ids;
            std::vector<int> contact_site_ids;
            std::vector<int> body_ids;
            Vector<optimization::design_vector_size> design_vector = Vector<optimization::design_vector_size>::Zero();

        private:
            // /* OSQP Solver, settings, and matrices */
            // OSQPSolver*   solver   = NULL;
            // OSQPSettings* settings = NULL;
            // OSQPCscMatrix* P = malloc(sizeof(OSQPCscMatrix));
            // OSQPCscMatrix* A = malloc(sizeof(OSQPCscMatrix));
};
